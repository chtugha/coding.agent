// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

use anyhow::Result;
use socket2::SockRef;
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::interconnect::{accept_connection, bind_listen, recv_packet, TEXT_PORT};

const ACCEPT_TIMEOUT: Duration = Duration::from_secs(1);
const RECV_TIMEOUT: Duration = Duration::from_secs(2);
const MAX_QUEUE_DEPTH: usize = 32;

pub type WhisperTextQueues = Arc<Mutex<WhisperTextState>>;

pub struct WhisperTextState {
    call_id_to_batch_idx: HashMap<u32, usize>,
    per_slot: Vec<VecDeque<String>>,
}

impl WhisperTextState {
    pub fn new(batch_size: usize) -> Self {
        Self {
            call_id_to_batch_idx: HashMap::new(),
            per_slot: (0..batch_size).map(|_| VecDeque::new()).collect(),
        }
    }

    pub fn register_call(&mut self, call_id: u32, batch_idx: usize) {
        self.call_id_to_batch_idx.insert(call_id, batch_idx);
    }

    pub fn unregister_call(&mut self, call_id: u32) {
        if let Some(batch_idx) = self.call_id_to_batch_idx.remove(&call_id) {
            if batch_idx < self.per_slot.len() {
                self.per_slot[batch_idx].clear();
            }
        }
    }

    pub fn drain_slot(&mut self, batch_idx: usize) -> Vec<String> {
        if batch_idx < self.per_slot.len() {
            self.per_slot[batch_idx].drain(..).collect()
        } else {
            Vec::new()
        }
    }

    fn push_text(&mut self, call_id: u32, text: String) -> bool {
        if let Some(&batch_idx) = self.call_id_to_batch_idx.get(&call_id) {
            if batch_idx < self.per_slot.len() {
                let queue = &mut self.per_slot[batch_idx];
                if queue.len() >= MAX_QUEUE_DEPTH {
                    queue.pop_front();
                    tracing::warn!(call_id, "whisper receiver: queue full, dropping oldest text");
                }
                queue.push_back(text);
                return true;
            }
        }
        false
    }
}

pub fn new_whisper_text_queues(batch_size: usize) -> WhisperTextQueues {
    Arc::new(Mutex::new(WhisperTextState::new(batch_size)))
}

pub fn spawn_whisper_receiver(
    queues: WhisperTextQueues,
    running: Arc<std::sync::atomic::AtomicBool>,
) -> std::thread::JoinHandle<()> {
    std::thread::Builder::new()
        .name("whisper-receiver".into())
        .spawn(move || {
            if let Err(e) = whisper_receiver_loop(&queues, &running) {
                if running.load(std::sync::atomic::Ordering::Relaxed) {
                    tracing::error!("whisper receiver fatal error: {}", e);
                }
            }
        })
        .expect("failed to spawn whisper-receiver thread")
}

fn is_io_timeout(e: &anyhow::Error) -> bool {
    for cause in e.chain() {
        if let Some(io_err) = cause.downcast_ref::<std::io::Error>() {
            if matches!(
                io_err.kind(),
                std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock
            ) {
                return true;
            }
        }
    }
    false
}

fn whisper_receiver_loop(
    queues: &WhisperTextQueues,
    running: &std::sync::atomic::AtomicBool,
) -> Result<()> {
    let listener = bind_listen(TEXT_PORT)?;
    SockRef::from(&listener).set_read_timeout(Some(ACCEPT_TIMEOUT))?;
    tracing::info!("whisper receiver listening on port {}", TEXT_PORT);

    while running.load(std::sync::atomic::Ordering::Relaxed) {
        tracing::trace!("whisper receiver waiting for connection on port {}", TEXT_PORT);
        let stream = match accept_connection(&listener) {
            Ok(s) => s,
            Err(e) => {
                if is_io_timeout(&e) {
                    continue;
                }
                if running.load(std::sync::atomic::Ordering::Relaxed) {
                    tracing::warn!("whisper receiver accept failed: {}, retrying", e);
                    std::thread::sleep(Duration::from_millis(200));
                }
                continue;
            }
        };
        if let Err(e) = stream.set_read_timeout(Some(RECV_TIMEOUT)) {
            tracing::warn!("whisper receiver: failed to set read timeout: {}", e);
        }
        tracing::info!("whisper receiver: connection established");

        let mut reader = std::io::BufReader::new(&stream);
        loop {
            if !running.load(std::sync::atomic::Ordering::Relaxed) {
                break;
            }
            match recv_packet(&mut reader) {
                Ok(packet) => {
                    let text = match std::str::from_utf8(&packet.payload) {
                        Ok(s) => s.to_string(),
                        Err(e) => {
                            tracing::warn!(
                                call_id = packet.call_id,
                                "whisper receiver: non-UTF-8 payload ({} bytes): {}",
                                packet.payload.len(),
                                e
                            );
                            continue;
                        }
                    };
                    if text.is_empty() {
                        continue;
                    }
                    let mut state = queues.lock().unwrap();
                    if !state.push_text(packet.call_id, text) {
                        tracing::debug!(
                            call_id = packet.call_id,
                            "whisper receiver: no batch slot for call_id, dropping text"
                        );
                    }
                }
                Err(e) => {
                    if is_io_timeout(&e) {
                        continue;
                    }
                    if running.load(std::sync::atomic::Ordering::Relaxed) {
                        tracing::warn!("whisper receiver: connection lost: {}", e);
                    }
                    break;
                }
            }
        }
        tracing::info!("whisper receiver: connection closed, will re-accept");
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::interconnect::{send_packet, Packet};
    use std::io::Write;
    use std::net::TcpStream;
    use std::sync::atomic::{AtomicBool, Ordering};

    #[test]
    fn whisper_text_state_register_and_push() {
        let mut state = WhisperTextState::new(4);
        state.register_call(100, 0);
        state.register_call(200, 2);

        assert!(state.push_text(100, "hello".into()));
        assert!(state.push_text(100, "world".into()));
        assert!(state.push_text(200, "german text".into()));
        assert!(!state.push_text(999, "unknown call".into()));

        let drained = state.drain_slot(0);
        assert_eq!(drained, vec!["hello", "world"]);

        let drained = state.drain_slot(2);
        assert_eq!(drained, vec!["german text"]);

        let drained = state.drain_slot(0);
        assert!(drained.is_empty());
    }

    #[test]
    fn whisper_text_state_unregister_clears() {
        let mut state = WhisperTextState::new(2);
        state.register_call(42, 1);
        state.push_text(42, "some text".into());
        state.unregister_call(42);

        let drained = state.drain_slot(1);
        assert!(drained.is_empty());

        assert!(!state.push_text(42, "after unregister".into()));
    }

    #[test]
    fn whisper_text_state_drain_out_of_bounds() {
        let mut state = WhisperTextState::new(2);
        let drained = state.drain_slot(99);
        assert!(drained.is_empty());
    }

    #[test]
    fn whisper_text_state_push_out_of_bounds_batch_idx() {
        let mut state = WhisperTextState::new(2);
        state.call_id_to_batch_idx.insert(1, 999);
        assert!(!state.push_text(1, "oob".into()));
    }

    #[test]
    fn whisper_text_state_queue_cap_drops_oldest() {
        let mut state = WhisperTextState::new(1);
        state.register_call(1, 0);

        for i in 0..(MAX_QUEUE_DEPTH + 2) {
            state.push_text(1, format!("msg{}", i));
        }

        let drained = state.drain_slot(0);
        assert_eq!(drained.len(), MAX_QUEUE_DEPTH, "queue must be capped at MAX_QUEUE_DEPTH");
        assert_eq!(drained[0], "msg2", "oldest entries must be dropped first");
        assert_eq!(
            drained[MAX_QUEUE_DEPTH - 1],
            format!("msg{}", MAX_QUEUE_DEPTH + 1),
            "newest entry must be retained"
        );
    }

    #[test]
    fn whisper_receiver_accepts_and_reads_packets() {
        let running = Arc::new(AtomicBool::new(true));
        let queues = new_whisper_text_queues(4);

        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();

        let queues_clone = queues.clone();

        {
            let mut state = queues.lock().unwrap();
            state.register_call(10, 0);
            state.register_call(20, 1);
        }

        let receiver_thread = std::thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let mut reader = std::io::BufReader::new(&stream);
            loop {
                match recv_packet(&mut reader) {
                    Ok(packet) => {
                        let text = std::str::from_utf8(&packet.payload).unwrap().to_string();
                        if !text.is_empty() {
                            let mut state = queues_clone.lock().unwrap();
                            state.push_text(packet.call_id, text);
                        }
                    }
                    Err(_) => break,
                }
            }
        });

        let mut stream = TcpStream::connect(format!("127.0.0.1:{}", port)).unwrap();
        let pkt1 = Packet { call_id: 10, payload: b"Hallo Welt".to_vec() };
        let pkt2 = Packet { call_id: 20, payload: b"Guten Tag".to_vec() };
        let pkt3 = Packet { call_id: 10, payload: b"zweiter Satz".to_vec() };

        let mut buf = Vec::new();
        send_packet(&mut buf, &pkt1).unwrap();
        send_packet(&mut buf, &pkt2).unwrap();
        send_packet(&mut buf, &pkt3).unwrap();
        stream.write_all(&buf).unwrap();
        drop(stream);

        receiver_thread.join().unwrap();

        let mut state = queues.lock().unwrap();
        let slot0 = state.drain_slot(0);
        assert_eq!(slot0, vec!["Hallo Welt", "zweiter Satz"]);
        let slot1 = state.drain_slot(1);
        assert_eq!(slot1, vec!["Guten Tag"]);

        running.store(false, Ordering::Relaxed);
    }
}
