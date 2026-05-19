use axum::extract::ws::{Message, WebSocket};
use futures_util::{SinkExt, StreamExt};
use std::collections::HashMap;
use std::sync::Arc;
use std::time::Instant;
use tokio::sync::mpsc;

use crate::config::SharedConfig;
use crate::tomedo_client::TomedoClient;

const MAX_CONTEXT_CHARS: usize = 50_000;
const TRIM_TO_CHARS: usize = 40_000;
const MAX_SLOTS: usize = 64;

#[derive(Debug, serde::Deserialize)]
#[serde(tag = "type")]
pub enum IncomingMessage {
    #[serde(rename = "token")]
    Token {
        slot_id: usize,
        role: String,
        text: String,
        #[serde(default)]
        ts_ms: Option<u64>,
    },
    #[serde(rename = "call_start")]
    CallStart {
        slot_id: usize,
        #[serde(default)]
        caller_phone: Option<String>,
        #[serde(default)]
        call_id: Option<u64>,
    },
    #[serde(rename = "call_end")]
    CallEnd { slot_id: usize },
    #[serde(rename = "ret_token")]
    RetToken { slot_id: usize },
}

#[derive(Debug, Clone, serde::Serialize)]
#[serde(tag = "type")]
pub enum OutgoingMessage {
    #[serde(rename = "reference_inject")]
    ReferenceInject { slot_id: usize, text: String },
    #[serde(rename = "arc_inject")]
    ArcInject { slot_id: usize, reference_text: String },
    #[serde(rename = "trigger_fired")]
    TriggerFired { slot_id: usize, label: String, action_type: String },
}

pub struct SlotState {
    pub patient_id: Option<i64>,
    pub caller_phone: Option<String>,
    pub call_id: Option<u64>,
    pub context_buffer: String,
    pub context_char_count: usize,
    pub trigger_cooldowns: HashMap<String, Instant>,
    pub last_token_ts_ms: Option<u64>,
    last_role: Option<String>,
}

impl SlotState {
    fn new() -> Self {
        Self {
            patient_id: None,
            caller_phone: None,
            call_id: None,
            context_buffer: String::new(),
            context_char_count: 0,
            trigger_cooldowns: HashMap::new(),
            last_token_ts_ms: None,
            last_role: None,
        }
    }

    fn append_token(&mut self, role: &str, token_text: &str) {
        let display_role = if role == "user" { "user" } else { "moshi" };

        let role_changed = self.last_role.as_deref() != Some(role);
        if role_changed {
            if !self.context_buffer.is_empty() {
                self.context_buffer.push('\n');
                self.context_char_count += 1;
            }
            self.context_buffer.push_str(display_role);
            self.context_buffer.push_str(": ");
            self.context_char_count += display_role.len() + 2; // ASCII-only labels: .len() == .chars().count()
            self.last_role = Some(role.to_string());
        }

        self.context_char_count += token_text.chars().count();
        self.context_buffer.push_str(token_text);

        if self.context_char_count > MAX_CONTEXT_CHARS {
            self.context_buffer = trim_to_last_n_chars(&self.context_buffer, TRIM_TO_CHARS);
            self.context_char_count = TRIM_TO_CHARS;
            self.last_role = None; // trim loses role context — next token will re-add prefix
        }
    }
}

fn trim_to_last_n_chars(s: &str, n: usize) -> String {
    let total = s.chars().count();
    if total <= n {
        return s.to_string();
    }
    let skip = total - n;
    let byte_offset = s.char_indices().nth(skip).map_or(0, |(i, _)| i);
    s[byte_offset..].to_string()
}

fn window_from_end(s: &str, total_chars: usize, window_chars: usize) -> &str {
    if total_chars <= window_chars {
        return s;
    }
    let skip = total_chars - window_chars;
    let start_byte = s.char_indices().nth(skip).map_or(0, |(i, _)| i);
    &s[start_byte..]
}

struct TriggerMatcher {
    compiled: Vec<CompiledTrigger>,
}

struct CompiledTrigger {
    id: String,
    label: String,
    pattern: TriggerPattern,
    action_type: String,
    action_url: Option<String>,
    cooldown_secs: f64,
    inject_result: bool,
}

enum TriggerPattern {
    Keyword(String),
    Regex(regex::Regex),
}

impl TriggerMatcher {
    fn from_config(config: &crate::config::AppConfig) -> Self {
        let mut compiled = Vec::new();
        for t in &config.triggers {
            let pattern = if t.trigger_type == "regex" {
                match regex::Regex::new(&t.match_pattern) {
                    Ok(re) => TriggerPattern::Regex(re),
                    Err(e) => {
                        tracing::warn!("invalid regex trigger '{}': {}", t.label, e);
                        continue;
                    }
                }
            } else {
                TriggerPattern::Keyword(t.match_pattern.to_lowercase())
            };
            compiled.push(CompiledTrigger {
                id: t.id.clone(),
                label: t.label.clone(),
                pattern,
                action_type: t.action_type.clone(),
                action_url: t.action_url.clone(),
                cooldown_secs: t.cooldown_secs,
                inject_result: t.inject_result,
            });
        }
        Self { compiled }
    }

    fn check_triggers(
        &self,
        window: &str,
        cooldowns: &mut HashMap<String, Instant>,
    ) -> Vec<&CompiledTrigger> {
        let now = Instant::now();
        let lower_window = window.to_lowercase();
        let mut fired = Vec::new();
        for trigger in &self.compiled {
            if let Some(last) = cooldowns.get(&trigger.id) {
                if now.duration_since(*last).as_secs_f64() < trigger.cooldown_secs {
                    continue;
                }
            }
            let matched = match &trigger.pattern {
                TriggerPattern::Keyword(kw) => lower_window.contains(kw.as_str()),
                TriggerPattern::Regex(re) => re.is_match(window),
            };
            if matched {
                cooldowns.insert(trigger.id.clone(), now);
                fired.push(trigger);
            }
        }
        fired
    }
}

pub struct ActionRequest {
    pub slot_id: usize,
    pub action_type: String,
    pub action_url: Option<String>,
    pub context_snippet: String,
    pub patient_id: Option<i64>,
    pub inject_result: bool,
    pub label: String,
    pub arc_mode: bool,
    pub response_tx: mpsc::Sender<OutgoingMessage>,
}

struct WsCtx<'a> {
    trigger_matcher: &'a TriggerMatcher,
    trigger_window_chars: usize,
    arc_mode: bool,
    action_tx: &'a mpsc::Sender<ActionRequest>,
    response_tx: &'a mpsc::Sender<OutgoingMessage>,
    tomedo_client: &'a Option<Arc<TomedoClient>>,
    pid_tx: &'a mpsc::Sender<(usize, Option<i64>)>,
    ret_action_type: &'a str,
    ret_inject_result: bool,
    ret_action_url: Option<&'a str>,
}

fn handle_call_start_msg(
    slot_id: usize,
    caller_phone: Option<String>,
    call_id: Option<u64>,
    slots: &mut HashMap<usize, SlotState>,
    ctx: &WsCtx<'_>,
) {
    tracing::info!(slot_id, ?caller_phone, ?call_id, "call_start");
    if slots.len() >= MAX_SLOTS && !slots.contains_key(&slot_id) {
        tracing::warn!(slot_id, max = MAX_SLOTS, "rejecting call_start: slot limit reached");
        return;
    }
    let mut slot = SlotState::new();
    slot.call_id = call_id;
    slot.caller_phone.clone_from(&caller_phone);
    slots.insert(slot_id, slot);

    if let (Some(tomedo), Some(phone), Some(cid)) =
        (ctx.tomedo_client.as_ref(), caller_phone, call_id)
    {
        let tomedo = tomedo.clone();
        let pid_tx = ctx.pid_tx.clone();
        tokio::spawn(async move {
            match tomedo.resolve_caller(cid, &phone).await {
                Ok(pid) => {
                    if pid_tx.send((slot_id, pid)).await.is_err() {
                        tracing::debug!(
                            slot_id,
                            "pid update channel closed before patient_id arrived"
                        );
                    }
                }
                Err(e) => tracing::warn!(slot_id, "resolve_caller failed: {e}"),
            }
        });
    }
}

fn handle_token_msg(
    slot_id: usize,
    role: String,
    token_text: String,
    ts_ms: Option<u64>,
    slots: &mut HashMap<usize, SlotState>,
    ctx: &WsCtx<'_>,
) {
    if slots.len() >= MAX_SLOTS && !slots.contains_key(&slot_id) {
        tracing::warn!(slot_id, max = MAX_SLOTS, "dropping token: slot limit reached");
        return;
    }
    let slot = slots.entry(slot_id).or_insert_with(SlotState::new);
    slot.last_token_ts_ms = ts_ms;
    slot.append_token(&role, &token_text);

    let window =
        window_from_end(&slot.context_buffer, slot.context_char_count, ctx.trigger_window_chars);
    let fired = ctx.trigger_matcher.check_triggers(window, &mut slot.trigger_cooldowns);

    for trigger in fired {
        tracing::info!(slot_id, label = %trigger.label, action_type = %trigger.action_type, "trigger fired");
        if let Err(e) = ctx.response_tx.try_send(OutgoingMessage::TriggerFired {
            slot_id,
            label: trigger.label.clone(),
            action_type: trigger.action_type.clone(),
        }) {
            tracing::warn!(slot_id, label = %trigger.label, error = %e, "dropped TriggerFired notification: response channel full");
        }
        if let Err(e) = ctx.action_tx.try_send(ActionRequest {
            slot_id,
            action_type: trigger.action_type.clone(),
            action_url: trigger.action_url.clone(),
            context_snippet: window.to_string(),
            patient_id: slot.patient_id,
            inject_result: trigger.inject_result,
            label: trigger.label.clone(),
            arc_mode: ctx.arc_mode,
            response_tx: ctx.response_tx.clone(),
        }) {
            tracing::warn!(slot_id, label = %trigger.label, error = %e, "dropped action dispatch: action channel full");
        }
    }
}

fn handle_ret_token_msg(slot_id: usize, slots: &HashMap<usize, SlotState>, ctx: &WsCtx<'_>) {
    tracing::info!(
        slot_id,
        action_type = ctx.ret_action_type,
        "ret_token received — dispatching configured action"
    );

    let (context_snippet, patient_id) = slots.get(&slot_id).map_or_else(
        || {
            tracing::warn!(slot_id, "ret_token for unknown slot — dispatching with empty context");
            (String::new(), None)
        },
        |slot| {
            let window = window_from_end(
                &slot.context_buffer,
                slot.context_char_count,
                ctx.trigger_window_chars,
            );
            (window.to_string(), slot.patient_id)
        },
    );

    if let Err(e) = ctx.action_tx.try_send(ActionRequest {
        slot_id,
        action_type: ctx.ret_action_type.to_string(),
        action_url: ctx.ret_action_url.map(str::to_string),
        context_snippet,
        patient_id,
        inject_result: ctx.ret_inject_result,
        label: "[RET]".to_string(),
        arc_mode: ctx.arc_mode,
        response_tx: ctx.response_tx.clone(),
    }) {
        tracing::warn!(slot_id, error = %e, "dropped [RET] action dispatch: action channel full");
    }
}

fn handle_incoming(
    incoming: IncomingMessage,
    slots: &mut HashMap<usize, SlotState>,
    ctx: &WsCtx<'_>,
) {
    match incoming {
        IncomingMessage::CallStart { slot_id, caller_phone, call_id } => {
            handle_call_start_msg(slot_id, caller_phone, call_id, slots, ctx);
        }
        IncomingMessage::CallEnd { slot_id } => {
            tracing::info!(slot_id, "call_end");
            slots.remove(&slot_id);
        }
        IncomingMessage::Token { slot_id, role, text: token_text, ts_ms } => {
            handle_token_msg(slot_id, role, token_text, ts_ms, slots, ctx);
        }
        IncomingMessage::RetToken { slot_id } => {
            handle_ret_token_msg(slot_id, slots, ctx);
        }
    }
}

pub async fn handle_websocket(
    socket: WebSocket,
    config: SharedConfig,
    action_tx: mpsc::Sender<ActionRequest>,
    tomedo_client: Option<Arc<TomedoClient>>,
) {
    let (mut ws_tx, mut ws_rx) = socket.split();
    let (response_tx, mut response_rx) = mpsc::channel::<OutgoingMessage>(256);

    let send_task = tokio::spawn(async move {
        while let Some(msg) = response_rx.recv().await {
            match serde_json::to_string(&msg) {
                Ok(json) => {
                    if ws_tx.send(Message::Text(json.into())).await.is_err() {
                        break;
                    }
                }
                Err(e) => tracing::error!("failed to serialize outgoing message: {}", e),
            }
        }
    });

    let mut slots: HashMap<usize, SlotState> = HashMap::new();
    // Intentionally cached at connection time — arc_mode, trigger_window_chars, and
    // ret_* settings are restart-level settings, not mid-call toggles. Avoids per-token
    // RwLock reads on the hot path. If these ever need to be dynamic, use
    // AtomicBool/AtomicUsize instead.
    //
    // NOTE: trigger_matcher is also cached here. Triggers updated via POST /api/config
    // take effect only on the next WebSocket connection (frontend reconnect / service
    // restart). If hot-reload is needed, add an AtomicU64 generation counter to
    // SharedConfig, increment it in post_config_handler, and rebuild TriggerMatcher
    // when the generation changes (check once per token — single atomic load, O(1)).
    let (
        trigger_matcher,
        trigger_window_chars,
        arc_mode,
        ret_action_type,
        ret_inject_result,
        ret_action_url,
    ) = {
        let cfg = config.read();
        (
            TriggerMatcher::from_config(&cfg),
            cfg.trigger_window_chars,
            cfg.arc_mode_enabled,
            cfg.ret_action_type.clone(),
            cfg.ret_inject_result,
            cfg.ret_action_url.clone(),
        )
    };

    // Channel for background patient-ID resolution results (slot_id, resolved patient_id).
    // Spawned tasks post here; the select! loop merges updates into SlotState.
    let (pid_tx, mut pid_rx) = mpsc::channel::<(usize, Option<i64>)>(MAX_SLOTS);

    {
        let ctx = WsCtx {
            trigger_matcher: &trigger_matcher,
            trigger_window_chars,
            arc_mode,
            action_tx: &action_tx,
            response_tx: &response_tx,
            tomedo_client: &tomedo_client,
            pid_tx: &pid_tx,
            ret_action_type: &ret_action_type,
            ret_inject_result,
            ret_action_url: ret_action_url.as_deref(),
        };
        loop {
            tokio::select! {
                msg_result = ws_rx.next() => {
                    let msg = match msg_result {
                        Some(Ok(m)) => m,
                        Some(Err(e)) => { tracing::warn!("websocket receive error: {}", e); break; }
                        None => break,
                    };
                    let text = match msg {
                        Message::Text(t) => t,
                        Message::Close(_) => break,
                        Message::Ping(_) | Message::Pong(_) | Message::Binary(_) => continue,
                    };
                    match serde_json::from_str::<IncomingMessage>(&text) {
                        Ok(incoming) => handle_incoming(incoming, &mut slots, &ctx),
                        Err(e) => tracing::warn!("invalid websocket message: {} — {}", e, text),
                    }
                }
                Some((sid, pid)) = pid_rx.recv() => {
                    if let Some(slot) = slots.get_mut(&sid) {
                        slot.patient_id = pid;
                        if pid.is_some() {
                            tracing::info!(slot_id = sid, patient_id = ?pid, "patient_id resolved from tomedo-crawl");
                        } else {
                            tracing::info!(slot_id = sid, "caller not found in tomedo-crawl — patient_id remains None");
                        }
                    }
                }
            }
        }
    } // ctx dropped here — response_tx is no longer borrowed

    drop(response_tx);
    if let Err(e) = send_task.await {
        tracing::error!("websocket send task panicked: {:?}", e);
    }
    tracing::info!("websocket session ended");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn window_from_end_ascii() {
        let s = "hello world foo bar";
        assert_eq!(window_from_end(s, s.chars().count(), 7), "foo bar");
    }

    #[test]
    fn window_from_end_unicode() {
        let s = "Diagnose: Müdigkeit und Übelkeit";
        let total = s.chars().count();
        let window = window_from_end(s, total, 10);
        assert!(s.ends_with(window));
        assert_eq!(window.chars().count(), 10);
        assert!(std::str::from_utf8(window.as_bytes()).is_ok());
    }

    #[test]
    fn window_from_end_shorter_than_window() {
        let s = "short";
        assert_eq!(window_from_end(s, s.chars().count(), 100), s);
    }

    #[test]
    fn trim_to_last_n_chars_basic() {
        let s = "abcde";
        assert_eq!(trim_to_last_n_chars(s, 3), "cde");
    }

    #[test]
    fn trim_to_last_n_chars_unicode() {
        let s = "Müdigkeit";
        let trimmed = trim_to_last_n_chars(s, 5);
        assert_eq!(trimmed.chars().count(), 5);
        assert!(std::str::from_utf8(trimmed.as_bytes()).is_ok());
    }

    #[test]
    fn slot_append_token_caps_buffer() {
        let mut slot = SlotState::new();
        let big_token: String = "a".repeat(1000);
        for _ in 0..(MAX_CONTEXT_CHARS / 900 + 10) {
            slot.append_token("user", &big_token);
        }
        assert!(slot.context_buffer.chars().count() <= MAX_CONTEXT_CHARS);
    }

    #[test]
    fn slot_append_token_role_transition() {
        let mut slot = SlotState::new();
        slot.append_token("user", "hello ");
        slot.append_token("user", "world");
        slot.append_token("model", "hi there");
        slot.append_token("user", "how are you");

        let buf = &slot.context_buffer;
        assert!(buf.contains("user: hello world"), "user text missing: {buf:?}");
        assert!(buf.contains("moshi: hi there"), "moshi text missing: {buf:?}");
        assert!(buf.contains("\nuser: how are you"), "second user turn missing: {buf:?}");
        assert_eq!(buf.chars().count(), slot.context_char_count, "context_char_count out of sync");
    }

    #[test]
    fn slot_char_count_matches_buffer() {
        let mut slot = SlotState::new();
        slot.append_token("user", "Müdigkeit");
        slot.append_token("user", " und Übelkeit");
        slot.append_token("model", "Ich verstehe.");
        assert_eq!(slot.context_buffer.chars().count(), slot.context_char_count);
    }
}
