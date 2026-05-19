use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::fmt::Write;

const MAX_QUERY_RESPONSE_BYTES: usize = 10 * 1024 * 1024;
const MAX_CALLER_ERROR_BYTES: usize = 16 * 1024;

#[derive(Debug, Clone)]
pub struct TomedoClient {
    http: reqwest::Client,
    base_url: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct QueryResult {
    pub text: String,
    #[serde(default)]
    pub score: f64,
    #[serde(default)]
    pub source: Option<String>,
}

#[derive(Debug, Deserialize)]
struct CallerPollResponse {
    #[serde(default)]
    status: String,
    #[serde(default)]
    patient_id: Option<i64>,
}

impl TomedoClient {
    pub fn new(base_url: &str) -> Self {
        Self { http: reqwest::Client::new(), base_url: base_url.trim_end_matches('/').to_string() }
    }

    pub async fn resolve_caller(&self, call_id: u64, phone: &str) -> Result<Option<i64>> {
        let url = format!("{}/caller", self.base_url);
        let body = serde_json::json!({
            "call_id": call_id,
            "phone": phone,
        });

        let resp = self
            .http
            .post(&url)
            .json(&body)
            .timeout(std::time::Duration::from_secs(2))
            .send()
            .await?;

        if !resp.status().is_success() {
            let status = resp.status();
            let body = resp.bytes().await.unwrap_or_default();
            let body_str = String::from_utf8_lossy(
                &body[..body.len().min(MAX_CALLER_ERROR_BYTES)],
            );
            tracing::warn!(
                call_id,
                phone,
                %status,
                body = %body_str,
                "tomedo-crawl POST /caller failed"
            );
            return Ok(None);
        }

        drop(resp);

        let poll_url = format!("{}/caller/{}", self.base_url, call_id);
        let max_attempts = 10;
        let interval = std::time::Duration::from_millis(50);

        for attempt in 0..max_attempts {
            tokio::time::sleep(interval).await;

            let poll_resp = match self
                .http
                .get(&poll_url)
                .timeout(std::time::Duration::from_secs(2))
                .send()
                .await
            {
                Ok(r) => r,
                Err(e) => {
                    tracing::debug!(attempt, "poll /caller/{call_id} error: {e}");
                    continue;
                }
            };

            if !poll_resp.status().is_success() {
                continue;
            }

            let parsed: CallerPollResponse = match poll_resp.json().await {
                Ok(p) => p,
                Err(e) => {
                    tracing::debug!(attempt, "poll /caller/{call_id} parse error: {e}");
                    continue;
                }
            };

            match parsed.status.as_str() {
                "found" => return Ok(parsed.patient_id),
                "not_found" => return Ok(None),
                _ => {}
            }
        }

        tracing::warn!(call_id, "tomedo-crawl caller poll timed out after {max_attempts} attempts");
        Ok(None)
    }

    pub async fn query(
        &self,
        text: &str,
        patient_id: Option<i64>,
        top_k: usize,
    ) -> Result<Vec<QueryResult>> {
        let url = format!("{}/query", self.base_url);
        let mut body = serde_json::json!({
            "text": text,
            "top_k": top_k,
        });
        if let Some(pid) = patient_id {
            body["patient_id"] = serde_json::Value::Number(pid.into());
        }

        let resp = self
            .http
            .post(&url)
            .json(&body)
            .timeout(std::time::Duration::from_secs(10))
            .send()
            .await?;

        if !resp.status().is_success() {
            let status = resp.status();
            let body = resp.bytes().await.unwrap_or_default();
            let body_text = String::from_utf8_lossy(&body[..body.len().min(MAX_CALLER_ERROR_BYTES)]);
            anyhow::bail!("tomedo-crawl POST /query returned {status}: {body_text}");
        }

        if let Some(cl) = resp.content_length() {
            if cl > MAX_QUERY_RESPONSE_BYTES as u64 {
                anyhow::bail!(
                    "tomedo-crawl POST /query response too large: content-length {cl} exceeds cap {}",
                    MAX_QUERY_RESPONSE_BYTES
                );
            }
        }

        let bytes = resp.bytes().await?;
        if bytes.len() > MAX_QUERY_RESPONSE_BYTES {
            anyhow::bail!(
                "tomedo-crawl POST /query response too large: {} bytes (max {})",
                bytes.len(),
                MAX_QUERY_RESPONSE_BYTES
            );
        }

        let results: Vec<QueryResult> = serde_json::from_slice(&bytes)?;
        Ok(results)
    }
}

pub fn format_query_results(results: &[QueryResult]) -> String {
    if results.is_empty() {
        return String::new();
    }
    let mut out = String::from("[Retrieved medical context]\n");
    for (i, r) in results.iter().enumerate() {
        if let Some(ref src) = r.source {
            let _ = writeln!(out, "({}) [{}] {}", i + 1, src, r.text);
        } else {
            let _ = writeln!(out, "({}) {}", i + 1, r.text);
        }
    }
    out.push_str("[End of retrieved medical context]");
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn format_empty_results() {
        assert_eq!(format_query_results(&[]), "");
    }

    #[test]
    fn format_single_result_no_source() {
        let results =
            vec![QueryResult { text: "Patient has diabetes.".into(), score: 0.95, source: None }];
        let formatted = format_query_results(&results);
        assert!(formatted.contains("[Retrieved medical context]"));
        assert!(formatted.contains("(1) Patient has diabetes."));
        assert!(formatted.contains("[End of retrieved medical context]"));
    }

    #[test]
    fn format_multiple_results_with_source() {
        let results = vec![
            QueryResult {
                text: "Blood pressure 140/90.".into(),
                score: 0.9,
                source: Some("vitals".into()),
            },
            QueryResult {
                text: "Prescribed metoprolol.".into(),
                score: 0.8,
                source: Some("medications".into()),
            },
        ];
        let formatted = format_query_results(&results);
        assert!(formatted.contains("(1) [vitals] Blood pressure 140/90."));
        assert!(formatted.contains("(2) [medications] Prescribed metoprolol."));
    }
}
