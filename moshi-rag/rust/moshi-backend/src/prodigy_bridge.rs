use std::collections::{HashMap, HashSet};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{Context, Result};

use crate::batched_channels::{BatchedStreamingChannels, InMsg};
use crate::interconnect::{
    accept_connection, bind_listen, connect_to, recv_mgmt, recv_packet, send_mgmt, send_packet,
    MgmtMsg, Packet, OAP_DATA_PORT, OAP_MGMT_PORT, UPSTREAM_DATA_PORT, UPSTREAM_MGMT_PORT,
};
use crate::stream_both::StreamOut;

const INACTIVITY_TIMEOUT: Duration = Duration::from_secs(60);
const OAP_RECONNECT_INTERVAL: Duration = Duration::from_millis(200);
const UPSTREAM_ACCEPT_TIMEOUT: Duration = Duration::from_secs(1);
const UPSTREAM_RECV_TIMEOUT: Duration = Duration::from_secs(2);
const OUTPUT_POLL_INTERVAL: Duration = Duration::from_millis(1);
const INACTIVITY_CHECK_INTERVAL: Duration = Duration::from_secs(5);

struct SlotInfo {
    in_tx: std::sync::mpsc::Sender<InMsg>,
    last_activity: Instant,
}

struct BridgeInner {
    call_to_slot: HashMap<u32, usize>,
    slot_to_call: Vec<Option<u32>>,
    slot_info: Vec<Option<SlotInfo>>,
    rejected: HashSet<u32>,
    batch_size: usize,
}

impl BridgeInner {
    fn new(batch_size: usize) -> Self {
        Self {
            call_to_slot: HashMap::new(),
            slot_to_call: vec![None; batch_size],
            slot_info: (0..batch_size).map(|_| None).collect(),
            rejected: HashSet::new(),
            batch_size,
        }
    }

    fn release_slot(&mut self, call_id: u32) {
        self.rejected.remove(&call_id);
        if let Some(batch_idx) = self.call_to_slot.remove(&call_id) {
            if batch_idx < self.batch_size {
                self.slot_to_call[batch_idx] = None;
                self.slot_info[batch_idx] = None;
            }
        }
    }

    fn call_id_for_slot(&self, batch_idx: usize) -> Option<u32> {
        self.slot_to_call.get(batch_idx).copied().flatten()
    }

    fn active_calls(&self) -> usize {
        self.call_to_slot.len()
    }

    fn slots_used(&self) -> usize {
        self.slot_info.iter().filter(|s| s.is_some()).count()
    }
}

enum OutputEvent {
    NewSlot {
        batch_idx: usize,
        call_id: u32,
        out_rx: tokio::sync::mpsc::UnboundedReceiver<StreamOut>,
    },
    SlotInvalidated {
        batch_idx: usize,
        call_id: u32,
    },
}

struct OapState {
    data_stream: Option<TcpStream>,
    mgmt_stream: Option<TcpStream>,
}

impl OapState {
    fn new() -> Self {
        Self {
            data_stream: None,
            mgmt_stream: None,
        }
    }

    fn is_connected(&self) -> bool {
        self.data_stream.is_some() && self.mgmt_stream.is_some()
    }

    fn disconnect(&mut self) {
        self.data_stream = None;
        self.mgmt_stream = None;
    }
}

pub struct BridgeStatusProvider {
    inner: Arc<Mutex<BridgeInner>>,
    oap: Arc<Mutex<OapState>>,
    model_path: String,
    model_loop_ready: Arc<AtomicBool>,
}

impl crate::cmd_port::StatusProvider for BridgeStatusProvider {
    fn status_string(&self) -> String {
        let inner = self.inner.lock().unwrap();
        let oap = self.oap.lock().unwrap();
        let oap_status = if oap.is_connected() { "CONNECTED" } else { "DISCONNECTED" };
        let loop_ready = if self.model_loop_ready.load(Ordering::Relaxed) { "YES" } else { "NO" };
        format!(
            "ACTIVE_CALLS:{}:MODEL:{}:BATCH:{}/{}:OAP:{}:MODEL_LOOP_READY:{}\n",
            inner.active_calls(),
            self.model_path,
            inner.slots_used(),
            inner.batch_size,
            oap_status,
            loop_ready
        )
    }
}

pub fn run_prodigy_bridge(
    pool: Arc<BatchedStreamingChannels>,
    running: Arc<AtomicBool>,
    batch_size: usize,
    model_path: String,
    log_forwarder: Arc<crate::log_forwarder::LogForwarder>,
    model_loop_ready: Arc<AtomicBool>,
) -> Result<Arc<dyn crate::cmd_port::StatusProvider>> {
    let inner = Arc::new(Mutex::new(BridgeInner::new(batch_size)));
    let (mgmt_drain_tx, mgmt_drain_rx) = std::sync::mpsc::sync_channel::<TcpStream>(1);
    let oap = Arc::new(Mutex::new(OapState::new()));
    let (output_event_tx, output_event_rx) = std::sync::mpsc::channel::<OutputEvent>();

    let status_provider: Arc<dyn crate::cmd_port::StatusProvider> =
        Arc::new(BridgeStatusProvider {
            inner: inner.clone(),
            oap: oap.clone(),
            model_path,
            model_loop_ready,
        });

    let oap_c = oap.clone();
    let r = running.clone();
    let _oap_handle = std::thread::Builder::new()
        .name("oap-reconnect".into())
        .spawn(move || oap_reconnect_loop(oap_c, mgmt_drain_tx, r))
        .context("failed to spawn oap-reconnect thread")?;

    let r = running.clone();
    let _drain_handle = std::thread::Builder::new()
        .name("oap-mgmt-drain".into())
        .spawn(move || oap_mgmt_drain_loop(mgmt_drain_rx, r))
        .context("failed to spawn oap-mgmt-drain thread")?;

    let oap_c = oap.clone();
    let inner_c = inner.clone();
    let r = running.clone();
    let fwd_c = log_forwarder.clone();
    let _output_handle = std::thread::Builder::new()
        .name("output-drain".into())
        .spawn(move || output_drain_loop(output_event_rx, oap_c, inner_c, r, fwd_c))
        .context("failed to spawn output-drain thread")?;

    let inner_c = inner.clone();
    let output_tx_inact = output_event_tx.clone();
    let r = running.clone();
    let _inactivity_handle = std::thread::Builder::new()
        .name("inactivity-check".into())
        .spawn(move || inactivity_check_loop(inner_c, output_tx_inact, r))
        .context("failed to spawn inactivity-check thread")?;

    let inner_c = inner.clone();
    let r = running.clone();
    let _ = log_forwarder;
    std::thread::Builder::new()
        .name("upstream-accept".into())
        .spawn(move || {
            if let Err(e) = upstream_accept_loop(pool, inner_c, oap.clone(), output_event_tx, r) {
                tracing::error!("upstream accept loop fatal: {}", e);
            }
        })
        .context("failed to spawn upstream-accept thread")?;

    Ok(status_provider)
}

fn is_socket_dead(stream: &TcpStream) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::io::AsRawFd;
        let fd = stream.as_raw_fd();
        let mut pollfd = libc::pollfd {
            fd,
            events: libc::POLLIN | libc::POLLHUP | libc::POLLERR,
            revents: 0,
        };
        let ret = unsafe { libc::poll(&mut pollfd, 1, 0) };
        if ret < 0 {
            return true;
        }
        if ret == 0 {
            return false;
        }
        if (pollfd.revents & (libc::POLLHUP | libc::POLLERR)) != 0 {
            return true;
        }
        if (pollfd.revents & libc::POLLIN) != 0 {
            let mut buf = [0u8; 1];
            let n = unsafe {
                libc::recv(
                    fd,
                    buf.as_mut_ptr() as *mut libc::c_void,
                    1,
                    libc::MSG_PEEK | libc::MSG_DONTWAIT,
                )
            };
            return n == 0;
        }
        false
    }
    #[cfg(not(unix))]
    {
        let _ = stream;
        false
    }
}

fn oap_reconnect_loop(
    oap: Arc<Mutex<OapState>>,
    mgmt_drain_tx: std::sync::mpsc::SyncSender<TcpStream>,
    running: Arc<AtomicBool>,
) {
    tracing::info!("OAP reconnect thread started (mgmt:{}, data:{})", OAP_MGMT_PORT, OAP_DATA_PORT);
    let mut last_log = Instant::now() - Duration::from_secs(30);

    while running.load(Ordering::Relaxed) {
        let needs_connect = {
            let mut state = oap.lock().unwrap();
            if state.is_connected() {
                let dd = state.data_stream.as_ref().is_none_or(is_socket_dead);
                let md = state.mgmt_stream.as_ref().is_none_or(is_socket_dead);
                if dd || md {
                    tracing::warn!("OAP connection dead (data_dead={}, mgmt_dead={}), disconnecting", dd, md);
                    state.disconnect();
                    true
                } else {
                    false
                }
            } else {
                true
            }
        };

        if needs_connect {
            match (
                connect_to("127.0.0.1", OAP_MGMT_PORT),
                connect_to("127.0.0.1", OAP_DATA_PORT),
            ) {
                (Ok(mgmt), Ok(data)) => {
                    tracing::info!("Connected to OAP (mgmt:{}, data:{})", OAP_MGMT_PORT, OAP_DATA_PORT);
                    let maybe_clone = mgmt.try_clone();
                    {
                        let mut state = oap.lock().unwrap();
                        state.mgmt_stream = Some(mgmt);
                        state.data_stream = Some(data);
                    }
                    match maybe_clone {
                        Ok(reader_clone) => {
                            if mgmt_drain_tx.send(reader_clone).is_err() {
                                tracing::error!("OAP mgmt drain thread exited, cannot drain OAP mgmt channel");
                            }
                        }
                        Err(e) => {
                            tracing::warn!("OAP mgmt try_clone failed: {}", e);
                        }
                    }
                }
                _ => {
                    if last_log.elapsed() > Duration::from_secs(10) {
                        tracing::debug!("OAP not available yet, will retry");
                        last_log = Instant::now();
                    }
                }
            }
        }

        std::thread::sleep(OAP_RECONNECT_INTERVAL);
    }
    tracing::info!("OAP reconnect thread stopped");
}

fn oap_mgmt_drain_loop(
    stream_rx: std::sync::mpsc::Receiver<TcpStream>,
    running: Arc<AtomicBool>,
) {
    tracing::info!("OAP mgmt drain thread started");
    let mut current: Option<std::io::BufReader<TcpStream>> = None;
    while running.load(Ordering::Relaxed) {
        match stream_rx.try_recv() {
            Ok(stream) => {
                if let Err(e) = stream.set_read_timeout(Some(Duration::from_secs(2))) {
                    tracing::warn!("OAP mgmt drain: set_read_timeout failed: {}", e);
                }
                current = Some(std::io::BufReader::new(stream));
                tracing::debug!("OAP mgmt drain: got new stream");
            }
            Err(std::sync::mpsc::TryRecvError::Disconnected) => break,
            Err(std::sync::mpsc::TryRecvError::Empty) => {}
        }

        if let Some(ref mut reader) = current {
            match crate::interconnect::recv_mgmt(reader) {
                Ok(msg) => {
                    tracing::debug!("OAP mgmt drain: discarding {:?}", msg);
                }
                Err(e) => {
                    if !is_io_timeout(&e) {
                        tracing::debug!("OAP mgmt drain: stream ended ({}), waiting for next", e);
                        current = None;
                    }
                }
            }
        } else {
            std::thread::sleep(Duration::from_millis(50));
        }
    }
    tracing::info!("OAP mgmt drain thread stopped");
}

fn output_drain_loop(
    event_rx: std::sync::mpsc::Receiver<OutputEvent>,
    oap: Arc<Mutex<OapState>>,
    inner: Arc<Mutex<BridgeInner>>,
    running: Arc<AtomicBool>,
    log_forwarder: Arc<crate::log_forwarder::LogForwarder>,
) {
    tracing::info!("output drain thread started");

    struct ActiveOutput {
        out_rx: tokio::sync::mpsc::UnboundedReceiver<StreamOut>,
        call_id: u32,
    }

    let mut outputs: HashMap<usize, ActiveOutput> = HashMap::new();

    while running.load(Ordering::Relaxed) {
        while let Ok(event) = event_rx.try_recv() {
            match event {
                OutputEvent::NewSlot {
                    batch_idx,
                    call_id,
                    out_rx,
                } => {
                    outputs.insert(batch_idx, ActiveOutput { out_rx, call_id });
                }
                OutputEvent::SlotInvalidated { batch_idx, call_id } => {
                    if outputs.get(&batch_idx).is_some_and(|a| a.call_id == call_id) {
                        outputs.remove(&batch_idx);
                    }
                }
            }
        }

        let mut had_output = false;
        let mut dead_slots = Vec::new();

        for (&batch_idx, active) in outputs.iter_mut() {
            loop {
                match active.out_rx.try_recv() {
                    Ok(StreamOut::Pcm { pcm }) => {
                        let call_id = {
                            let inner_guard = inner.lock().unwrap();
                            inner_guard.call_id_for_slot(batch_idx)
                        };
                        let call_id = match call_id {
                            Some(cid) if cid == active.call_id => cid,
                            _ => continue,
                        };

                        let payload: Vec<u8> = pcm
                            .iter()
                            .flat_map(|&f| f.to_le_bytes())
                            .collect();

                        if let Ok(pkt) = Packet::new(call_id, payload) {
                            let mut oap_guard = oap.lock().unwrap();
                            if let Some(ref mut data_stream) = oap_guard.data_stream {
                                if send_packet(data_stream, &pkt).is_err() {
                                    tracing::warn!("OAP data send failed, disconnecting");
                                    oap_guard.disconnect();
                                }
                            }
                        }
                        had_output = true;
                    }
                    Ok(StreamOut::Ready) => {}
                    Ok(StreamOut::TextByRole { text, role }) => {
                        if role == crate::turn_manager::TextRole::Model && !text.is_empty() {
                            log_forwarder.forward(
                                crate::log_forwarder::LogLevel::Info,
                                active.call_id,
                                &format!("Moshi transcription: {}", text),
                            );
                        }
                    }
                    Ok(_) => {}
                    Err(tokio::sync::mpsc::error::TryRecvError::Empty) => break,
                    Err(tokio::sync::mpsc::error::TryRecvError::Disconnected) => {
                        dead_slots.push(batch_idx);
                        break;
                    }
                }
            }
        }

        for slot in dead_slots {
            outputs.remove(&slot);
        }

        if !had_output {
            std::thread::sleep(OUTPUT_POLL_INTERVAL);
        }
    }
    tracing::info!("output drain thread stopped");
}

fn inactivity_check_loop(
    inner: Arc<Mutex<BridgeInner>>,
    output_event_tx: std::sync::mpsc::Sender<OutputEvent>,
    running: Arc<AtomicBool>,
) {
    tracing::info!("inactivity checker started");

    while running.load(Ordering::Relaxed) {
        std::thread::sleep(INACTIVITY_CHECK_INTERVAL);
        if !running.load(Ordering::Relaxed) {
            break;
        }

        let now = Instant::now();
        let mut timed_out = Vec::new();

        {
            let guard = inner.lock().unwrap();
            for (&call_id, &batch_idx) in &guard.call_to_slot {
                if let Some(ref info) = guard.slot_info[batch_idx] {
                    if now.duration_since(info.last_activity) > INACTIVITY_TIMEOUT {
                        timed_out.push((call_id, batch_idx));
                    }
                }
            }
        }

        for (call_id, batch_idx) in timed_out {
            tracing::warn!(call_id, "inactivity timeout (60s), releasing slot");
            let _ = output_event_tx.send(OutputEvent::SlotInvalidated { batch_idx, call_id });
            let mut guard = inner.lock().unwrap();
            guard.release_slot(call_id);
        }
    }
    tracing::info!("inactivity checker stopped");
}

fn forward_call_end_to_oap(oap: &Arc<Mutex<OapState>>, call_id: u32) {
    let mut oap_guard = oap.lock().unwrap();
    if let Some(ref mut mgmt_stream) = oap_guard.mgmt_stream {
        if send_mgmt(mgmt_stream, &MgmtMsg::CallEnd(call_id)).is_err() {
            tracing::warn!("OAP mgmt send (CALL_END) failed, disconnecting");
            oap_guard.disconnect();
        }
    }
}

fn upstream_accept_loop(
    pool: Arc<BatchedStreamingChannels>,
    inner: Arc<Mutex<BridgeInner>>,
    oap: Arc<Mutex<OapState>>,
    output_event_tx: std::sync::mpsc::Sender<OutputEvent>,
    running: Arc<AtomicBool>,
) -> Result<()> {
    let mgmt_listener = bind_listen(UPSTREAM_MGMT_PORT)
        .context("failed to bind upstream mgmt listener")?;
    let data_listener = bind_listen(UPSTREAM_DATA_PORT)
        .context("failed to bind upstream data listener")?;

    {
        use socket2::SockRef;
        SockRef::from(&mgmt_listener).set_read_timeout(Some(UPSTREAM_ACCEPT_TIMEOUT))?;
        SockRef::from(&data_listener).set_read_timeout(Some(UPSTREAM_ACCEPT_TIMEOUT))?;
    }

    tracing::info!(
        "upstream accept loop: listening on mgmt:{}, data:{}",
        UPSTREAM_MGMT_PORT,
        UPSTREAM_DATA_PORT
    );

    while running.load(Ordering::Relaxed) {
        let mgmt_stream = loop {
            if !running.load(Ordering::Relaxed) {
                return Ok(());
            }
            match accept_connection(&mgmt_listener) {
                Ok(s) => break s,
                Err(e) => {
                    if is_io_timeout(&e) {
                        continue;
                    }
                    tracing::warn!("upstream mgmt accept error: {}, retrying", e);
                    std::thread::sleep(Duration::from_millis(200));
                }
            }
        };
        tracing::info!("upstream mgmt connection accepted");

        let data_stream = loop {
            if !running.load(Ordering::Relaxed) {
                return Ok(());
            }
            match accept_connection(&data_listener) {
                Ok(s) => break s,
                Err(e) => {
                    if is_io_timeout(&e) {
                        continue;
                    }
                    tracing::warn!("upstream data accept error: {}, retrying", e);
                    std::thread::sleep(Duration::from_millis(200));
                }
            }
        };
        tracing::info!("upstream data connection accepted");

        if let Err(e) = mgmt_stream.set_read_timeout(Some(UPSTREAM_RECV_TIMEOUT)) {
            tracing::warn!("failed to set mgmt read timeout: {}", e);
        }
        if let Err(e) = data_stream.set_read_timeout(Some(UPSTREAM_RECV_TIMEOUT)) {
            tracing::warn!("failed to set data read timeout: {}", e);
        }

        let mgmt_dead = Arc::new(AtomicBool::new(false));
        let data_dead = Arc::new(AtomicBool::new(false));

        let inner_m = inner.clone();
        let oap_m = oap.clone();
        let output_tx_m = output_event_tx.clone();
        let r_m = running.clone();
        let md = mgmt_dead.clone();
        let mgmt_handle = std::thread::Builder::new()
            .name("upstream-mgmt".into())
            .spawn(move || {
                mgmt_receive_loop(mgmt_stream, inner_m, oap_m, output_tx_m, r_m);
                md.store(true, Ordering::SeqCst);
            })
            .context("failed to spawn mgmt receive thread")?;

        let inner_d = inner.clone();
        let pool_d = pool.clone();
        let r_d = running.clone();
        let dd = data_dead.clone();
        let output_tx = output_event_tx.clone();
        let data_handle = std::thread::Builder::new()
            .name("upstream-data".into())
            .spawn(move || {
                data_receive_loop(data_stream, pool_d, inner_d, output_tx, r_d);
                dd.store(true, Ordering::SeqCst);
            })
            .context("failed to spawn data receive thread")?;

        while running.load(Ordering::Relaxed) {
            if mgmt_dead.load(Ordering::Relaxed) || data_dead.load(Ordering::Relaxed) {
                tracing::warn!("upstream connection lost, will re-accept");
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }

        let _ = mgmt_handle.join();
        let _ = data_handle.join();

        tracing::info!("upstream session ended, cleaning up all active calls");
        cleanup_all_calls(&inner, &oap, &output_event_tx);
    }

    Ok(())
}

fn cleanup_all_calls(
    inner: &Arc<Mutex<BridgeInner>>,
    oap: &Arc<Mutex<OapState>>,
    output_event_tx: &std::sync::mpsc::Sender<OutputEvent>,
) {
    let call_slots: Vec<(u32, usize)> = {
        let guard = inner.lock().unwrap();
        guard.call_to_slot.iter().map(|(&cid, &bid)| (cid, bid)).collect()
    };
    for (call_id, batch_idx) in call_slots {
        let _ = output_event_tx.send(OutputEvent::SlotInvalidated { batch_idx, call_id });
        inner.lock().unwrap().release_slot(call_id);
        forward_call_end_to_oap(oap, call_id);
    }
    inner.lock().unwrap().rejected.clear();
}

fn mgmt_receive_loop(
    stream: TcpStream,
    inner: Arc<Mutex<BridgeInner>>,
    oap: Arc<Mutex<OapState>>,
    output_event_tx: std::sync::mpsc::Sender<OutputEvent>,
    running: Arc<AtomicBool>,
) {
    tracing::info!("upstream mgmt receive loop started");
    let mut reader = std::io::BufReader::new(&stream);
    let mut writer = &stream;

    while running.load(Ordering::Relaxed) {
        let msg = match recv_mgmt(&mut reader) {
            Ok(msg) => msg,
            Err(e) => {
                if is_io_timeout(&e) {
                    if is_socket_dead(&stream) {
                        tracing::warn!("upstream mgmt socket dead");
                        break;
                    }
                    continue;
                }
                if running.load(Ordering::Relaxed) {
                    tracing::warn!("upstream mgmt recv error: {}", e);
                }
                break;
            }
        };

        match msg {
            MgmtMsg::CallEnd(call_id) => {
                tracing::info!(call_id, "CALL_END received");
                let batch_idx = {
                    let guard = inner.lock().unwrap();
                    guard.call_to_slot.get(&call_id).copied()
                };
                if let Some(bid) = batch_idx {
                    let _ = output_event_tx.send(OutputEvent::SlotInvalidated { batch_idx: bid, call_id });
                }
                inner.lock().unwrap().release_slot(call_id);
                forward_call_end_to_oap(&oap, call_id);
            }
            MgmtMsg::SpeechActive(_) | MgmtMsg::SpeechIdle(_) => {}
            MgmtMsg::Ping => {
                if let Err(e) = send_mgmt(&mut writer, &MgmtMsg::Pong) {
                    tracing::warn!("failed to send PONG to upstream: {}", e);
                    break;
                }
            }
            MgmtMsg::Pong => {}
            MgmtMsg::Custom(ref s) => {
                if s == "SAMPLE_RATE_QUERY" {
                    let response = MgmtMsg::Custom("SAMPLE_RATE:24000".to_string());
                    if let Err(e) = send_mgmt(&mut writer, &response) {
                        tracing::warn!("failed to send SAMPLE_RATE response: {}", e);
                        break;
                    }
                } else {
                    tracing::debug!("received CUSTOM mgmt: {}", s);
                }
            }
        }
    }

    tracing::info!("upstream mgmt receive loop stopped");
}

fn data_receive_loop(
    stream: TcpStream,
    pool: Arc<BatchedStreamingChannels>,
    inner: Arc<Mutex<BridgeInner>>,
    output_event_tx: std::sync::mpsc::Sender<OutputEvent>,
    running: Arc<AtomicBool>,
) {
    tracing::info!("upstream data receive loop started");
    let mut reader = std::io::BufReader::new(&stream);

    while running.load(Ordering::Relaxed) {
        let packet = match recv_packet(&mut reader) {
            Ok(pkt) => pkt,
            Err(e) => {
                if is_io_timeout(&e) {
                    if is_socket_dead(&stream) {
                        tracing::warn!("upstream data socket dead");
                        break;
                    }
                    continue;
                }
                if running.load(Ordering::Relaxed) {
                    tracing::warn!("upstream data recv error: {}", e);
                }
                break;
            }
        };

        let call_id = packet.call_id;

        let known_opt: Option<bool> = {
            let guard = inner.lock().unwrap();
            if guard.rejected.contains(&call_id) {
                None
            } else {
                Some(guard.call_to_slot.contains_key(&call_id))
            }
        };

        let known = match known_opt {
            None => continue,
            Some(k) => k,
        };

        let pcm = bytes_to_f32_le(&packet.payload);
        if pcm.is_empty() {
            continue;
        }

        static AUDIO_PKT_COUNT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let pkt_num = AUDIO_PKT_COUNT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if pkt_num < 20 || pkt_num % 500 == 0 {
            tracing::info!(call_id, known, pcm_len = pcm.len(), pkt_num, payload_bytes = packet.payload.len(), "upstream_data: audio packet received");
        }

        if !known {
            match pool.take_slot() {
                Some((batch_idx, in_tx, out_rx)) => {
                    tracing::info!(call_id, batch_idx, pcm_len = pcm.len(), "new call, assigned batch slot");

                    if in_tx.send(InMsg::Init).is_err() {
                        tracing::error!(call_id, batch_idx, "failed to send Init to channel");
                        continue;
                    }

                    {
                        let mut guard = inner.lock().unwrap();
                        guard.call_to_slot.insert(call_id, batch_idx);
                        guard.slot_to_call[batch_idx] = Some(call_id);
                        guard.slot_info[batch_idx] = Some(SlotInfo {
                            in_tx: in_tx.clone(),
                            last_activity: Instant::now(),
                        });
                    }

                    let _ = output_event_tx.send(OutputEvent::NewSlot {
                        batch_idx,
                        call_id,
                        out_rx,
                    });

                    if in_tx.send(InMsg::Audio { pcm }).is_err() {
                        tracing::warn!(call_id, "failed to send first audio to channel");
                    }
                }
                None => {
                    tracing::warn!(call_id, "batch full, rejecting call");
                    let mut guard = inner.lock().unwrap();
                    guard.rejected.insert(call_id);
                }
            }
        } else {
            let mut guard = inner.lock().unwrap();
            if let Some(batch_idx) = guard.call_to_slot.get(&call_id).copied() {
                if let Some(ref mut info) = guard.slot_info[batch_idx] {
                    let send_result = info.in_tx.send(InMsg::Audio { pcm });
                    info.last_activity = Instant::now();
                    if send_result.is_err() {
                        tracing::warn!(call_id, batch_idx, "channel disconnected, audio dropped");
                    }
                }
            }
        }
    }

    tracing::info!("upstream data receive loop stopped");
}

fn bytes_to_f32_le(bytes: &[u8]) -> Vec<f32> {
    if !bytes.len().is_multiple_of(4) {
        tracing::warn!(
            "PCM payload size {} is not a multiple of 4, truncating",
            bytes.len()
        );
    }
    bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect()
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

static SIGNAL_RUNNING: std::sync::OnceLock<Arc<AtomicBool>> = std::sync::OnceLock::new();

extern "C" fn signal_handler(_sig: libc::c_int) {
    if let Some(flag) = SIGNAL_RUNNING.get() {
        flag.store(false, Ordering::SeqCst);
    }
}

pub fn install_signal_handler(running: Arc<AtomicBool>) {
    SIGNAL_RUNNING.get_or_init(|| running);
    #[cfg(unix)]
    unsafe {
        libc::signal(libc::SIGINT, signal_handler as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, signal_handler as *const () as libc::sighandler_t);
    }
}
