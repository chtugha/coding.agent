use anyhow::Result;
use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use crate::log_forwarder::LogForwarder;

const CMD_POLL_TIMEOUT: Duration = Duration::from_millis(500);
const CMD_RECV_TIMEOUT: Duration = Duration::from_secs(2);
const CMD_BUF_SIZE: usize = 4096;

pub trait StatusProvider: Send + Sync {
    fn status_string(&self) -> String;
}

pub struct DefaultStatusProvider {
    pub model_path: String,
}

impl StatusProvider for DefaultStatusProvider {
    fn status_string(&self) -> String {
        format!("ACTIVE_CALLS:0:MODEL:{}:BATCH:0/0\n", self.model_path)
    }
}

pub fn run_cmd_listener(
    listener: TcpListener,
    running: Arc<AtomicBool>,
    log_forwarder: Arc<LogForwarder>,
    status_provider: Arc<dyn StatusProvider>,
) -> Result<()> {
    listener.set_nonblocking(true)?;
    let port = listener.local_addr().map(|a| a.port()).unwrap_or(0);
    tracing::info!("CMD listener on port {}", port);

    while running.load(Ordering::Relaxed) {
        let stream = match listener.accept() {
            Ok((s, _addr)) => s,
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(CMD_POLL_TIMEOUT);
                continue;
            }
            Err(e) => {
                tracing::warn!("CMD accept error: {}", e);
                std::thread::sleep(CMD_POLL_TIMEOUT);
                continue;
            }
        };

        if let Err(e) = stream.set_read_timeout(Some(CMD_RECV_TIMEOUT)) {
            tracing::warn!("CMD set_read_timeout failed: {}", e);
            continue;
        }

        let mut reader = BufReader::with_capacity(CMD_BUF_SIZE, &stream);
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) | Err(_) => continue,
            Ok(_) => {}
        }
        let cmd = line.trim_end_matches(|c| c == '\n' || c == '\r');
        let response = handle_command(cmd, &log_forwarder, &*status_provider);
        let mut writer = stream;
        let _ = writer.write_all(response.as_bytes());
        let _ = writer.flush();
    }

    tracing::info!("CMD listener shutting down");
    Ok(())
}

fn handle_command(
    cmd: &str,
    log_forwarder: &LogForwarder,
    status_provider: &dyn StatusProvider,
) -> String {
    if cmd == "PING" {
        return "PONG\n".to_string();
    }
    if let Some(level_str) = cmd.strip_prefix("SET_LOG_LEVEL:") {
        log_forwarder.set_level_str(level_str);
        return "OK\n".to_string();
    }
    if cmd == "STATUS" {
        return status_provider.status_string();
    }
    "ERROR:Unknown command\n".to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpStream;

    struct MockStatus;
    impl StatusProvider for MockStatus {
        fn status_string(&self) -> String {
            "ACTIVE_CALLS:3:MODEL:test.gguf:BATCH:3/8\n".to_string()
        }
    }

    #[test]
    fn handle_ping() {
        let fwd = Arc::new(LogForwarder::new("TEST"));
        let status = MockStatus;
        assert_eq!(handle_command("PING", &fwd, &status), "PONG\n");
    }

    #[test]
    fn handle_status() {
        let fwd = Arc::new(LogForwarder::new("TEST"));
        let status = MockStatus;
        let result = handle_command("STATUS", &fwd, &status);
        assert!(result.contains("ACTIVE_CALLS:3"));
        assert!(result.contains("test.gguf"));
    }

    #[test]
    fn handle_set_log_level() {
        let fwd = Arc::new(LogForwarder::new("TEST"));
        let status = MockStatus;
        let result = handle_command("SET_LOG_LEVEL:ERROR", &fwd, &status);
        assert_eq!(result, "OK\n");
    }

    #[test]
    fn handle_unknown_command() {
        let fwd = Arc::new(LogForwarder::new("TEST"));
        let status = MockStatus;
        let result = handle_command("FOOBAR", &fwd, &status);
        assert!(result.starts_with("ERROR:"));
    }

    #[test]
    fn cmd_listener_starts_and_stops() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();

        let running = Arc::new(AtomicBool::new(true));
        let fwd = Arc::new(LogForwarder::new("TEST"));
        let status: Arc<dyn StatusProvider> =
            Arc::new(DefaultStatusProvider { model_path: "test.gguf".into() });

        let r = running.clone();
        let f = fwd.clone();
        let s = status.clone();
        let handle = std::thread::spawn(move || {
            let _ = run_cmd_listener(listener, r, f, s);
        });

        let mut stream = TcpStream::connect(format!("127.0.0.1:{}", port)).unwrap();
        stream.set_write_timeout(Some(Duration::from_secs(5))).unwrap();
        stream.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        stream.write_all(b"PING\n").unwrap();
        stream.flush().unwrap();

        let mut reader = BufReader::new(&stream);
        let mut response = String::new();
        reader.read_line(&mut response).unwrap();
        assert_eq!(response.trim(), "PONG");

        running.store(false, Ordering::Relaxed);
        handle.join().unwrap();
    }
}
