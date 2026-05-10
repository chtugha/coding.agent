// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

use anyhow::{bail, Context, Result};
use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
use socket2::{SockRef, TcpKeepalive};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream, ToSocketAddrs};
use std::time::Duration;

pub const UPSTREAM_MGMT_PORT: u16 = 13155;
pub const UPSTREAM_DATA_PORT: u16 = 13156;
pub const CMD_PORT: u16 = 13157;
pub const TEXT_PORT: u16 = 13158;

pub const OAP_MGMT_PORT: u16 = 13150;
pub const OAP_DATA_PORT: u16 = 13151;

pub const MAX_PAYLOAD_SIZE: u32 = 1024 * 1024;

const MAX_FRAME_LEN: u32 = MAX_PAYLOAD_SIZE + 256;
const CONNECT_TIMEOUT_MS: u64 = 2000;
const KEEPALIVE_IDLE_SECS: u64 = 2;
const KEEPALIVE_INTERVAL_SECS: u64 = 1;
const KEEPALIVE_RETRIES: u32 = 2;

const TYPE_CALL_END: u8 = 1;
const TYPE_SPEECH_ACTIVE: u8 = 2;
const TYPE_SPEECH_IDLE: u8 = 3;
const TYPE_PING: u8 = 4;
const TYPE_PONG: u8 = 5;
const TYPE_CUSTOM: u8 = 10;

#[derive(Debug, Clone, PartialEq)]
pub struct Packet {
    pub call_id: u32,
    pub payload: Vec<u8>,
}

impl Packet {
    pub fn new(call_id: u32, payload: Vec<u8>) -> Result<Self> {
        if call_id == 0 {
            bail!("call_id must not be 0");
        }
        if payload.len() as u64 > MAX_PAYLOAD_SIZE as u64 {
            bail!("payload size {} exceeds MAX_PAYLOAD_SIZE {}", payload.len(), MAX_PAYLOAD_SIZE);
        }
        Ok(Self { call_id, payload })
    }

    fn payload_size(&self) -> u32 {
        self.payload.len() as u32
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum MgmtMsg {
    CallEnd(u32),
    SpeechActive(u32),
    SpeechIdle(u32),
    Ping,
    Pong,
    Custom(String),
}

fn recv_exact_n<R: Read>(reader: &mut R, n: usize) -> Result<Vec<u8>> {
    let mut buf = vec![0u8; n];
    reader.read_exact(&mut buf).with_context(|| format!("failed to read {} bytes", n))?;
    Ok(buf)
}

fn recv_outer_frame<R: Read>(reader: &mut R) -> Result<Vec<u8>> {
    let frame_len = reader
        .read_u32::<BigEndian>()
        .context("failed to read 4-byte frame length")?;
    if frame_len > MAX_FRAME_LEN {
        bail!(
            "frame_len {} exceeds maximum {} — peer may have IC encryption enabled (not supported)",
            frame_len,
            MAX_FRAME_LEN
        );
    }
    recv_exact_n(reader, frame_len as usize).context("failed to read frame body")
}

fn send_outer_frame<W: Write>(writer: &mut W, inner: &[u8]) -> Result<()> {
    let mut frame = Vec::with_capacity(4 + inner.len());
    frame.write_u32::<BigEndian>(inner.len() as u32)?;
    frame.extend_from_slice(inner);
    writer.write_all(&frame).context("failed to write frame")?;
    Ok(())
}

pub fn send_packet<W: Write>(writer: &mut W, packet: &Packet) -> Result<()> {
    if packet.call_id == 0 {
        bail!("cannot send packet with call_id=0");
    }
    if packet.payload.len() > MAX_PAYLOAD_SIZE as usize {
        bail!("payload size {} exceeds MAX_PAYLOAD_SIZE {}", packet.payload.len(), MAX_PAYLOAD_SIZE);
    }
    let payload_size = packet.payload_size();
    let mut inner = Vec::with_capacity(8 + payload_size as usize);
    inner.write_u32::<BigEndian>(packet.call_id)?;
    inner.write_u32::<BigEndian>(payload_size)?;
    inner.extend_from_slice(&packet.payload);
    send_outer_frame(writer, &inner)
}

pub fn recv_packet<R: Read>(reader: &mut R) -> Result<Packet> {
    let inner = recv_outer_frame(reader)?;
    if inner.len() < 8 {
        bail!("packet frame too short: {} bytes (need at least 8)", inner.len());
    }
    let mut cursor = std::io::Cursor::new(&inner);
    let call_id = cursor.read_u32::<BigEndian>()?;
    let payload_size = cursor.read_u32::<BigEndian>()?;
    if call_id == 0 {
        bail!("received packet with call_id=0 (invalid)");
    }
    if payload_size > MAX_PAYLOAD_SIZE {
        bail!("payload_size {} exceeds MAX_PAYLOAD_SIZE {}", payload_size, MAX_PAYLOAD_SIZE);
    }
    let expected_len = 8 + payload_size as usize;
    if inner.len() < expected_len {
        bail!(
            "frame truncated: frame body is {} bytes but packet header claims {} bytes of payload",
            inner.len(),
            payload_size
        );
    }
    if inner.len() > expected_len {
        bail!(
            "frame body is {} bytes but packet header claims only {} bytes of payload — protocol violation",
            inner.len(),
            payload_size
        );
    }
    let payload = inner[8..].to_vec();
    Ok(Packet { call_id, payload })
}

pub fn send_mgmt<W: Write>(writer: &mut W, msg: &MgmtMsg) -> Result<()> {
    let inner = serialize_mgmt(msg)?;
    send_outer_frame(writer, &inner)
}

pub fn recv_mgmt<R: Read>(reader: &mut R) -> Result<MgmtMsg> {
    let inner = recv_outer_frame(reader)?;
    deserialize_mgmt(&inner)
}

fn serialize_mgmt(msg: &MgmtMsg) -> Result<Vec<u8>> {
    match msg {
        MgmtMsg::CallEnd(call_id) => {
            let mut buf = Vec::with_capacity(5);
            buf.push(TYPE_CALL_END);
            buf.write_u32::<BigEndian>(*call_id)?;
            Ok(buf)
        }
        MgmtMsg::SpeechActive(call_id) => {
            let mut buf = Vec::with_capacity(5);
            buf.push(TYPE_SPEECH_ACTIVE);
            buf.write_u32::<BigEndian>(*call_id)?;
            Ok(buf)
        }
        MgmtMsg::SpeechIdle(call_id) => {
            let mut buf = Vec::with_capacity(5);
            buf.push(TYPE_SPEECH_IDLE);
            buf.write_u32::<BigEndian>(*call_id)?;
            Ok(buf)
        }
        MgmtMsg::Ping => Ok(vec![TYPE_PING]),
        MgmtMsg::Pong => Ok(vec![TYPE_PONG]),
        MgmtMsg::Custom(s) => {
            let bytes = s.as_bytes();
            if bytes.len() > 65535 {
                bail!("CUSTOM message string too long: {} bytes (max 65535)", bytes.len());
            }
            let str_len = bytes.len() as u16;
            let mut buf = Vec::with_capacity(3 + bytes.len());
            buf.push(TYPE_CUSTOM);
            buf.write_u16::<BigEndian>(str_len)?;
            buf.extend_from_slice(bytes);
            Ok(buf)
        }
    }
}

fn deserialize_mgmt(inner: &[u8]) -> Result<MgmtMsg> {
    if inner.is_empty() {
        bail!("empty mgmt frame");
    }
    match inner[0] {
        TYPE_CALL_END | TYPE_SPEECH_ACTIVE | TYPE_SPEECH_IDLE => {
            if inner.len() < 5 {
                bail!("mgmt frame type {} too short: {} bytes (need 5)", inner[0], inner.len());
            }
            if inner.len() > 5 {
                bail!("mgmt frame type {} too long: {} bytes (expected 5)", inner[0], inner.len());
            }
            let mut cursor = std::io::Cursor::new(&inner[1..5]);
            let call_id = cursor.read_u32::<BigEndian>()?;
            Ok(match inner[0] {
                TYPE_CALL_END => MgmtMsg::CallEnd(call_id),
                TYPE_SPEECH_ACTIVE => MgmtMsg::SpeechActive(call_id),
                _ => MgmtMsg::SpeechIdle(call_id),
            })
        }
        TYPE_PING => {
            if inner.len() != 1 {
                bail!("PING mgmt frame wrong length: {} bytes (expected 1)", inner.len());
            }
            Ok(MgmtMsg::Ping)
        }
        TYPE_PONG => {
            if inner.len() != 1 {
                bail!("PONG mgmt frame wrong length: {} bytes (expected 1)", inner.len());
            }
            Ok(MgmtMsg::Pong)
        }
        TYPE_CUSTOM => {
            if inner.len() < 3 {
                bail!("CUSTOM mgmt frame too short: {} bytes (need at least 3)", inner.len());
            }
            let mut cursor = std::io::Cursor::new(&inner[1..3]);
            let str_len = cursor.read_u16::<BigEndian>()? as usize;
            if inner.len() < 3 + str_len {
                bail!(
                    "CUSTOM mgmt frame truncated: header claims {} str bytes, frame has {}",
                    str_len,
                    inner.len() - 3
                );
            }
            if inner.len() > 3 + str_len {
                bail!(
                    "CUSTOM mgmt frame has trailing bytes: {} bytes extra after string",
                    inner.len() - (3 + str_len)
                );
            }
            let s = std::str::from_utf8(&inner[3..3 + str_len])
                .context("CUSTOM msg payload is not valid UTF-8")?
                .to_string();
            Ok(MgmtMsg::Custom(s))
        }
        unknown => bail!("unknown mgmt message type byte: {} — caller should mark connection dead", unknown),
    }
}

fn set_keepalive_options(stream: &TcpStream) {
    let keepalive = TcpKeepalive::new()
        .with_time(Duration::from_secs(KEEPALIVE_IDLE_SECS))
        .with_interval(Duration::from_secs(KEEPALIVE_INTERVAL_SECS))
        .with_retries(KEEPALIVE_RETRIES);
    if let Err(e) = SockRef::from(stream).set_tcp_keepalive(&keepalive) {
        tracing::warn!("failed to set TCP keepalive options: {}", e);
    }
}

pub fn bind_listen(port: u16) -> Result<TcpListener> {
    let addr: std::net::SocketAddr = ([127, 0, 0, 1], port).into();
    let socket = socket2::Socket::new(
        socket2::Domain::IPV4,
        socket2::Type::STREAM,
        Some(socket2::Protocol::TCP),
    )
    .with_context(|| format!("failed to create TCP socket for port {}", port))?;
    socket
        .set_reuse_address(true)
        .with_context(|| format!("failed to set SO_REUSEADDR on port {}", port))?;
    socket
        .bind(&addr.into())
        .with_context(|| format!("failed to bind TCP listener on 127.0.0.1:{}", port))?;
    socket
        .listen(1)
        .with_context(|| format!("failed to listen on 127.0.0.1:{}", port))?;
    Ok(std::net::TcpListener::from(socket))
}

pub fn accept_connection(listener: &TcpListener) -> Result<TcpStream> {
    let (stream, addr) = listener.accept().context("failed to accept TCP connection")?;
    stream.set_nodelay(true).context("failed to set TCP_NODELAY on accepted connection")?;
    set_keepalive_options(&stream);
    tracing::debug!("accepted connection from {}", addr);
    Ok(stream)
}

pub fn connect_to(host: &str, port: u16) -> Result<TcpStream> {
    let addr = (host, port)
        .to_socket_addrs()
        .with_context(|| format!("failed to resolve {}:{}", host, port))?
        .next()
        .ok_or_else(|| anyhow::anyhow!("no addresses resolved for {}:{}", host, port))?;
    let stream =
        TcpStream::connect_timeout(&addr, Duration::from_millis(CONNECT_TIMEOUT_MS))
            .with_context(|| format!("failed to connect to {}:{}", host, port))?;
    stream.set_nodelay(true).context("failed to set TCP_NODELAY")?;
    set_keepalive_options(&stream);
    Ok(stream)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip_packet(pkt: &Packet) -> Result<Packet> {
        let mut buf = Vec::new();
        send_packet(&mut buf, pkt)?;
        let mut cursor = std::io::Cursor::new(buf);
        recv_packet(&mut cursor)
    }

    fn roundtrip_mgmt(msg: &MgmtMsg) -> Result<MgmtMsg> {
        let mut buf = Vec::new();
        send_mgmt(&mut buf, msg)?;
        let mut cursor = std::io::Cursor::new(buf);
        recv_mgmt(&mut cursor)
    }

    #[test]
    fn packet_roundtrip_with_payload() {
        let pkt = Packet { call_id: 42, payload: b"hello world".to_vec() };
        let received = roundtrip_packet(&pkt).unwrap();
        assert_eq!(received, pkt);
    }

    #[test]
    fn packet_roundtrip_empty_payload() {
        let pkt = Packet { call_id: 1, payload: vec![] };
        let received = roundtrip_packet(&pkt).unwrap();
        assert_eq!(received, pkt);
    }

    #[test]
    fn packet_roundtrip_binary_payload() {
        let payload: Vec<u8> = (0..480u32).flat_map(|i| i.to_le_bytes()).collect();
        let pkt = Packet { call_id: 0xDEADBEEF, payload };
        let received = roundtrip_packet(&pkt).unwrap();
        assert_eq!(received, pkt);
    }

    #[test]
    fn packet_call_id_zero_rejected_on_send() {
        let pkt = Packet { call_id: 0, payload: vec![1, 2, 3] };
        assert!(send_packet(&mut Vec::new(), &pkt).is_err());
    }

    #[test]
    fn packet_call_id_zero_rejected_on_recv() {
        let pkt_zero_cid = Packet { call_id: 1, payload: b"test".to_vec() };
        let mut buf = Vec::new();
        send_packet(&mut buf, &pkt_zero_cid).unwrap();
        buf[4] = 0;
        buf[5] = 0;
        buf[6] = 0;
        buf[7] = 0;
        let mut cursor = std::io::Cursor::new(buf);
        assert!(recv_packet(&mut cursor).is_err());
    }

    #[test]
    fn packet_new_validates_call_id_zero() {
        assert!(Packet::new(0, vec![]).is_err());
    }

    #[test]
    fn packet_new_validates_max_payload_size() {
        assert!(Packet::new(1, vec![0u8; MAX_PAYLOAD_SIZE as usize + 1]).is_err());
    }

    #[test]
    fn packet_frame_wire_format() {
        let pkt = Packet { call_id: 1, payload: vec![0xAA, 0xBB] };
        let mut buf = Vec::new();
        send_packet(&mut buf, &pkt).unwrap();
        assert_eq!(buf[0..4], [0, 0, 0, 10], "outer frame_len should be 10 (8+2)");
        assert_eq!(buf[4..8], [0, 0, 0, 1], "call_id should be big-endian 1");
        assert_eq!(buf[8..12], [0, 0, 0, 2], "payload_size should be big-endian 2");
        assert_eq!(buf[12..14], [0xAA, 0xBB], "payload bytes");
    }

    #[test]
    fn mgmt_roundtrip_call_end() {
        let msg = MgmtMsg::CallEnd(12345);
        assert_eq!(roundtrip_mgmt(&msg).unwrap(), msg);
    }

    #[test]
    fn mgmt_roundtrip_speech_active() {
        let msg = MgmtMsg::SpeechActive(999);
        assert_eq!(roundtrip_mgmt(&msg).unwrap(), msg);
    }

    #[test]
    fn mgmt_roundtrip_speech_idle() {
        let msg = MgmtMsg::SpeechIdle(777);
        assert_eq!(roundtrip_mgmt(&msg).unwrap(), msg);
    }

    #[test]
    fn mgmt_roundtrip_ping() {
        assert_eq!(roundtrip_mgmt(&MgmtMsg::Ping).unwrap(), MgmtMsg::Ping);
    }

    #[test]
    fn mgmt_roundtrip_pong() {
        assert_eq!(roundtrip_mgmt(&MgmtMsg::Pong).unwrap(), MgmtMsg::Pong);
    }

    #[test]
    fn mgmt_roundtrip_custom_nonempty() {
        let msg = MgmtMsg::Custom("SAMPLE_RATE_QUERY".to_string());
        assert_eq!(roundtrip_mgmt(&msg).unwrap(), msg);
    }

    #[test]
    fn mgmt_roundtrip_custom_empty() {
        let msg = MgmtMsg::Custom(String::new());
        assert_eq!(roundtrip_mgmt(&msg).unwrap(), msg);
    }

    #[test]
    fn mgmt_custom_response_roundtrip() {
        let msg = MgmtMsg::Custom("SAMPLE_RATE:24000".to_string());
        assert_eq!(roundtrip_mgmt(&msg).unwrap(), msg);
    }

    #[test]
    fn mgmt_unknown_type_returns_error() {
        let inner = vec![0xFFu8];
        assert!(deserialize_mgmt(&inner).is_err());
    }

    #[test]
    fn mgmt_type_6_is_unknown_not_custom() {
        let inner = vec![6u8, 0, 0];
        assert!(
            deserialize_mgmt(&inner).is_err(),
            "type byte 6 is not a valid MgmtMsgType (CUSTOM=10, not 6)"
        );
    }

    #[test]
    fn mgmt_empty_frame_returns_error() {
        assert!(deserialize_mgmt(&[]).is_err());
    }

    #[test]
    fn mgmt_ping_wire_format() {
        let mut buf = Vec::new();
        send_mgmt(&mut buf, &MgmtMsg::Ping).unwrap();
        assert_eq!(buf[0..4], [0, 0, 0, 1], "outer frame_len should be 1");
        assert_eq!(buf[4], TYPE_PING, "inner byte should be PING type");
    }

    #[test]
    fn mgmt_call_end_wire_format() {
        let mut buf = Vec::new();
        send_mgmt(&mut buf, &MgmtMsg::CallEnd(1)).unwrap();
        assert_eq!(buf[0..4], [0, 0, 0, 5], "outer frame_len should be 5");
        assert_eq!(buf[4], TYPE_CALL_END);
        assert_eq!(buf[5..9], [0, 0, 0, 1], "call_id big-endian 1");
    }

    #[test]
    fn mgmt_custom_wire_format() {
        let mut buf = Vec::new();
        send_mgmt(&mut buf, &MgmtMsg::Custom("HI".to_string())).unwrap();
        assert_eq!(buf[0..4], [0, 0, 0, 5], "outer frame_len should be 5 (1+2+2)");
        assert_eq!(buf[4], TYPE_CUSTOM);
        assert_eq!(buf[5..7], [0, 2], "str len big-endian 2");
        assert_eq!(&buf[7..9], b"HI");
    }

    #[test]
    fn packet_trailing_bytes_rejected() {
        let pkt = Packet { call_id: 7, payload: b"ab".to_vec() };
        let mut buf = Vec::new();
        send_packet(&mut buf, &pkt).unwrap();
        buf.push(0xFF);
        buf[0..4].copy_from_slice(&(11u32).to_be_bytes());
        let mut cursor = std::io::Cursor::new(buf);
        assert!(
            recv_packet(&mut cursor).is_err(),
            "packet with trailing bytes must be rejected"
        );
    }

    #[test]
    fn mgmt_call_end_trailing_bytes_rejected() {
        let inner = vec![TYPE_CALL_END, 0, 0, 0, 1, 0xFF];
        assert!(
            deserialize_mgmt(&inner).is_err(),
            "CallEnd with trailing byte must be rejected"
        );
    }

    #[test]
    fn mgmt_ping_trailing_bytes_rejected() {
        let inner = vec![TYPE_PING, 0xFF];
        assert!(
            deserialize_mgmt(&inner).is_err(),
            "PING with trailing byte must be rejected"
        );
    }

    #[test]
    fn mgmt_custom_trailing_bytes_rejected() {
        let inner = vec![TYPE_CUSTOM, 0, 2, b'H', b'I', 0xFF];
        assert!(
            deserialize_mgmt(&inner).is_err(),
            "CUSTOM with trailing byte must be rejected"
        );
    }

    #[test]
    fn mgmt_custom_too_long_string_rejected_on_send() {
        let long_str = "x".repeat(65536);
        let msg = MgmtMsg::Custom(long_str);
        assert!(
            serialize_mgmt(&msg).is_err(),
            "CUSTOM string > 65535 bytes must be rejected, not silently truncated"
        );
    }

    #[test]
    fn max_payload_size_matches_cpp() {
        assert_eq!(MAX_PAYLOAD_SIZE, 1024 * 1024, "must match C++ MAX_PAYLOAD_SIZE");
    }

    #[test]
    fn port_constants_match_spec() {
        assert_eq!(UPSTREAM_MGMT_PORT, 13155);
        assert_eq!(UPSTREAM_DATA_PORT, 13156);
        assert_eq!(CMD_PORT, 13157);
        assert_eq!(TEXT_PORT, 13158);
        assert_eq!(OAP_MGMT_PORT, 13150);
        assert_eq!(OAP_DATA_PORT, 13151);
    }
}
