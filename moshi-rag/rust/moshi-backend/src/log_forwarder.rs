use std::net::UdpSocket;
use std::sync::atomic::{AtomicU8, Ordering};

const FRONTEND_LOG_PORT: u16 = 22022;
const MAX_MSG_LEN: usize = 2048;
const MAX_DATAGRAM_LEN: usize = 2304;

fn truncate_to_char_boundary(s: &str, max_bytes: usize) -> &str {
    if s.len() <= max_bytes {
        return s;
    }
    let mut boundary = max_bytes;
    while boundary > 0 && !s.is_char_boundary(boundary) {
        boundary -= 1;
    }
    &s[..boundary]
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum LogLevel {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
}

impl LogLevel {
    pub fn as_str(self) -> &'static str {
        match self {
            LogLevel::Error => "ERROR",
            LogLevel::Warn => "WARN",
            LogLevel::Info => "INFO",
            LogLevel::Debug => "DEBUG",
        }
    }

    pub fn from_str_lossy(s: &str) -> Self {
        match s.trim().to_ascii_uppercase().as_str() {
            "ERROR" => LogLevel::Error,
            "WARN" | "WARNING" => LogLevel::Warn,
            "INFO" => LogLevel::Info,
            "DEBUG" | "TRACE" => LogLevel::Debug,
            _ => LogLevel::Info,
        }
    }
}

pub struct LogForwarder {
    socket: Option<UdpSocket>,
    service_name: &'static str,
    level: AtomicU8,
}

impl LogForwarder {
    pub fn new(service_name: &'static str) -> Self {
        let socket = UdpSocket::bind("0.0.0.0:0").ok().and_then(|s| {
            s.connect(format!("127.0.0.1:{}", FRONTEND_LOG_PORT)).ok()?;
            s.set_nonblocking(true).ok()?;
            Some(s)
        });
        Self {
            socket,
            service_name,
            level: AtomicU8::new(LogLevel::Info as u8),
        }
    }

    pub fn set_level(&self, level: LogLevel) {
        self.level.store(level as u8, Ordering::Relaxed);
    }

    pub fn set_level_str(&self, level_str: &str) {
        self.set_level(LogLevel::from_str_lossy(level_str));
    }

    pub fn forward(&self, level: LogLevel, call_id: u32, message: &str) {
        let socket = match &self.socket {
            Some(s) => s,
            None => return,
        };
        if (level as u8) > self.level.load(Ordering::Relaxed) {
            return;
        }
        let msg = truncate_to_char_boundary(message, MAX_MSG_LEN);
        let formatted = format!("{} {} {} {}", self.service_name, level.as_str(), call_id, msg);
        let bytes = formatted.as_bytes();
        let to_send = if bytes.len() > MAX_DATAGRAM_LEN { &bytes[..MAX_DATAGRAM_LEN] } else { bytes };
        let _ = socket.send(to_send);
    }
}

pub struct TracingLogLayer {
    forwarder: std::sync::Arc<LogForwarder>,
}

impl TracingLogLayer {
    pub fn new(forwarder: std::sync::Arc<LogForwarder>) -> Self {
        Self { forwarder }
    }
}

impl<S> tracing_subscriber::Layer<S> for TracingLogLayer
where
    S: tracing::Subscriber,
{
    fn on_event(
        &self,
        event: &tracing::Event<'_>,
        _ctx: tracing_subscriber::layer::Context<'_, S>,
    ) {
        let level = match *event.metadata().level() {
            tracing::Level::ERROR => LogLevel::Error,
            tracing::Level::WARN => LogLevel::Warn,
            tracing::Level::INFO => LogLevel::Info,
            tracing::Level::DEBUG | tracing::Level::TRACE => LogLevel::Debug,
        };
        let mut visitor = MessageVisitor::default();
        event.record(&mut visitor);
        let message = if visitor.message.is_empty() {
            event.metadata().name().to_string()
        } else {
            visitor.message
        };
        self.forwarder.forward(level, 0, &message);
    }
}

#[derive(Default)]
struct MessageVisitor {
    message: String,
}

impl tracing::field::Visit for MessageVisitor {
    fn record_debug(&mut self, field: &tracing::field::Field, value: &dyn std::fmt::Debug) {
        if field.name() == "message" {
            self.message = format!("{:?}", value);
        } else if self.message.is_empty() {
            self.message = format!("{}={:?}", field.name(), value);
        } else {
            self.message.push_str(&format!(" {}={:?}", field.name(), value));
        }
    }

    fn record_str(&mut self, field: &tracing::field::Field, value: &str) {
        if field.name() == "message" {
            self.message = value.to_string();
        } else if self.message.is_empty() {
            self.message = format!("{}={}", field.name(), value);
        } else {
            self.message.push_str(&format!(" {}={}", field.name(), value));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn log_level_ordering() {
        assert!(LogLevel::Error < LogLevel::Warn);
        assert!(LogLevel::Warn < LogLevel::Info);
        assert!(LogLevel::Info < LogLevel::Debug);
    }

    #[test]
    fn log_level_from_str() {
        assert_eq!(LogLevel::from_str_lossy("ERROR"), LogLevel::Error);
        assert_eq!(LogLevel::from_str_lossy("warn"), LogLevel::Warn);
        assert_eq!(LogLevel::from_str_lossy("INFO"), LogLevel::Info);
        assert_eq!(LogLevel::from_str_lossy("DEBUG"), LogLevel::Debug);
        assert_eq!(LogLevel::from_str_lossy("unknown"), LogLevel::Info);
    }

    #[test]
    fn forwarder_new_does_not_panic() {
        let fwd = LogForwarder::new("TEST_SERVICE");
        assert_eq!(fwd.service_name, "TEST_SERVICE");
    }

    #[test]
    fn forward_with_no_socket_does_not_panic() {
        let fwd = LogForwarder {
            socket: None,
            service_name: "TEST",
            level: AtomicU8::new(LogLevel::Info as u8),
        };
        fwd.forward(LogLevel::Info, 42, "test message");
    }

    #[test]
    fn set_level_str_works() {
        let fwd = LogForwarder::new("TEST");
        fwd.set_level_str("ERROR");
        assert_eq!(fwd.level.load(Ordering::Relaxed), LogLevel::Error as u8);
        fwd.set_level_str("debug");
        assert_eq!(fwd.level.load(Ordering::Relaxed), LogLevel::Debug as u8);
    }

    #[test]
    fn forward_filters_by_level() {
        let fwd = LogForwarder::new("TEST");
        fwd.set_level(LogLevel::Error);
        fwd.forward(LogLevel::Debug, 1, "should be filtered");
        fwd.forward(LogLevel::Error, 1, "should pass");
    }

    #[test]
    fn truncate_to_char_boundary_ascii() {
        let s = "hello world";
        assert_eq!(truncate_to_char_boundary(s, 5), "hello");
        assert_eq!(truncate_to_char_boundary(s, 100), s);
        assert_eq!(truncate_to_char_boundary(s, 0), "");
    }

    #[test]
    fn truncate_to_char_boundary_multibyte() {
        let s = "Grüße";
        assert_eq!(s.len(), 7);
        assert_eq!(truncate_to_char_boundary(s, 4), "Gr\u{fc}");
        assert_eq!(truncate_to_char_boundary(s, 3), "Gr");
        assert_eq!(truncate_to_char_boundary(s, 2), "Gr");
    }

    #[test]
    fn forward_with_multibyte_message_does_not_panic() {
        let fwd = LogForwarder::new("TEST");
        let long_german = "ü".repeat(MAX_MSG_LEN);
        fwd.forward(LogLevel::Info, 0, &long_german);
    }
}
