use anyhow::Result;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::config::SharedConfig;
use crate::text_stream::{ActionRequest, OutgoingMessage};
use crate::tomedo_client::{self, TomedoClient};

pub struct ActionDispatcher {
    config: SharedConfig,
    tomedo: Option<Arc<TomedoClient>>,
    http: reqwest::Client,
}

#[derive(Debug, Clone)]
pub struct ActionResult {
    pub text: String,
    pub action_type: String,
}

impl ActionDispatcher {
    pub fn new(config: SharedConfig, tomedo: Option<Arc<TomedoClient>>) -> Self {
        Self { config, tomedo, http: reqwest::Client::new() }
    }

    pub async fn dispatch(&self, req: &ActionRequest) -> Result<ActionResult> {
        match req.action_type.as_str() {
            "tomedo-crawl-query" => self.handle_tomedo_query(req).await,
            "webhook" => self.handle_webhook(req).await,
            "calendar" => self.handle_calendar(req).await,
            "script" => self.handle_script(req).await,
            "llm-retrieval" => self.handle_llm_retrieval(req),
            "retrieval_and_webhook" => self.handle_retrieval_and_webhook(req).await,
            other => {
                tracing::warn!(action_type = other, "unknown action type");
                Ok(ActionResult {
                    text: format!("unknown action type: {other}"),
                    action_type: other.to_string(),
                })
            }
        }
    }

    async fn handle_tomedo_query(&self, req: &ActionRequest) -> Result<ActionResult> {
        let tomedo = self.tomedo.as_ref().ok_or_else(|| {
            anyhow::anyhow!("tomedo-crawl-query action but no tomedo_crawl_url configured")
        })?;

        tracing::info!(
            slot_id = req.slot_id,
            action_type = "tomedo-crawl-query",
            patient_id = ?req.patient_id,
            context_len = req.context_snippet.len(),
            "dispatching tomedo-crawl query"
        );

        let results = tomedo.query(&req.context_snippet, req.patient_id, 3).await?;
        let text = tomedo_client::format_query_results(&results);

        Ok(ActionResult { text, action_type: "tomedo-crawl-query".into() })
    }

    async fn handle_webhook(&self, req: &ActionRequest) -> Result<ActionResult> {
        let url = req
            .action_url
            .as_deref()
            .ok_or_else(|| anyhow::anyhow!("webhook action requires action_url"))?;

        validate_url_scheme(url)?;
        self.validate_webhook_host(url)?;

        tracing::info!(
            slot_id = req.slot_id,
            action_type = "webhook",
            url = url,
            "dispatching webhook"
        );

        let body = serde_json::json!({
            "slot_id": req.slot_id,
            "action_type": "webhook",
            "label": req.label,
            "context_snippet": req.context_snippet,
            "patient_id": req.patient_id,
        });

        let resp = self
            .http
            .post(url)
            .json(&body)
            .timeout(std::time::Duration::from_secs(10))
            .send()
            .await?;

        let status = resp.status();
        let resp_text = resp.text().await.unwrap_or_default();

        if !status.is_success() {
            tracing::warn!(url, %status, "webhook returned non-success");
        }

        Ok(ActionResult { text: resp_text, action_type: "webhook".into() })
    }

    async fn handle_calendar(&self, req: &ActionRequest) -> Result<ActionResult> {
        let url = req
            .action_url
            .as_deref()
            .ok_or_else(|| anyhow::anyhow!("calendar action requires action_url"))?;

        validate_url_scheme(url)?;
        self.validate_webhook_host(url)?;

        tracing::info!(
            slot_id = req.slot_id,
            action_type = "calendar",
            url = url,
            "dispatching calendar webhook"
        );

        let body = serde_json::json!({
            "slot_id": req.slot_id,
            "action_type": "calendar",
            "label": req.label,
            "context_snippet": req.context_snippet,
            "patient_id": req.patient_id,
        });

        let resp = self
            .http
            .post(url)
            .json(&body)
            .timeout(std::time::Duration::from_secs(10))
            .send()
            .await?;

        let status = resp.status();
        let resp_text = resp.text().await.unwrap_or_default();

        if !status.is_success() {
            tracing::warn!(url, %status, "calendar webhook returned non-success");
        }

        Ok(ActionResult { text: resp_text, action_type: "calendar".into() })
    }

    async fn handle_script(&self, req: &ActionRequest) -> Result<ActionResult> {
        let script_path_str = req
            .action_url
            .as_deref()
            .ok_or_else(|| anyhow::anyhow!("script action requires action_url (script path)"))?;

        let script_path = PathBuf::from(script_path_str);

        let allowed_dir = {
            let cfg = self.config.read();
            cfg.allowed_script_dir.clone()
        };

        if let Some(ref dir) = allowed_dir {
            validate_script_path(&script_path, dir)?;
        } else {
            anyhow::bail!("script execution blocked: allowed_script_dir not configured");
        }

        tracing::info!(
            slot_id = req.slot_id,
            action_type = "script",
            path = script_path_str,
            "dispatching script execution"
        );

        let mut child = tokio::process::Command::new(&script_path)
            .env("SLOT_ID", req.slot_id.to_string())
            .env("LABEL", &req.label)
            .env("CONTEXT_SNIPPET", &req.context_snippet)
            .env("PATIENT_ID", req.patient_id.map(|p| p.to_string()).unwrap_or_default())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()?;

        let child_stdout = child.stdout.take();
        let child_stderr = child.stderr.take();

        let stdout_task = tokio::spawn(async move {
            let mut buf = Vec::new();
            if let Some(mut out) = child_stdout {
                let _ = tokio::io::AsyncReadExt::read_to_end(&mut out, &mut buf).await;
            }
            buf
        });
        let stderr_task = tokio::spawn(async move {
            let mut buf = Vec::new();
            if let Some(mut err) = child_stderr {
                let _ = tokio::io::AsyncReadExt::read_to_end(&mut err, &mut buf).await;
            }
            buf
        });

        let exit_status = if let Ok(result) =
            tokio::time::timeout(std::time::Duration::from_secs(30), child.wait()).await
        {
            result?
        } else {
            let _ = child.kill().await;
            anyhow::bail!("script execution timed out after 30 seconds");
        };

        let stdout_buf = stdout_task.await.unwrap_or_default();
        let stderr_buf = stderr_task.await.unwrap_or_default();

        let stdout = String::from_utf8_lossy(&stdout_buf).to_string();
        let stderr = String::from_utf8_lossy(&stderr_buf).to_string();

        if !exit_status.success() {
            tracing::warn!(
                script = script_path_str,
                exit_code = ?exit_status.code(),
                stderr = %stderr,
                "script exited with non-zero status"
            );
        } else if !stderr.is_empty() {
            tracing::debug!(
                script = script_path_str,
                stderr = %stderr,
                "script produced stderr output on successful exit"
            );
        }

        Ok(ActionResult { text: stdout, action_type: "script".into() })
    }

    fn handle_llm_retrieval(&self, req: &ActionRequest) -> Result<ActionResult> {
        let cfg = self.config.read();
        if !cfg.llm_mode_enabled {
            anyhow::bail!("llm-retrieval action but llm_mode_enabled is false");
        }
        drop(cfg);

        tracing::info!(
            slot_id = req.slot_id,
            action_type = "llm-retrieval",
            "dispatching llm-retrieval (delegated to retrieval module in Step 7)"
        );

        Ok(ActionResult { text: String::new(), action_type: "llm-retrieval".into() })
    }

    async fn handle_retrieval_and_webhook(&self, req: &ActionRequest) -> Result<ActionResult> {
        let tomedo_req = ActionRequest {
            slot_id: req.slot_id,
            action_type: "tomedo-crawl-query".into(),
            action_url: None,
            context_snippet: req.context_snippet.clone(),
            patient_id: req.patient_id,
            inject_result: req.inject_result,
            label: req.label.clone(),
            arc_mode: req.arc_mode,
            response_tx: req.response_tx.clone(),
        };

        let webhook_req = ActionRequest {
            slot_id: req.slot_id,
            action_type: "webhook".into(),
            action_url: req.action_url.clone(),
            context_snippet: req.context_snippet.clone(),
            patient_id: req.patient_id,
            inject_result: false,
            label: req.label.clone(),
            arc_mode: req.arc_mode,
            response_tx: req.response_tx.clone(),
        };

        let (tomedo_result, webhook_result) =
            tokio::join!(self.handle_tomedo_query(&tomedo_req), self.handle_webhook(&webhook_req),);

        let mut combined_text = String::new();

        match tomedo_result {
            Ok(r) => combined_text.push_str(&r.text),
            Err(e) => tracing::warn!("retrieval_and_webhook: tomedo query failed: {e}"),
        }

        match webhook_result {
            Ok(r) => {
                if !combined_text.is_empty() && !r.text.is_empty() {
                    combined_text.push('\n');
                }
                combined_text.push_str(&r.text);
            }
            Err(e) => tracing::warn!("retrieval_and_webhook: webhook failed: {e}"),
        }

        Ok(ActionResult { text: combined_text, action_type: "retrieval_and_webhook".into() })
    }

    fn validate_webhook_host(&self, url: &str) -> Result<()> {
        let allowed_hosts = {
            let cfg = self.config.read();
            cfg.allowed_webhook_hosts.clone()
        };

        if allowed_hosts.is_empty() {
            return Ok(());
        }

        let parsed = url::Url::parse(url)
            .map_err(|e| anyhow::anyhow!("invalid webhook URL '{url}': {e}"))?;

        let host =
            parsed.host_str().ok_or_else(|| anyhow::anyhow!("webhook URL '{url}' has no host"))?;

        if allowed_hosts.iter().any(|h| h == host) {
            Ok(())
        } else {
            tracing::warn!(
                url = url,
                host = host,
                "webhook host not in allowed_webhook_hosts — request blocked"
            );
            anyhow::bail!("webhook host '{host}' not in allowed_webhook_hosts")
        }
    }
}

fn validate_url_scheme(url: &str) -> Result<()> {
    let parsed = url::Url::parse(url).map_err(|e| anyhow::anyhow!("invalid URL '{url}': {e}"))?;

    match parsed.scheme() {
        "http" | "https" => Ok(()),
        scheme => {
            tracing::error!(
                url = url,
                scheme = scheme,
                "rejected outbound request: only http/https schemes allowed"
            );
            anyhow::bail!("URL scheme '{scheme}' not allowed — only http and https are permitted")
        }
    }
}

fn validate_script_path(script_path: &Path, allowed_dir: &str) -> Result<()> {
    let allowed = PathBuf::from(allowed_dir);

    let canonical_allowed = allowed.canonicalize().map_err(|e| {
        anyhow::anyhow!("allowed_script_dir '{}' cannot be resolved: {e}", allowed_dir)
    })?;

    let canonical_script = script_path.canonicalize().map_err(|e| {
        anyhow::anyhow!("script path '{}' cannot be resolved: {e}", script_path.display())
    })?;

    if canonical_script.starts_with(&canonical_allowed) {
        Ok(())
    } else {
        tracing::error!(
            script = %script_path.display(),
            allowed_dir = allowed_dir,
            "script path outside allowed_script_dir — execution blocked"
        );
        anyhow::bail!(
            "script '{}' is outside allowed_script_dir '{}'",
            script_path.display(),
            allowed_dir
        )
    }
}

pub async fn run_action_loop(
    mut action_rx: tokio::sync::mpsc::Receiver<ActionRequest>,
    dispatcher: Arc<ActionDispatcher>,
) {
    while let Some(req) = action_rx.recv().await {
        let dispatcher = dispatcher.clone();
        tokio::spawn(async move {
            let slot_id = req.slot_id;
            let inject_result = req.inject_result;
            let arc_mode = req.arc_mode;
            let response_tx = req.response_tx.clone();
            let label = req.label.clone();
            let action_type = req.action_type.clone();

            match dispatcher.dispatch(&req).await {
                Ok(result) => {
                    tracing::info!(
                        slot_id,
                        action_type = %action_type,
                        label = %label,
                        result_len = result.text.len(),
                        "action completed"
                    );

                    if inject_result && !result.text.is_empty() {
                        let msg = if arc_mode {
                            OutgoingMessage::ArcInject { slot_id, reference_text: result.text }
                        } else {
                            OutgoingMessage::ReferenceInject { slot_id, text: result.text }
                        };
                        let _ = response_tx.send(msg).await;
                    }
                }
                Err(e) => {
                    tracing::error!(
                        slot_id,
                        action_type = %action_type,
                        label = %label,
                        error = %e,
                        "action dispatch failed"
                    );
                }
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validate_url_scheme_http_ok() {
        assert!(validate_url_scheme("http://example.com/hook").is_ok());
    }

    #[test]
    fn validate_url_scheme_https_ok() {
        assert!(validate_url_scheme("https://example.com/hook").is_ok());
    }

    #[test]
    fn validate_url_scheme_file_rejected() {
        let result = validate_url_scheme("file:///etc/passwd");
        assert!(result.is_err());
        let err_msg = result.unwrap_err().to_string();
        assert!(err_msg.contains("not allowed"), "got: {err_msg}");
    }

    #[test]
    fn validate_url_scheme_ftp_rejected() {
        let result = validate_url_scheme("ftp://evil.com/payload");
        assert!(result.is_err());
    }

    #[test]
    fn validate_url_scheme_javascript_rejected() {
        let result = validate_url_scheme("javascript:alert(1)");
        assert!(result.is_err());
    }

    #[test]
    fn validate_script_path_outside_dir() {
        let tmp = std::env::temp_dir();
        let allowed = tmp.join("moshi_scripts_test_allowed");
        let outside = tmp.join("moshi_scripts_test_outside");
        let _ = std::fs::create_dir_all(&allowed);
        let _ = std::fs::create_dir_all(&outside);
        let script = outside.join("evil.sh");
        std::fs::write(&script, "#!/bin/sh\necho evil").unwrap();

        let result = validate_script_path(&script, allowed.to_str().unwrap());
        assert!(result.is_err());

        let _ = std::fs::remove_dir_all(&allowed);
        let _ = std::fs::remove_dir_all(&outside);
    }
}
