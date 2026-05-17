use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Receiver;
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use serde::Deserialize;

#[derive(Debug, Deserialize)]
struct BackendRetrievalResponse {
    #[serde(default)]
    reference_text: String,
    #[serde(default)]
    lm_label: String,
    #[serde(default)]
    #[allow(dead_code)]
    num_turns: usize,
}

struct SlotTask {
    result_rx: Receiver<String>,
    join_handle: JoinHandle<()>,
    cancel: Arc<AtomicBool>,
}

pub struct RagManager {
    backend_url: String,
    retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
    http_client: reqwest::blocking::Client,
    batched_tasks: Mutex<HashMap<usize, SlotTask>>,
}

impl RagManager {
    pub fn new(
        backend_url: String,
        retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
    ) -> Self {
        Self {
            backend_url,
            retrieval,
            http_client: reqwest::blocking::Client::new(),
            batched_tasks: Mutex::new(HashMap::new()),
        }
    }

    fn fetch_reference_backend(
        http_client: &reqwest::blocking::Client,
        backend_url: &str,
        context: &str,
        active_profile_id: Option<String>,
        rag_timeout: f32,
    ) -> Result<String, Box<dyn std::error::Error + Send + Sync>> {
        let url = format!("{}/api/retrieval", backend_url.trim_end_matches('/'));
        let body = serde_json::json!({
            "context": context,
            "history": [],
            "active_profile_id": active_profile_id,
            "timeout_secs": if rag_timeout > 0.0 { rag_timeout as f64 } else { 3.0 },
            "max_tokens": 256,
        });

        tracing::info!(
            "RAG: calling backend retrieval, context_len={}, profile={:?}",
            context.len(),
            active_profile_id,
        );

        let start = Instant::now();

        let mut request = http_client.post(&url).json(&body);
        if rag_timeout > 0.0 {
            request = request.timeout(Duration::from_secs_f64(rag_timeout as f64));
        }

        let response = request.send().map_err(|e| -> Box<dyn std::error::Error + Send + Sync> {
            if e.is_timeout() {
                format!("backend retrieval timed out after {:.1}s", rag_timeout).into()
            } else {
                e.to_string().into()
            }
        })?;

        let elapsed = start.elapsed();

        if !response.status().is_success() {
            let status = response.status();
            let body_text = response.text().unwrap_or_else(|_| "unknown".to_string());
            return Err(
                format!("backend retrieval returned {} - {}", status, body_text).into()
            );
        }

        let resp: BackendRetrievalResponse = response.json().map_err(
            |e| -> Box<dyn std::error::Error + Send + Sync> {
                format!("failed to parse backend retrieval response: {}", e).into()
            },
        )?;

        tracing::info!(
            "RAG: backend retrieval completed in {:.3}s, label={}",
            elapsed.as_secs_f64(),
            resp.lm_label
        );

        Ok(format!("{}\t{}", resp.lm_label, resp.reference_text))
    }

    pub fn trigger_background_generation_slot<F>(
        &self,
        slot_id: usize,
        wait_secs: f64,
        rag_timeout: f32,
        context_provider: F,
    ) where
        F: FnOnce() -> String + Send + 'static,
    {
        self.cancel_pending_slot_nonblocking(slot_id);

        let (result_tx, result_rx) = std::sync::mpsc::channel();
        let cancel = Arc::new(AtomicBool::new(false));

        let cancel_clone = Arc::clone(&cancel);
        let backend_url = self.backend_url.clone();
        let http_client = self.http_client.clone();
        let profile_id = self.retrieval.default_id_for_ui_slot(slot_id);

        let handle = thread::spawn(move || {
            let deadline = Instant::now()
                .checked_add(Duration::from_secs_f64(wait_secs.max(0.0)))
                .unwrap_or_else(Instant::now);
            while Instant::now() < deadline {
                if cancel_clone.load(Ordering::SeqCst) {
                    let _ = result_tx.send(String::new());
                    return;
                }
                let remaining = deadline.saturating_duration_since(Instant::now());
                let sleep_dur = remaining.min(Duration::from_millis(50));
                thread::sleep(sleep_dur);
            }
            if cancel_clone.load(Ordering::SeqCst) {
                let _ = result_tx.send(String::new());
                return;
            }

            let context = context_provider();
            let reference_text = match Self::fetch_reference_backend(
                &http_client,
                &backend_url,
                &context,
                profile_id,
                rag_timeout,
            ) {
                Ok(t) => t,
                Err(e) => {
                    tracing::warn!("[RET_FAILED] backend retrieval error for slot {}: {}", slot_id, e);
                    String::new()
                }
            };
            let _ = result_tx.send(reference_text);
        });

        let mut tasks = self.batched_tasks.lock().unwrap();
        tasks.insert(slot_id, SlotTask { result_rx, join_handle: handle, cancel });
    }

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
            tasks.remove(&slot_id);
            Some((slot_id, text))
        } else {
            None
        }
    }

    pub fn cancel_pending_slot_nonblocking(&self, slot_id: usize) {
        let task = {
            let mut tasks = self.batched_tasks.lock().unwrap();
            tasks.remove(&slot_id)
        };
        if let Some(task) = task {
            task.cancel.store(true, Ordering::SeqCst);
        }
    }

    /// Blocking variant of slot cancellation — waits for the background thread to finish.
    #[allow(dead_code)]
    pub fn cancel_pending_slot(&self, slot_id: usize) {
        let task = {
            let mut tasks = self.batched_tasks.lock().unwrap();
            tasks.remove(&slot_id)
        };
        if let Some(task) = task {
            task.cancel.store(true, Ordering::SeqCst);
            let _ = task.join_handle.join();
        }
    }

    /// Cancels and joins all pending background tasks (used during graceful shutdown).
    #[allow(dead_code)]
    pub fn cancel_pending(&self) {
        let drained: Vec<_> = {
            let mut tasks = self.batched_tasks.lock().unwrap();
            tasks.drain().collect()
        };
        for (_, task) in &drained {
            task.cancel.store(true, Ordering::SeqCst);
        }
        for (_, task) in drained {
            let _ = task.join_handle.join();
        }
    }
}
