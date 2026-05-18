use std::sync::mpsc::Sender;
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};

#[derive(Debug, Clone, serde::Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum OutboundWsMsg {
    Token { slot_id: usize, role: String, text: String },
    CallStart { slot_id: usize, caller_phone: Option<String>, call_id: Option<u64> },
    CallEnd { slot_id: usize },
    RetToken { slot_id: usize },
}

#[derive(Debug, Clone, serde::Deserialize)]
#[serde(tag = "type")]
pub enum InboundWsMsg {
    #[serde(rename = "reference_inject")]
    ReferenceInject { slot_id: usize, text: String },
    #[serde(rename = "arc_inject")]
    ArcInject { slot_id: usize, reference_text: String },
    #[serde(rename = "trigger_fired")]
    TriggerFired {
        slot_id: usize,
        #[serde(default)]
        label: String,
        #[serde(default)]
        action_type: String,
    },
}

impl OutboundWsMsg {
    fn to_json(&self) -> String {
        serde_json::to_string(self).expect("OutboundWsMsg serialization is infallible")
    }
}

pub struct BackendWsClient {
    out_tx: tokio::sync::mpsc::UnboundedSender<OutboundWsMsg>,
}

impl BackendWsClient {
    pub fn sender(&self) -> tokio::sync::mpsc::UnboundedSender<OutboundWsMsg> {
        self.out_tx.clone()
    }

    pub fn new(
        backend_url: String,
        rt: tokio::runtime::Handle,
    ) -> (Self, std::sync::mpsc::Receiver<InboundWsMsg>) {
        let (out_tx, out_rx) = tokio::sync::mpsc::unbounded_channel::<OutboundWsMsg>();
        let (in_tx, in_rx) = std::sync::mpsc::channel::<InboundWsMsg>();
        rt.spawn(Self::ws_task(backend_url, out_rx, in_tx));
        (Self { out_tx }, in_rx)
    }

    async fn ws_task(
        backend_url: String,
        mut out_rx: tokio::sync::mpsc::UnboundedReceiver<OutboundWsMsg>,
        in_tx: Sender<InboundWsMsg>,
    ) {
        let ws_url = format!(
            "{}/ws/text-stream",
            backend_url
                .trim_end_matches('/')
                .replace("http://", "ws://")
                .replace("https://", "wss://")
        );

        let mut backoff_ms: u64 = 500;
        const MAX_BACKOFF_MS: u64 = 30_000;

        loop {
            tracing::info!("backend_ws: connecting to {}", ws_url);
            let connect_result = tokio::time::timeout(
                Duration::from_secs(5),
                tokio_tungstenite::connect_async(&ws_url),
            )
            .await;
            match connect_result {
                Err(_) => {
                    tracing::warn!(
                        "backend_ws: connection timed out after 5s. Retrying in {}ms",
                        backoff_ms
                    );
                }
                Ok(Err(e)) => {
                    tracing::warn!(
                        "backend_ws: connection failed: {}. Retrying in {}ms",
                        e,
                        backoff_ms
                    );
                }
                Ok(Ok((ws_stream, _))) => {
                    tracing::info!("backend_ws: connected to {}", ws_url);
                    backoff_ms = 500;
                    let (mut write, mut read) = ws_stream.split();

                    loop {
                        tokio::select! {
                            ws_msg = read.next() => {
                                match ws_msg {
                                    Some(Ok(tokio_tungstenite::tungstenite::Message::Text(text))) => {
                                        match serde_json::from_str::<InboundWsMsg>(&text) {
                                            Ok(inbound) => {
                                                if in_tx.send(inbound).is_err() {
                                                    tracing::debug!("backend_ws: inbound channel closed");
                                                    return;
                                                }
                                            }
                                            Err(e) => {
                                                tracing::debug!("backend_ws: unknown/malformed message: {} — {}", e, text);
                                            }
                                        }
                                    }
                                    Some(Ok(tokio_tungstenite::tungstenite::Message::Ping(data))) => {
                                        if let Err(e) = write
                                            .send(tokio_tungstenite::tungstenite::Message::Pong(data))
                                            .await
                                        {
                                            tracing::warn!("backend_ws: failed to send pong: {}", e);
                                            break;
                                        }
                                    }
                                    Some(Ok(tokio_tungstenite::tungstenite::Message::Close(_))) => {
                                        tracing::info!("backend_ws: server closed connection");
                                        break;
                                    }
                                    Some(Err(e)) => {
                                        tracing::warn!("backend_ws: read error: {}", e);
                                        break;
                                    }
                                    None => {
                                        tracing::info!("backend_ws: WebSocket stream ended");
                                        break;
                                    }
                                    _ => {}
                                }
                            }
                            out_msg = out_rx.recv() => {
                                match out_msg {
                                    Some(msg) => {
                                        let json = msg.to_json();
                                        if let Err(e) = write
                                            .send(tokio_tungstenite::tungstenite::Message::Text(json))
                                            .await
                                        {
                                            tracing::warn!("backend_ws: write error: {}", e);
                                            break;
                                        }
                                    }
                                    None => {
                                        tracing::info!("backend_ws: outbound channel closed, stopping ws_task");
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            tokio::time::sleep(Duration::from_millis(backoff_ms)).await;
            backoff_ms = (backoff_ms * 2).min(MAX_BACKOFF_MS);
        }
    }
}
