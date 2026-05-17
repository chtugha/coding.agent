use axum::extract::ws::WebSocketUpgrade;
use axum::extract::State;
use axum::response::IntoResponse;
use axum::routing::{get, post};
use axum::{Json, Router};
use std::sync::Arc;
use tokio::sync::mpsc;

use crate::action::ActionDispatcher;
use crate::config::SharedConfig;
use crate::text_stream;
use crate::tomedo_client::TomedoClient;

pub struct AppState {
    pub config: SharedConfig,
    pub action_tx: mpsc::Sender<text_stream::ActionRequest>,
    pub http_client: reqwest::Client,
    pub tomedo_client: Option<Arc<TomedoClient>>,
    pub action_dispatcher: Arc<ActionDispatcher>,
}

pub fn create_router(state: Arc<AppState>) -> Router {
    Router::new()
        .route("/api/health", get(health_handler))
        .route("/api/config", get(get_config_handler))
        .route("/api/config", post(post_config_handler))
        .route("/api/retrieval", post(retrieval_handler))
        .route("/api/action", post(action_handler))
        .route("/ws/text-stream", get(ws_handler))
        .with_state(state)
}

async fn health_handler(State(state): State<Arc<AppState>>) -> impl IntoResponse {
    let (tomedo_url, llm_mode, arc_mode) = {
        let config = state.config.read();
        (config.tomedo_crawl_url.clone(), config.llm_mode_enabled, config.arc_mode_enabled)
    };
    let tomedo_reachable = if let Some(ref url) = tomedo_url {
        check_tomedo_health(&state.http_client, url).await
    } else {
        false
    };
    Json(serde_json::json!({
        "status": "ok",
        "tomedo_crawl_reachable": tomedo_reachable,
        "llm_mode_enabled": llm_mode,
        "arc_mode_enabled": arc_mode,
    }))
}

async fn check_tomedo_health(client: &reqwest::Client, base_url: &str) -> bool {
    let url = format!("{}/health", base_url.trim_end_matches('/'));
    client
        .get(&url)
        .timeout(std::time::Duration::from_secs(2))
        .send()
        .await
        .is_ok_and(|r| r.status().is_success())
}

async fn get_config_handler(State(state): State<Arc<AppState>>) -> impl IntoResponse {
    let config = state.config.read();
    Json(serde_json::json!({
        "llm_mode_enabled": config.llm_mode_enabled,
        "arc_mode_enabled": config.arc_mode_enabled,
        "tomedo_crawl_url": config.tomedo_crawl_url,
        "trigger_window_chars": config.trigger_window_chars,
        "triggers": config.triggers,
        "llm_profiles": config.llm_profiles,
        "default_timeout_secs": config.default_timeout_secs,
        "default_max_tokens": config.default_max_tokens,
        "ret_action_type": config.ret_action_type,
        "ret_inject_result": config.ret_inject_result,
        "ret_action_url": config.ret_action_url,
    }))
}

#[derive(serde::Deserialize)]
struct ConfigUpdate {
    #[serde(default)]
    llm_mode_enabled: Option<bool>,
    #[serde(default)]
    arc_mode_enabled: Option<bool>,
    #[serde(default)]
    triggers: Option<Vec<crate::config::TriggerConfig>>,
    #[serde(default)]
    tomedo_crawl_url: Option<String>,
    #[serde(default)]
    trigger_window_chars: Option<usize>,
    #[serde(default)]
    ret_action_type: Option<String>,
    #[serde(default)]
    ret_inject_result: Option<bool>,
    #[serde(default)]
    ret_action_url: Option<String>,
}

async fn post_config_handler(
    State(state): State<Arc<AppState>>,
    Json(update): Json<ConfigUpdate>,
) -> impl IntoResponse {
    {
        let mut config = state.config.write();
        if let Some(v) = update.llm_mode_enabled {
            config.llm_mode_enabled = v;
        }
        if let Some(v) = update.arc_mode_enabled {
            config.arc_mode_enabled = v;
        }
        if let Some(t) = update.triggers {
            config.triggers = t;
        }
        if let Some(url) = update.tomedo_crawl_url {
            config.tomedo_crawl_url = Some(url);
        }
        if let Some(w) = update.trigger_window_chars {
            config.trigger_window_chars = w;
        }
        if let Some(v) = update.ret_action_type {
            config.ret_action_type = v;
        }
        if let Some(v) = update.ret_inject_result {
            config.ret_inject_result = v;
        }
        if let Some(v) = update.ret_action_url {
            config.ret_action_url = Some(v);
        }
    }
    Json(serde_json::json!({"status": "ok"}))
}

async fn retrieval_handler(
    State(state): State<Arc<AppState>>,
    Json(request): Json<crate::retrieval::RetrievalRequest>,
) -> impl IntoResponse {
    let llm_enabled = state.config.read().llm_mode_enabled;
    if !llm_enabled {
        return (
            axum::http::StatusCode::SERVICE_UNAVAILABLE,
            Json(serde_json::json!({
                "error": "LLM mode is not enabled"
            })),
        );
    }

    match crate::retrieval::handle_retrieval(&state.config, &state.http_client, request).await {
        Ok(resp) => (
            axum::http::StatusCode::OK,
            Json(serde_json::json!({
                "reference_text": resp.reference_text,
                "lm_label": resp.lm_label,
                "num_turns": resp.num_turns,
            })),
        ),
        Err(e) => {
            tracing::error!("retrieval error: {e}");
            (
                axum::http::StatusCode::INTERNAL_SERVER_ERROR,
                Json(serde_json::json!({
                    "error": e.to_string(),
                    "reference_text": "",
                    "lm_label": "",
                    "num_turns": 0,
                })),
            )
        }
    }
}

#[derive(serde::Deserialize)]
struct ActionRequestBody {
    slot_id: usize,
    action_type: String,
    #[serde(default)]
    action_url: Option<String>,
    #[serde(default)]
    context_snippet: Option<String>,
    #[serde(default)]
    patient_id: Option<i64>,
}

async fn action_handler(
    State(state): State<Arc<AppState>>,
    Json(request): Json<ActionRequestBody>,
) -> impl IntoResponse {
    let (discard_tx, _discard_rx) = mpsc::channel::<text_stream::OutgoingMessage>(1);
    let arc_mode = state.config.read().arc_mode_enabled;

    let action_req = text_stream::ActionRequest {
        slot_id: request.slot_id,
        action_type: request.action_type.clone(),
        action_url: request.action_url,
        context_snippet: request.context_snippet.unwrap_or_default(),
        patient_id: request.patient_id,
        inject_result: true,
        label: format!("http-api-{}", request.action_type),
        arc_mode,
        response_tx: discard_tx,
    };

    match state.action_dispatcher.dispatch(&action_req).await {
        Ok(result) => (
            axum::http::StatusCode::OK,
            Json(serde_json::json!({
                "status": "ok",
                "result": result.text,
                "action_type": result.action_type,
                "inject_text": if result.text.is_empty() { None } else { Some(&result.text) }
            })),
        ),
        Err(e) => (
            axum::http::StatusCode::INTERNAL_SERVER_ERROR,
            Json(serde_json::json!({
                "status": "error",
                "error": e.to_string(),
                "result": "",
                "inject_text": null
            })),
        ),
    }
}

async fn ws_handler(State(state): State<Arc<AppState>>, ws: WebSocketUpgrade) -> impl IntoResponse {
    let config = state.config.clone();
    let action_tx = state.action_tx.clone();
    let tomedo = state.tomedo_client.clone();
    ws.on_upgrade(move |socket| text_stream::handle_websocket(socket, config, action_tx, tomedo))
}
