// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

use std::collections::HashMap;
use std::error::Error;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use serde::Deserialize;

const REFERENCE_PROMPT_TEMPLATE_ORIGINAL: &str =
    include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/prompts/reference_prompt_template.txt"));
const REFERENCE_PROMPT_TEMPLATE_SIMPLIFIED: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/prompts/reference_prompt_template_simplified.txt"
));

fn bundled_prompt_for_style(t: crate::rag_retrieval::PromptStyle) -> &'static str {
    match t {
        crate::rag_retrieval::PromptStyle::Original => REFERENCE_PROMPT_TEMPLATE_ORIGINAL,
        crate::rag_retrieval::PromptStyle::Simplified => REFERENCE_PROMPT_TEMPLATE_SIMPLIFIED,
    }
}

/// Rust backend does not yet replay prior `Reference:` lines into the prompt.
fn process_reference_context(conversation_context: &str) -> String {
    #[derive(Clone)]
    struct Turn {
        role: &'static str,
        text: String,
    }

    fn filter_printable(s: &str) -> String {
        s.chars().filter(|c| !c.is_control()).collect::<String>().trim().to_string()
    }

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
            let text = filter_printable(rest);
            turns.push(Turn { role: "Human", text });
        } else if let Some(rest) = turn.strip_prefix("moshi:") {
            let text = filter_printable(rest);
            turns.push(Turn { role: "moshi", text });
        }
    }

    if !turns.is_empty() && turns.last().is_some_and(|t| t.role == "moshi") {
        turns.pop();
    }
    if !turns.is_empty() && turns.first().is_some_and(|t| t.role == "moshi") {
        turns.remove(0);
    }

    let mut out = String::new();
    out.push_str(retrieved_prefix);
    for t in &turns {
        if !t.text.is_empty() {
            out.push_str(&format!("{}: {}\n", t.role, t.text));
        } else {
            out.push_str(&format!("{}:\n", t.role));
        }
    }
    out.push_str("Reference:");
    out
}

/// Custom ChatCompletionMessage to handle Groq's additional fields (reasoning, executed_tools)
#[derive(Debug, Deserialize)]
struct CustomChatCompletionMessage {
    #[allow(dead_code)]
    pub role: String,
    pub content: String,
    #[serde(default)]
    pub reasoning: Option<String>,
    #[serde(default)]
    pub executed_tools: Option<Vec<ExecutedTool>>,
}

#[derive(Debug, Deserialize)]
struct ExecutedTool {
    #[allow(dead_code)]
    pub index: u32,
    #[serde(rename = "type")]
    #[allow(dead_code)]
    pub tool_type: String,
    #[allow(dead_code)]
    pub arguments: String,
    #[allow(dead_code)]
    pub output: String,
}

/// Custom ChatCompletionChoice to use our message type
#[derive(Debug, Deserialize)]
struct CustomChatCompletionChoice {
    #[allow(dead_code)]
    pub index: u32,
    pub message: CustomChatCompletionMessage,
    #[serde(default)]
    #[allow(dead_code)]
    pub logprobs: Option<serde_json::Value>,
    #[serde(default)]
    #[allow(dead_code)]
    pub finish_reason: Option<String>,
}

/// Custom ChatCompletionResponse that skips deserializing unsupported service_tier
#[derive(Debug, Deserialize)]
struct CustomChatCompletionResponse {
    #[allow(dead_code)]
    pub id: String,
    #[allow(dead_code)]
    pub object: String,
    #[allow(dead_code)]
    pub created: u64,
    #[allow(dead_code)]
    pub model: String,
    pub choices: Vec<CustomChatCompletionChoice>,
    #[allow(dead_code)]
    pub usage: serde_json::Value,
    #[serde(default)]
    #[allow(dead_code)]
    pub x_groq: Option<serde_json::Value>,
    #[serde(skip_deserializing)]
    #[allow(dead_code)]
    pub service_tier: Option<String>,
}

/// Per-slot state for an in-flight RAG request.
struct SlotTask {
    result_rx: Receiver<String>,
    join_handle: JoinHandle<()>,
    cancel: Arc<AtomicBool>,
}

/// Manages background RAG reference text generation: wait N seconds (wall-clock), then call OpenAI API, send result to channel.
/// Supports multiple in-flight requests keyed by slot (batch index) for batched inference.
pub struct RagManager {
    retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
    two_step_client: Option<Arc<crate::rag_two_step::TwoStepRagClient>>,
    llm_client: reqwest::Client,
    /// Single-slot mode: one in-flight task (slot_id = 0).
    single_result_rx: Mutex<Option<Receiver<String>>>,
    single_join_handle: Mutex<Option<JoinHandle<()>>>,
    single_cancel: Arc<AtomicBool>,
    /// Batched mode: per-slot in-flight tasks.
    batched_tasks: Mutex<HashMap<usize, SlotTask>>,
}

impl RagManager {
    pub fn new(retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>) -> Self {
        Self {
            retrieval,
            two_step_client: None,
            llm_client: reqwest::Client::new(),
            single_result_rx: Mutex::new(None),
            single_join_handle: Mutex::new(None),
            single_cancel: Arc::new(AtomicBool::new(false)),
            batched_tasks: Mutex::new(HashMap::new()),
        }
    }

    pub fn new_with_two_step(
        retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
        rag_timeout: f32,
    ) -> Self {
        Self {
            retrieval,
            two_step_client: Some(crate::rag_two_step::new_client(rag_timeout)),
            llm_client: reqwest::Client::new(),
            single_result_rx: Mutex::new(None),
            single_join_handle: Mutex::new(None),
            single_cancel: Arc::new(AtomicBool::new(false)),
            batched_tasks: Mutex::new(HashMap::new()),
        }
    }

    /// Shared worker: wait `wait_secs`, then fetch RAG reference text from OpenAI and send on `result_tx`.
    /// If `cancel` is set or send fails, sends empty string. `slot_id` is only for logging.
    /// `rag_timeout` is applied to the HTTP client (`0` = no timeout).
    fn spawn_rag_worker<F>(
        wait_secs: f64,
        rag_timeout: f32,
        context_provider: F,
        cancel: Arc<AtomicBool>,
        result_tx: Sender<String>,
        slot_id: Option<usize>,
        retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
        two_step_client: Option<Arc<crate::rag_two_step::TwoStepRagClient>>,
        patient_id: Option<i64>,
        llm_client: reqwest::Client,
    ) -> JoinHandle<()>
    where
        F: FnOnce() -> String + Send + 'static,
    {
        thread::spawn(move || {
            let deadline = Instant::now()
                .checked_add(Duration::from_secs_f64(wait_secs.max(0.0)))
                .unwrap_or_else(Instant::now);
            while Instant::now() < deadline {
                if cancel.load(Ordering::SeqCst) {
                    let _ = result_tx.send(String::new());
                    return;
                }
                let remaining = deadline.saturating_duration_since(Instant::now());
                let sleep_dur = remaining.min(Duration::from_millis(50));
                thread::sleep(sleep_dur);
            }
            if cancel.load(Ordering::SeqCst) {
                let _ = result_tx.send(String::new());
                return;
            }

            let context = context_provider();
            let reference_text = match Self::fetch_reference_text(
                &context,
                slot_id,
                rag_timeout,
                retrieval.as_ref(),
                two_step_client.as_deref(),
                patient_id,
                &llm_client,
            ) {
                Ok(t) => t,
                Err(e) => {
                    if let Some(sid) = slot_id {
                        tracing::warn!("[RET_FAILED] RAG API error for slot {}: {}", sid, e);
                    } else {
                        tracing::warn!("[RET_FAILED] RAG API error: {}", e);
                    }
                    String::new()
                }
            };
            let _ = result_tx.send(reference_text);
        })
    }

    /// Trigger background generation (single-slot mode). Spawns a thread that waits approximately
    /// `wait_secs` seconds in wall-clock time (while honoring cancellation), then gets context
    /// from `context_provider`, calls the OpenAI API, and sends the reference text (or empty
    /// on error/timeout) to the internal result channel.
    pub fn trigger_background_generation<F>(
        &self,
        wait_secs: f64,
        rag_timeout: f32,
        context_provider: F,
    ) where
        F: FnOnce() -> String + Send + 'static,
    {
        self.trigger_background_generation_with_patient(wait_secs, rag_timeout, context_provider, None)
    }

    pub fn trigger_background_generation_with_patient<F>(
        &self,
        wait_secs: f64,
        rag_timeout: f32,
        context_provider: F,
        patient_id: Option<i64>,
    ) where
        F: FnOnce() -> String + Send + 'static,
    {
        self.cancel_pending();

        let (result_tx, result_rx) = std::sync::mpsc::channel();
        *self.single_result_rx.lock().unwrap() = Some(result_rx);

        let handle = Self::spawn_rag_worker(
            wait_secs,
            rag_timeout,
            context_provider,
            Arc::clone(&self.single_cancel),
            result_tx,
            None,
            Arc::clone(&self.retrieval),
            self.two_step_client.clone(),
            patient_id,
            self.llm_client.clone(),
        );
        *self.single_join_handle.lock().unwrap() = Some(handle);
    }

    /// Trigger background generation for a specific slot (batched mode). Cancels any existing task for that slot.
    pub fn trigger_background_generation_slot<F>(
        &self,
        slot_id: usize,
        wait_secs: f64,
        rag_timeout: f32,
        context_provider: F,
    ) where
        F: FnOnce() -> String + Send + 'static,
    {
        self.trigger_background_generation_slot_with_patient(slot_id, wait_secs, rag_timeout, context_provider, None)
    }

    pub fn trigger_background_generation_slot_with_patient<F>(
        &self,
        slot_id: usize,
        wait_secs: f64,
        rag_timeout: f32,
        context_provider: F,
        patient_id: Option<i64>,
    ) where
        F: FnOnce() -> String + Send + 'static,
    {
        self.cancel_pending_slot(slot_id);

        let (result_tx, result_rx) = std::sync::mpsc::channel();
        let cancel = Arc::new(AtomicBool::new(false));

        let handle = Self::spawn_rag_worker(
            wait_secs,
            rag_timeout,
            context_provider,
            Arc::clone(&cancel),
            result_tx,
            Some(slot_id),
            Arc::clone(&self.retrieval),
            self.two_step_client.clone(),
            patient_id,
            self.llm_client.clone(),
        );

        let mut tasks = self.batched_tasks.lock().unwrap();
        tasks.insert(slot_id, SlotTask { result_rx, join_handle: handle, cancel });
    }

    fn reference_response_nonempty(prefixed: &str) -> bool {
        prefixed.split_once('\t').is_some_and(|(_, body)| !body.trim().is_empty())
    }

    /// One OpenAI-compatible reference call. Returns `model_name\tcontent` for the client.
    async fn fetch_reference_http(
        http_client: &reqwest::Client,
        base_url: String,
        model_name: String,
        api_key: String,
        context: String,
        rag_timeout: f32,
        bundled_prompt: &'static str,
    ) -> Result<String, Box<dyn Error + Send + Sync>> {
        let system_prompt = "You are a helpful assistant.";
        let processed = process_reference_context(&context);
        let user_content = format!("{bundled_prompt}{processed}");

        let request_body = serde_json::json!({
            "model": model_name,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_content}
            ],
            "max_tokens": 256,
            "temperature": 1.0,
            "stop": ["\n"],
            "reasoning_effort": "low"
        });

        tracing::info!("RAG: Calling LLM API '{}' content_len={}", model_name, user_content.len());

        let start = std::time::Instant::now();

        let mut request = http_client
            .post(format!("{}/chat/completions", base_url.trim_end_matches('/')))
            .header("Authorization", format!("Bearer {}", api_key))
            .json(&request_body);
        if rag_timeout > 0.0 {
            request = request.timeout(Duration::from_secs_f64(rag_timeout as f64));
        }
        let response = request
            .send()
            .await
            .map_err(|e| -> Box<dyn Error + Send + Sync> {
                if e.is_timeout() {
                    format!("LLM request timed out after {:.1}s (rag_timeout)", rag_timeout).into()
                } else {
                    e.to_string().into()
                }
            })?;

        let elapsed = start.elapsed();

        if !response.status().is_success() {
            let status = response.status();
            let error_text = response.text().await.unwrap_or_else(|_| "Unknown error".to_string());
            return Err(format!("LLM API returned error {} - {}", status, error_text).into());
        }

        let response_text = response.text().await?;
        let custom_response: CustomChatCompletionResponse = serde_json::from_str(&response_text)?;

        tracing::info!("RAG: LLM API call completed in {:.3}s", elapsed.as_secs_f64());

        let content = custom_response
            .choices
            .first()
            .map(|choice| {
                if let Some(reasoning) = &choice.message.reasoning {
                    tracing::info!("RAG: LLM reasoning: {}", reasoning);
                }
                if let Some(tools) = &choice.message.executed_tools {
                    tracing::debug!("RAG: LLM executed {} tool(s)", tools.len());
                }
                choice.message.content.clone()
            })
            .unwrap_or_default();
        Ok(format!("{model_name}\t{content}"))
    }

    fn fetch_reference_text(
        context: &str,
        slot_id: Option<usize>,
        rag_timeout: f32,
        retrieval: &crate::rag_retrieval::RagRetrievalEndpoints,
        two_step_client: Option<&crate::rag_two_step::TwoStepRagClient>,
        patient_id: Option<i64>,
        llm_client: &reqwest::Client,
    ) -> Result<String, Box<dyn Error + Send + Sync>> {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()?;

        let llm_client = llm_client.clone();

        let step_a_start = Instant::now();

        let ctx = if let Some(client) = two_step_client {
            let plain_ctx = context.to_string();
            let pid = patient_id;
            rt.block_on(client.retrieve_and_augment_context(&plain_ctx, pid))
        } else {
            context.to_string()
        };

        let step_b_timeout = if rag_timeout > 0.0 {
            let elapsed = step_a_start.elapsed();
            let total = Duration::from_secs_f64(rag_timeout as f64);
            let remaining = total.saturating_sub(elapsed);
            if remaining.is_zero() {
                tracing::warn!(
                    slot_id = ?slot_id,
                    "RAG timeout budget ({:.1}s) exhausted after Step A; skipping LLM call",
                    rag_timeout,
                );
                return Ok(String::new());
            }
            remaining.as_secs_f64() as f32
        } else {
            0.0
        };

        let sid = slot_id.unwrap_or(0);

        if retrieval.len() < 2 {
            let (base_url, model_name, api_key) =
                retrieval
                    .resolve_llm_credentials_for_slot(sid)
                    .map_err(|e| -> Box<dyn Error + Send + Sync> { e.into() })?;
            let idx = if retrieval.len() == 1 { 0 } else { retrieval.active_index_for_slot(sid) };
            let bundled = bundled_prompt_for_style(retrieval.prompt_style_at(idx));
            return rt.block_on(Self::fetch_reference_http(
                &llm_client,
                base_url,
                model_name,
                api_key,
                ctx,
                step_b_timeout,
                bundled,
            ));
        }

        let ai = retrieval.active_index_for_slot(sid);
        let di = retrieval.default_index();
        if ai == di {
            let (base_url, model_name, api_key) = retrieval
                .credentials_at(ai)
                .map_err(|e| -> Box<dyn Error + Send + Sync> { e.into() })?;
            let bundled = bundled_prompt_for_style(retrieval.prompt_style_at(ai));
            return rt.block_on(Self::fetch_reference_http(
                &llm_client,
                base_url,
                model_name,
                api_key,
                ctx,
                step_b_timeout,
                bundled,
            ));
        }

        let (bu_a, mn_a, ak_a) = retrieval
            .credentials_at(ai)
            .map_err(|e| -> Box<dyn Error + Send + Sync> { e.into() })?;
        let (bu_b, mn_b, ak_b) = retrieval
            .credentials_at(di)
            .map_err(|e| -> Box<dyn Error + Send + Sync> { e.into() })?;
        let bundled_a = bundled_prompt_for_style(retrieval.prompt_style_at(ai));
        let bundled_b = bundled_prompt_for_style(retrieval.prompt_style_at(di));

        rt.block_on(async move {
            let (r_active, r_default) = tokio::join!(
                Self::fetch_reference_http(&llm_client, bu_a, mn_a, ak_a, ctx.clone(), step_b_timeout, bundled_a,),
                Self::fetch_reference_http(&llm_client, bu_b, mn_b, ak_b, ctx, step_b_timeout, bundled_b,),
            );

            if let Ok(ref s) = r_active {
                if Self::reference_response_nonempty(s) {
                    return Ok(s.clone());
                }
            } else if let Err(ref e) = r_active {
                tracing::warn!("RAG: active retrieval error: {}", e);
            }

            match r_default {
                Ok(s) => {
                    if Self::reference_response_nonempty(&s) {
                        tracing::warn!(
                            "RAG: active retrieval returned empty; using default profile response"
                        );
                    }
                    Ok(s)
                }
                Err(e) => match r_active {
                    Ok(s) => Ok(s),
                    Err(_) => Err(e),
                },
            }
        })
    }

    /// Try to receive a reference text result (single-slot mode). Non-blocking.
    pub fn try_recv_result(&self) -> Option<String> {
        if let Ok(guard) = self.single_result_rx.lock() {
            guard.as_ref().and_then(|rx| rx.try_recv().ok())
        } else {
            None
        }
    }

    /// Try to receive a reference text result from any slot (batched mode). Non-blocking.
    /// Returns (slot_id, reference_text) for the first completed task.
    pub fn try_recv_result_slot(&self) -> Option<(usize, String)> {
        let mut tasks = self.batched_tasks.lock().unwrap();
        let mut done_slot = None;
        for (&slot_id, task) in tasks.iter() {
            if let Ok(text) = task.result_rx.try_recv() {
                done_slot = Some((slot_id, text));
                break;
            }
        }
        if let Some((slot_id, text)) = done_slot {
            if let Some(task) = tasks.remove(&slot_id) {
                let _ = task.join_handle.join();
            }
            Some((slot_id, text))
        } else {
            None
        }
    }

    /// Cancel any pending RAG task for a slot (batched mode).
    pub fn cancel_pending_slot(&self, slot_id: usize) {
        let mut tasks = self.batched_tasks.lock().unwrap();
        if let Some(task) = tasks.remove(&slot_id) {
            task.cancel.store(true, Ordering::SeqCst);
        }
    }

    /// Cancel any pending RAG task. Signals cancel and joins the worker thread.
    pub fn cancel_pending(&self) {
        self.single_cancel.store(true, Ordering::SeqCst);
        *self.single_result_rx.lock().unwrap() = None;
        if let Ok(mut h) = self.single_join_handle.lock() {
            if let Some(handle) = h.take() {
                let _ = handle.join();
            }
        }
        self.single_cancel.store(false, Ordering::SeqCst);
        let mut tasks = self.batched_tasks.lock().unwrap();
        for (_, task) in tasks.drain() {
            task.cancel.store(true, Ordering::SeqCst);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn process_reference_context_no_retrieved_block() {
        let ctx = "user: hello\nmoshi: hi there\nuser: how are you";
        let result = process_reference_context(ctx);
        assert!(result.starts_with("Human: hello\n"));
        assert!(result.contains("Human: how are you\n"));
        assert!(result.ends_with("Reference:"));
        assert!(!result.contains("[Retrieved medical context]"));
    }

    #[test]
    fn process_reference_context_preserves_retrieved_block() {
        let ctx = "[Retrieved medical context]\n(1) Patient has diabetes.\n[End of retrieved context]\n\nuser: what medication?\nmoshi: let me check\nuser: please hurry";
        let result = process_reference_context(ctx);
        assert!(result.starts_with("[Retrieved medical context]\n"));
        assert!(result.contains("(1) Patient has diabetes."));
        assert!(result.contains("[End of retrieved context]\n\n"));
        assert!(result.contains("Human: what medication?"));
        assert!(result.contains("Human: please hurry"));
        assert!(result.ends_with("Reference:"));
    }

    #[test]
    fn process_reference_context_retrieved_block_no_terminator_falls_back() {
        let ctx = "[Retrieved medical context]\n(1) chunk\nuser: hello";
        let result = process_reference_context(ctx);
        assert!(result.ends_with("Reference:"));
        assert!(!result.contains("[Retrieved medical context]"), "no terminator: block should not be extracted (body is whole string, no user: lines found inside block)");
    }
}
