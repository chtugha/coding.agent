use std::fmt::Write as FmtWrite;
use std::time::Duration;

use anyhow::Result;
use serde::{Deserialize, Serialize};

use crate::config::{LlmProfile, SharedConfig};
use crate::prompts;

const MAX_RETRIEVAL_CONTEXT_CHARS: usize = 50_000;

#[derive(Debug, Clone, Deserialize)]
pub struct RetrievalRequest {
    pub context: String,
    #[serde(default)]
    pub history: Vec<HistoryEntry>,
    #[serde(default)]
    pub active_profile_id: Option<String>,
    #[serde(default = "crate::config::default_timeout")]
    pub timeout_secs: f64,
    #[serde(default = "crate::config::default_max_tokens")]
    pub max_tokens: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HistoryEntry {
    pub num_turns: usize,
    pub ref_text: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct RetrievalResponse {
    pub reference_text: String,
    pub lm_label: String,
    pub num_turns: usize,
}

#[derive(Clone)]
struct Turn {
    role: &'static str,
    text: String,
}

fn filter_printable(s: &str) -> String {
    s.chars().filter(|c| !c.is_control()).collect::<String>().trim().to_string()
}

pub fn process_reference_context(
    conversation_context: &str,
    history: &[HistoryEntry],
) -> (String, usize) {
    const BLOCK_OPEN: &str = "[Retrieved medical context]\n";
    const BLOCK_END_MARKER: &str = "[End of retrieved context]\n\n";

    let (retrieved_prefix, body) = if conversation_context.starts_with(BLOCK_OPEN) {
        let search_from = BLOCK_OPEN.len();
        if let Some(rel_pos) = conversation_context[search_from..].find(BLOCK_END_MARKER) {
            let end_pos = search_from + rel_pos;
            let after = end_pos + BLOCK_END_MARKER.len();
            (&conversation_context[..after], &conversation_context[after..])
        } else {
            ("", conversation_context)
        }
    } else {
        ("", conversation_context)
    };

    let mut turns: Vec<Turn> = Vec::new();
    for turn in body.split('\n') {
        if let Some(rest) = turn.strip_prefix("user:") {
            turns.push(Turn { role: "Human", text: filter_printable(rest) });
        } else if let Some(rest) = turn.strip_prefix("moshi:") {
            turns.push(Turn { role: "moshi", text: filter_printable(rest) });
        }
    }

    if !turns.is_empty() && turns.last().is_some_and(|t| t.role == "moshi") {
        turns.pop();
    }
    if !turns.is_empty() && turns.first().is_some_and(|t| t.role == "moshi") {
        turns.remove(0);
    }

    let num_turns = turns.iter().filter(|t| t.role == "Human").count();

    let mut sorted_history: Vec<&HistoryEntry> = history.iter().collect();
    sorted_history.sort_by_key(|h| h.num_turns);

    let mut out = String::new();
    out.push_str(retrieved_prefix);

    let mut hist_idx = 0;
    let mut user_turn_count: usize = 0;
    for t in &turns {
        if t.role != "Human" {
            while hist_idx < sorted_history.len()
                && sorted_history[hist_idx].num_turns <= user_turn_count
            {
                writeln!(out, "Reference: {}", sorted_history[hist_idx].ref_text).unwrap();
                hist_idx += 1;
            }
        }
        if !t.text.is_empty() {
            writeln!(out, "{}: {}", t.role, t.text).unwrap();
        } else {
            writeln!(out, "{}:", t.role).unwrap();
        }
        if t.role == "Human" {
            user_turn_count += 1;
        }
    }

    while hist_idx < sorted_history.len() {
        writeln!(out, "Reference: {}", sorted_history[hist_idx].ref_text).unwrap();
        hist_idx += 1;
    }

    out.push_str("Reference:");
    (out, num_turns)
}

#[derive(Debug, Deserialize)]
struct ChatCompletionMessage {
    #[allow(dead_code)]
    pub role: String,
    pub content: String,
    #[serde(default)]
    pub reasoning: Option<String>,
}

#[derive(Debug, Deserialize)]
struct ChatCompletionChoice {
    #[allow(dead_code)]
    pub index: u32,
    pub message: ChatCompletionMessage,
}

#[derive(Debug, Deserialize)]
struct ChatCompletionResponse {
    #[allow(dead_code)]
    pub id: String,
    pub choices: Vec<ChatCompletionChoice>,
}

#[allow(clippy::too_many_arguments)]
async fn fetch_reference_http(
    http_client: &reqwest::Client,
    base_url: &str,
    model_name: &str,
    api_key: &str,
    processed_context: &str,
    prompt_template: &str,
    timeout_secs: f64,
    max_tokens: usize,
) -> Result<(String, String)> {
    let user_content = format!("{prompt_template}{processed_context}");

    let request_body = serde_json::json!({
        "model": model_name,
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": user_content}
        ],
        "max_tokens": max_tokens,
        "temperature": 1.0,
        "stop": ["\n"],
        "reasoning_effort": "low"
    });

    tracing::info!(
        model = model_name,
        content_len = user_content.len(),
        "retrieval: calling LLM API"
    );

    let start = std::time::Instant::now();

    let mut request = http_client
        .post(format!("{}/chat/completions", base_url.trim_end_matches('/')))
        .header("Authorization", format!("Bearer {api_key}"))
        .json(&request_body);

    if timeout_secs > 0.0 {
        request = request.timeout(Duration::from_secs_f64(timeout_secs));
    }

    let response = request.send().await.map_err(|e| {
        if e.is_timeout() {
            anyhow::anyhow!("LLM request timed out after {timeout_secs:.1}s")
        } else {
            anyhow::anyhow!("LLM request failed: {e}")
        }
    })?;

    let elapsed = start.elapsed();

    if !response.status().is_success() {
        let status = response.status();
        let error_text = response.text().await.unwrap_or_else(|_| "Unknown error".to_string());
        anyhow::bail!("LLM API returned error {status} - {error_text}");
    }

    let response_text = response.text().await?;
    let parsed: ChatCompletionResponse = serde_json::from_str(&response_text)?;

    tracing::info!(elapsed_secs = elapsed.as_secs_f64(), "retrieval: LLM API call completed");

    let content = parsed
        .choices
        .first()
        .map(|choice| {
            if let Some(ref reasoning) = choice.message.reasoning {
                tracing::info!(reasoning = %reasoning, "retrieval: LLM reasoning");
            }
            choice.message.content.clone()
        })
        .unwrap_or_default();

    Ok((model_name.to_string(), content))
}

fn reference_response_nonempty(content: &str) -> bool {
    !content.trim().is_empty()
}

fn resolve_profile_credentials(profile: &LlmProfile) -> (String, String, String) {
    let api_key = profile
        .api_key_env
        .as_deref()
        .and_then(|env_name| std::env::var(env_name).ok())
        .unwrap_or_else(|| std::env::var("LLM_API_KEY").unwrap_or_default());
    (profile.base_url.clone(), profile.model.clone(), api_key)
}

pub async fn handle_retrieval(
    config: &SharedConfig,
    http_client: &reqwest::Client,
    request: RetrievalRequest,
) -> Result<RetrievalResponse> {
    let ctx_len = request.context.chars().count();
    if ctx_len > MAX_RETRIEVAL_CONTEXT_CHARS {
        anyhow::bail!(
            "context too large: {ctx_len} chars (max {MAX_RETRIEVAL_CONTEXT_CHARS})"
        );
    }

    let (profiles, default_timeout, default_max_tokens) = {
        let cfg = config.read();
        if !cfg.llm_mode_enabled {
            anyhow::bail!("LLM mode is not enabled");
        }
        (cfg.llm_profiles.clone(), cfg.default_timeout_secs, cfg.default_max_tokens)
    };

    if profiles.is_empty() {
        let base_url = std::env::var("LLM_BASE_URL")
            .map_err(|_| anyhow::anyhow!("no llm_profiles configured and LLM_BASE_URL not set"))?;
        let model_name = std::env::var("LLM_MODEL_NAME").map_err(|_| {
            anyhow::anyhow!("no llm_profiles configured and LLM_MODEL_NAME not set")
        })?;
        let api_key = std::env::var("LLM_API_KEY").unwrap_or_default();

        let timeout = if request.timeout_secs > 0.0 { request.timeout_secs } else { default_timeout };
        let max_tokens = if request.max_tokens > 0 { request.max_tokens } else { default_max_tokens };

        let (processed, num_turns) =
            process_reference_context(&request.context, &request.history);
        let prompt = prompts::bundled_prompt_for_style("simplified");

        let (lm_label, reference_text) = fetch_reference_http(
            http_client,
            &base_url,
            &model_name,
            &api_key,
            &processed,
            prompt,
            timeout,
            max_tokens,
        )
        .await?;

        return Ok(RetrievalResponse { reference_text, lm_label, num_turns });
    }

    let timeout = if request.timeout_secs > 0.0 { request.timeout_secs } else { default_timeout };
    let max_tokens = if request.max_tokens > 0 { request.max_tokens } else { default_max_tokens };

    let (processed, num_turns) =
        process_reference_context(&request.context, &request.history);

    let default_idx = profiles
        .iter()
        .position(|p| p.is_default)
        .unwrap_or(0);

    let active_idx = request
        .active_profile_id
        .as_deref()
        .and_then(|id| profiles.iter().position(|p| p.id == id))
        .unwrap_or(default_idx);

    if active_idx == default_idx || profiles.len() < 2 {
        let profile = &profiles[active_idx];
        let (base_url, model_name, api_key) = resolve_profile_credentials(profile);
        let prompt = prompts::bundled_prompt_for_style(&profile.prompt_style);

        let (lm_label, reference_text) = fetch_reference_http(
            http_client,
            &base_url,
            &model_name,
            &api_key,
            &processed,
            prompt,
            timeout,
            max_tokens,
        )
        .await?;

        return Ok(RetrievalResponse { reference_text, lm_label, num_turns });
    }

    let active_profile = &profiles[active_idx];
    let default_profile = &profiles[default_idx];

    let (bu_a, mn_a, ak_a) = resolve_profile_credentials(active_profile);
    let (bu_d, mn_d, ak_d) = resolve_profile_credentials(default_profile);
    let prompt_a = prompts::bundled_prompt_for_style(&active_profile.prompt_style);
    let prompt_d = prompts::bundled_prompt_for_style(&default_profile.prompt_style);

    let (r_active, r_default) = tokio::join!(
        fetch_reference_http(
            http_client, &bu_a, &mn_a, &ak_a, &processed, prompt_a, timeout, max_tokens,
        ),
        fetch_reference_http(
            http_client, &bu_d, &mn_d, &ak_d, &processed, prompt_d, timeout, max_tokens,
        ),
    );

    if let Ok((ref label, ref text)) = r_active {
        if reference_response_nonempty(text) {
            return Ok(RetrievalResponse {
                reference_text: text.clone(),
                lm_label: label.clone(),
                num_turns,
            });
        }
    } else if let Err(ref e) = r_active {
        tracing::warn!("retrieval: active profile error: {e}");
    }

    match r_default {
        Ok((label, text)) => {
            if reference_response_nonempty(&text) {
                tracing::warn!(
                    "retrieval: active profile returned empty; using default profile response"
                );
            }
            Ok(RetrievalResponse { reference_text: text, lm_label: label, num_turns })
        }
        Err(e) => match r_active {
            Ok((label, text)) => {
                Ok(RetrievalResponse { reference_text: text, lm_label: label, num_turns })
            }
            Err(_) => Err(e),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn process_reference_context_no_retrieved_block() {
        let ctx = "user: hello\nmoshi: hi there\nuser: how are you";
        let (result, num_turns) = process_reference_context(ctx, &[]);
        assert!(result.starts_with("Human: hello\n"));
        assert!(result.contains("Human: how are you\n"));
        assert!(result.ends_with("Reference:"));
        assert!(!result.contains("[Retrieved medical context]"));
        assert_eq!(num_turns, 2);
    }

    #[test]
    fn process_reference_context_preserves_retrieved_block() {
        let ctx = "[Retrieved medical context]\n(1) Patient has diabetes.\n[End of retrieved context]\n\nuser: what medication?\nmoshi: let me check\nuser: please hurry";
        let (result, num_turns) = process_reference_context(ctx, &[]);
        assert!(result.starts_with("[Retrieved medical context]\n"));
        assert!(result.contains("(1) Patient has diabetes."));
        assert!(result.contains("[End of retrieved context]\n\n"));
        assert!(result.contains("Human: what medication?"));
        assert!(result.contains("Human: please hurry"));
        assert!(result.ends_with("Reference:"));
        assert_eq!(num_turns, 2);
    }

    #[test]
    fn process_reference_context_retrieved_block_no_terminator_falls_back() {
        let ctx = "[Retrieved medical context]\n(1) chunk\nuser: hello";
        let (result, _) = process_reference_context(ctx, &[]);
        assert!(result.ends_with("Reference:"));
        assert!(
            !result.contains("[Retrieved medical context]"),
            "no terminator: block should not be extracted"
        );
    }

    #[test]
    fn process_reference_context_with_history() {
        let ctx = "user: hello\nmoshi: hi\nuser: help me\nmoshi: sure";
        let history = vec![HistoryEntry { num_turns: 1, ref_text: "Previous reference".into() }];
        let (result, num_turns) = process_reference_context(ctx, &history);
        assert!(result.contains("Reference: Previous reference\n"));
        assert!(result.contains("Human: hello\n"));
        assert!(result.contains("Human: help me\n"));
        assert!(result.ends_with("Reference:"));
        assert_eq!(num_turns, 2);
    }

    #[test]
    fn process_reference_context_history_at_end() {
        let ctx = "user: hello";
        let history = vec![HistoryEntry { num_turns: 99, ref_text: "Late reference".into() }];
        let (result, _) = process_reference_context(ctx, &history);
        assert!(result.contains("Human: hello\n"));
        assert!(result.contains("Reference: Late reference\n"));
        assert!(result.ends_with("Reference:"));
    }

    #[test]
    fn process_reference_context_strips_leading_trailing_moshi() {
        let ctx = "moshi: greeting\nuser: hello\nmoshi: bye";
        let (result, num_turns) = process_reference_context(ctx, &[]);
        assert!(!result.contains("moshi: greeting"));
        assert!(!result.contains("moshi: bye"));
        assert!(result.contains("Human: hello"));
        assert_eq!(num_turns, 1);
    }

    #[test]
    fn process_reference_context_history_inserted_before_moshi_not_before_user() {
        let ctx = "user: a\nmoshi: b\nuser: c\nmoshi: d\nuser: e";
        let history = vec![HistoryEntry { num_turns: 2, ref_text: "Ref after turn 2".into() }];
        let (result, num_turns) = process_reference_context(ctx, &history);
        assert_eq!(num_turns, 3);

        let ref_pos = result.find("Reference: Ref after turn 2\n").expect("reference not found");
        let user_c_pos = result.find("Human: c\n").expect("Human: c not found");
        let moshi_d_pos = result.find("moshi: d\n").expect("moshi: d not found");

        assert!(
            user_c_pos < ref_pos,
            "Reference must come AFTER 'Human: c' (2nd user turn), not before it"
        );
        assert!(
            ref_pos < moshi_d_pos,
            "Reference must come BEFORE 'moshi: d' (moshi response to 2nd user turn)"
        );
    }
}
