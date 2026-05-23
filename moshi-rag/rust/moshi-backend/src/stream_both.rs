// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

use anyhow::{Context, Result};
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct Config {
    pub instance_name: String,
    #[serde(default)]
    pub hf_repo: String,
    pub lm_model_file: String,
    pub log_dir: String,
    pub text_tokenizer_file: String,
    pub mimi_model_file: String,
    pub mimi_num_codebooks: usize,
    pub lm_config: Option<moshi::lm_generate_multistream::Config>,
    pub batch_size: usize,
    #[serde(default = "default_false")]
    pub use_cpu_for_mimi: bool,
    pub asr_delay_in_tokens: Option<usize>,
    // Optional
    #[serde(default)]
    pub power_threshold: Option<f64>,
    #[serde(default)]
    pub init_active_speaker: Option<String>,
    // STT model files (in-process streaming transcription)
    #[serde(default)]
    pub stt_lm_model_file: Option<String>,
    #[serde(default)]
    pub stt_text_tokenizer_file: Option<String>,
    #[serde(default)]
    pub stt_mimi_model_file: Option<String>,
    // Retrieval context preparation
    #[serde(default)]
    pub stt_wait_time: Option<f32>,
    #[serde(default)]
    pub vad_window_size: Option<usize>,
    #[serde(default)]
    pub vad_threshold: Option<f32>,
    // Retrieval and post-processing
    #[serde(default)]
    pub rag_token_id: Option<u32>,
    #[serde(default = "default_zero_f32")]
    pub rag_timeout: f32,
    #[serde(default)]
    pub arc_encoder_tokenizer_path: Option<String>,
    #[serde(default)]
    pub arc_encoder_model_file: Option<String>,
    #[serde(default)]
    pub backend_url: Option<String>,
    #[serde(default)]
    pub arc_mode_enabled: bool,
    #[serde(default = "default_zero_usize")]
    pub moshi_gpu_id: usize,
    #[serde(default = "default_zero_usize")]
    pub stt_gpu_id: usize,
    /// Optional list of retrieval (reference) LLM profiles from `MOSHI_RETRIEVAL_LLMS_JSON` only.
    #[serde(default, skip_deserializing)]
    pub rag_llm_profiles: Option<Vec<crate::rag_retrieval::RagLlmProfile>>,
    /// Deprecated for config-driven profiles; ignored from `config.json`.
    #[serde(default, skip_deserializing)]
    pub rag_llm_default_id: Option<String>,
}

fn default_zero_usize() -> usize {
    0
}

fn default_zero_f32() -> f32 {
    0.0
}

fn default_false() -> bool {
    false
}

fn parse_prompt_style_from_json_value(
    v: &serde_json::Value,
) -> Result<crate::rag_retrieval::PromptStyle> {
    let s = v.as_str().ok_or_else(|| anyhow::anyhow!("prompt_style must be a JSON string"))?;
    match s.trim().to_ascii_lowercase().as_str() {
        "original" => Ok(crate::rag_retrieval::PromptStyle::Original),
        "simplified" => Ok(crate::rag_retrieval::PromptStyle::Simplified),
        x => anyhow::bail!("prompt_style must be \"original\" or \"simplified\", got {:?}", x),
    }
}

fn parse_default_flag_from_json_value(v: &serde_json::Value) -> Result<bool> {
    match v {
        serde_json::Value::Null => Ok(false),
        serde_json::Value::Bool(b) => Ok(*b),
        serde_json::Value::String(s) => match s.trim().to_ascii_lowercase().as_str() {
            "true" => Ok(true),
            "false" => Ok(false),
            _ => anyhow::bail!("default must be true/false or \"true\"/\"false\", got {:?}", s),
        },
        _ => anyhow::bail!("default must be true/false or \"true\"/\"false\""),
    }
}

pub(crate) fn parse_retrieval_llms_json(config: &mut Config) -> Result<()> {
    if let Ok(raw) = std::env::var("MOSHI_RETRIEVAL_LLMS_JSON") {
        let trimmed = raw.trim();
        if !trimmed.is_empty() {
            let value: serde_json::Value = serde_json::from_str(trimmed)
                .with_context(|| "invalid JSON in MOSHI_RETRIEVAL_LLMS_JSON")?;
            #[derive(serde::Deserialize)]
            struct RagLlmProfileEnv {
                id: String,
                base_url: String,
                model: String,
                #[serde(default)]
                api_key: Option<String>,
                #[serde(rename = "default", default)]
                raw_is_default: serde_json::Value,
                #[serde(default, alias = "reference_prompt")]
                prompt_style: Option<crate::rag_retrieval::PromptStyle>,
            }

            let (profiles_value, profile_default_prompt) = match value {
                serde_json::Value::Array(_) => (value, crate::rag_retrieval::PromptStyle::Original),
                serde_json::Value::Object(mut map) => {
                    let profile_default_prompt = match map
                        .remove("prompt_style")
                        .or_else(|| map.remove("reference_prompt"))
                    {
                        None => crate::rag_retrieval::PromptStyle::Original,
                        Some(v) => parse_prompt_style_from_json_value(&v)?,
                    };
                    let profiles_val = map.remove("profiles").ok_or_else(|| {
                        anyhow::anyhow!(
                            "MOSHI_RETRIEVAL_LLMS_JSON: object form requires a \"profiles\" array (legacy form is a bare JSON array of profile objects)"
                        )
                    })?;
                    if !map.is_empty() {
                        let keys: Vec<String> = map.keys().cloned().collect();
                        anyhow::bail!(
                            "MOSHI_RETRIEVAL_LLMS_JSON: unknown top-level keys: {}",
                            keys.join(", ")
                        );
                    }
                    (profiles_val, profile_default_prompt)
                }
                _ => anyhow::bail!(
                    "MOSHI_RETRIEVAL_LLMS_JSON: must be an array of profiles, or an object with \"profiles\" (and optional \"prompt_style\")"
                ),
            };
            let raw_profiles: Vec<RagLlmProfileEnv> = serde_json::from_value(profiles_value).with_context(|| {
                "invalid profile objects in MOSHI_RETRIEVAL_LLMS_JSON (expected id, base_url, model, optional api_key, optional default, optional prompt_style)"
            })?;
            let profiles: Vec<crate::rag_retrieval::RagLlmProfile> = raw_profiles
                .into_iter()
                .map(|r| {
                    Ok(crate::rag_retrieval::RagLlmProfile {
                        id: r.id,
                        base_url: r.base_url,
                        model: r.model,
                        api_key: r.api_key,
                        is_default: parse_default_flag_from_json_value(&r.raw_is_default)?,
                        prompt_style: r.prompt_style.unwrap_or(profile_default_prompt),
                    })
                })
                .collect::<Result<Vec<_>>>()?;
            if profiles.len() >= 2 {
                let n_default = profiles.iter().filter(|p| p.is_default).count();
                if n_default != 1 {
                    anyhow::bail!(
                        "MOSHI_RETRIEVAL_LLMS_JSON: exactly one profile must have \"default\": true when using multiple profiles (found {})",
                        n_default
                    );
                }
            }
            let n = profiles.len();
            config.rag_llm_profiles = Some(profiles);
            tracing::info!(
                "MOSHI_RETRIEVAL_LLMS_JSON: loaded {n} rag_llm_profile(s) from environment (overrides config)"
            );
        }
    }
    Ok(())
}

impl Config {
    #[allow(dead_code)]
    pub fn load<P: AsRef<std::path::Path>>(p: P) -> Result<Self> {
        let config = std::fs::read_to_string(p)?;
        let mut config: Self = serde_json::from_str(&config)?;
        config.log_dir = crate::utils::replace_env_vars(&config.log_dir);
        config.text_tokenizer_file = crate::utils::replace_env_vars(&config.text_tokenizer_file);
        config.mimi_model_file = crate::utils::replace_env_vars(&config.mimi_model_file);
        config.lm_model_file = crate::utils::replace_env_vars(&config.lm_model_file);
        if let Some(ref mut s) = config.stt_lm_model_file {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.stt_text_tokenizer_file {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.stt_mimi_model_file {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.arc_encoder_tokenizer_path {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.arc_encoder_model_file {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.backend_url {
            *s = crate::utils::replace_env_vars(s);
        }

        parse_retrieval_llms_json(&mut config)?;

        Ok(config)
    }

    pub fn use_rag(&self) -> bool {
        self.rag_token_id.is_some() && self.backend_url.is_some()
    }

    pub fn use_stt(&self) -> bool {
        self.stt_lm_model_file.is_some()
            && self.stt_text_tokenizer_file.is_some()
            && self.stt_mimi_model_file.is_some()
    }

    pub fn use_arc(&self) -> bool {
        self.arc_mode_enabled && self.arc_encoder_tokenizer_path.is_some()
    }

    pub fn requires_model_download(&self) -> bool {
        let mut paths: Vec<&str> = vec![
            self.lm_model_file.as_str(),
            self.mimi_model_file.as_str(),
            self.text_tokenizer_file.as_str(),
        ];
        if self.use_stt() {
            paths.extend([
                self.stt_lm_model_file.as_deref().unwrap(),
                self.stt_mimi_model_file.as_deref().unwrap(),
                self.stt_text_tokenizer_file.as_deref().unwrap(),
            ]);
        }
        if self.use_arc() {
            if let Some(p) = self.arc_encoder_tokenizer_path.as_deref() {
                paths.push(p);
            }
            if let Some(p) = self.arc_encoder_model_file.as_deref() {
                paths.push(p);
            }
        }
        paths.iter().any(|p| crate::hf_path::path_needs_resolution(p))
    }
}

fn rms_db(frame: &[f32]) -> f32 {
    let n = frame.len();
    if n == 0 {
        return f32::NEG_INFINITY;
    }
    let sum_sq: f32 = frame.iter().map(|x| x * x).sum();
    let rms_sq = sum_sq / n as f32;
    10.0 * (rms_sq + 1e-16_f32).log10()
}

fn apply_power_threshold_frame(frame: &mut [f32], thresh_db: f32) {
    if rms_db(frame) < thresh_db {
        frame.fill(0.0);
    }
}

pub struct AppStateInner {
    pub lm_model: moshi::lm::LmModel,
    pub mimi_model: moshi::mimi::Mimi,
    pub text_tokenizer: sentencepiece::SentencePieceProcessor,
    pub device: candle::Device,
    pub stt_asr: Mutex<Option<moshi::asr::State>>,
    pub stt_tokenizer: Mutex<Option<sentencepiece::SentencePieceProcessor>>,
    pub config: Config,
}

fn decode_text_piece(
    tokenizer: &sentencepiece::SentencePieceProcessor,
    prev_text_token: u32,
    text_token: u32,
    config: &moshi::lm_generate_multistream::Config,
) -> Option<String> {
    if text_token != config.text_start_token
        && text_token != config.text_pad_token
        && text_token != config.text_eop_token
    {
        if prev_text_token == config.text_start_token {
            tokenizer.decode_piece_ids(&[text_token]).ok()
        } else {
            let prev_ids = tokenizer.decode_piece_ids(&[prev_text_token]).ok();
            let ids = tokenizer.decode_piece_ids(&[prev_text_token, text_token]).ok();
            prev_ids.and_then(|prev_ids| {
                ids.map(|ids| {
                    if ids.len() > prev_ids.len() {
                        ids[prev_ids.len()..].to_string()
                    } else {
                        String::new()
                    }
                })
            })
        }
    } else {
        None
    }
}

/// Index of the VAD head in the STT model's probability output (AsrMsg::Step::prs).
const BATCH_VAD_HEAD_INDEX: usize = 2;

/// Batched state: one shared model loop & channel pool. Used when batch_size > 1.
pub struct BatchedState {
    /// Accessed in Step 9 (teacher-forcing tokenization uses inner.text_tokenizer).
    #[allow(dead_code)]
    pub inner: Arc<AppStateInner>,
    pub pool: Arc<crate::batched_channels::BatchedStreamingChannels>,
    /// Keeps the Arc alive so profile data is retained; also used by rag_manager via rag_for_loop.
    #[allow(dead_code)]
    pub rag_retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
    _loop_handle: Option<std::thread::JoinHandle<()>>,
    pub model_loop_ready: Arc<std::sync::atomic::AtomicBool>,
    _ws_client: Option<crate::backend_ws::BackendWsClient>,
}

impl BatchedState {
    /// Create the pool and spawn the single model loop. Call when batch_size > 1.
    pub fn new(inner: Arc<AppStateInner>) -> Result<Self> {
        let batch_size = inner.lm_model.batch_size();
        if batch_size <= 1 {
            anyhow::bail!("batch_size > 1 required");
        }
        let mimi_config = inner.mimi_model.config();
        let frame_size = (mimi_config.sample_rate / mimi_config.frame_rate).ceil() as usize;
        let pool = crate::batched_channels::BatchedStreamingChannels::new(batch_size, frame_size);
        let pool = Arc::new(pool);
        let rag_retrieval = crate::rag_retrieval::RagRetrievalEndpoints::from_profiles(
            inner.config.rag_llm_profiles.clone(),
            inner.config.rag_llm_default_id.as_deref(),
        );

        let (ws_client, ws_sender, ws_rx) = if let Some(ref backend_url) = inner.config.backend_url
        {
            let rt = tokio::runtime::Handle::current();
            let (client, in_rx) = crate::backend_ws::BackendWsClient::new(
                backend_url.clone(),
                rt,
            );
            let sender = client.sender();
            (Some(client), Some(sender), Some(in_rx))
        } else {
            (None, None, None)
        };

        let inner_clone = inner.clone();
        let pool_clone = pool.clone();
        let rag_for_loop = rag_retrieval.clone();
        let model_loop_ready = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let ready_flag = model_loop_ready.clone();
        let loop_handle = std::thread::spawn(move || {
            if let Err(e) = Self::run_loop(
                inner_clone,
                pool_clone,
                rag_for_loop,
                ready_flag,
                ws_sender,
                ws_rx,
            ) {
                if !e.to_string().contains("max step_idx reached") {
                    tracing::error!(err = %e, "batched model loop error");
                }
            }
        });
        Ok(Self {
            inner,
            pool,
            rag_retrieval,
            _loop_handle: Some(loop_handle),
            model_loop_ready,
            _ws_client: ws_client,
        })
    }

    fn run_loop(
        inner: Arc<AppStateInner>,
        pool: Arc<crate::batched_channels::BatchedStreamingChannels>,
        rag_retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
        model_loop_ready: Arc<std::sync::atomic::AtomicBool>,
        ws_sender: Option<tokio::sync::mpsc::UnboundedSender<crate::backend_ws::OutboundWsMsg>>,
        ws_rx: Option<std::sync::mpsc::Receiver<crate::backend_ws::InboundWsMsg>>,
    ) -> Result<()> {
        let session_config = default_session_config_batched();
        let gen_config = inner
            .config
            .lm_config
            .clone()
            .unwrap_or_else(moshi::lm_generate_multistream::Config::v0_1);
        let batch_size = inner.lm_model.batch_size();
        let audio_lp: Vec<_> = (0..batch_size)
            .map(|_| {
                candle_transformers::generation::LogitsProcessor::from_sampling(
                    session_config.audio_seed,
                    candle_transformers::generation::Sampling::TopK {
                        k: session_config.audio_topk,
                        temperature: session_config.audio_temperature,
                    },
                )
            })
            .collect();
        let text_lp = candle_transformers::generation::LogitsProcessor::from_sampling(
            session_config.text_seed,
            candle_transformers::generation::Sampling::TopK {
                k: session_config.text_topk,
                temperature: session_config.text_temperature,
            },
        );
        let mut state = moshi::batched_lm_generate_multistream::State::new(
            inner.lm_model.clone(),
            inner.mimi_model.clone(),
            session_config.max_steps,
            audio_lp,
            text_lp,
            session_config.pad_mult,
            session_config.repetition_penalty,
            gen_config.clone(),
        )?;
        state.warmup(pool.frame_size)?;
        tracing::info!("main model warmup done, warming up reset_batch_idx...");
        for bid in 0..inner.lm_model.batch_size() {
            state.reset_batch_idx(bid)?;
        }
        {
            let dev = inner.device.clone();
            dev.synchronize()?;
        }
        tracing::info!("reset_batch_idx warmup done, running post-reset step warmup...");
        {
            let dev = inner.device.clone();
            let pcm = candle::Tensor::zeros((inner.lm_model.batch_size(), 1, pool.frame_size), candle::DType::F32, &dev)?;
            let mask = moshi::StreamMask::new(vec![true; inner.lm_model.batch_size()], &dev)?;
            let _ = state.step_pcm(&pcm, &mask)?;
            dev.synchronize()?;
        }
        tracing::info!("post-reset step warmup done");
        let batch_size = inner.lm_model.batch_size();
        let mut stt_state: Option<(moshi::asr::State, sentencepiece::SentencePieceProcessor)> = {
            let taken_asr = inner.stt_asr.lock().unwrap().take();
            let taken_tok = inner.stt_tokenizer.lock().unwrap().take();
            match (taken_asr, taken_tok) {
                (Some(asr), Some(tok)) => {
                    tracing::info!("batched STT state taken from AppStateInner");
                    Some((asr, tok))
                }
                _ => {
                    if inner.config.use_stt() {
                        tracing::warn!("STT enabled in config but models not available in AppStateInner");
                    }
                    None
                }
            }
        };
        let device = inner.device.clone();
        let rag_manager = match (inner.config.rag_token_id, inner.config.backend_url.as_ref()) {
            (Some(rag_token_id), Some(backend_url)) => Some((
                crate::rag_manager::RagManager::new(
                    backend_url.clone(),
                    rag_retrieval.clone(),
                ),
                rag_token_id,
            )),
            _ => None,
        };
        let arc_mode_enabled = inner.config.arc_mode_enabled;
        let stt_wait_secs = inner.config.stt_wait_time.map(|t| t as f64).unwrap_or(0.0);
        let rag_timeout = inner.config.rag_timeout;
        let vad_window = inner.config.vad_window_size.unwrap_or(4);
        let vad_threshold = inner.config.vad_threshold.unwrap_or(0.5);
        let frame_rate = inner.mimi_model.config().frame_rate;
        let vad_wait_steps = (stt_wait_secs * frame_rate).ceil() as usize;
        let (init_speaker, first_speaker_str): (crate::turn_manager::TextRole, Option<&str>) =
            match inner.config.init_active_speaker.as_deref() {
                Some("user") => (crate::turn_manager::TextRole::User, Some("user")),
                Some("model") => (crate::turn_manager::TextRole::Model, Some("model")),
                _ => (crate::turn_manager::TextRole::Model, None),
            };
        let turn_managers: Option<Vec<Arc<Mutex<crate::turn_manager::TurnManager>>>> =
            if stt_state.is_some() {
                Some(
                    (0..batch_size)
                        .map(|_| {
                            Arc::new(Mutex::new(crate::turn_manager::TurnManager::new(
                                vad_window,
                                vad_threshold,
                                vad_wait_steps,
                                init_speaker,
                            )))
                        })
                        .collect(),
                )
            } else {
                None
            };

        if let Some((ref mut stt_asr, _)) = stt_state {
            tracing::info!("warming up STT model...");
            let stt_dev = stt_asr.device().clone();
            let stt_pcm = candle::Tensor::zeros((batch_size, 1, pool.frame_size), candle::DType::F32, &stt_dev)?;
            let stt_mask = moshi::StreamMask::new(vec![true; batch_size], &stt_dev)?;
            let _ = stt_asr.step_pcm(stt_pcm, None, &stt_mask, |_, _, _| ())?;
            stt_dev.synchronize()?;
            tracing::info!("STT warmup done");
        }
        model_loop_ready.store(true, std::sync::atomic::Ordering::SeqCst);
        tracing::info!("batched model loop started (all warmup complete)");
        let mut prev_channel_ids: Vec<Option<crate::batched_channels::ChannelId>> =
            vec![None; batch_size];
        let mut loop_counter: u64 = 0;
        loop {
            loop_counter += 1;
            let (mut batch_pcm, mask, ref_channel_ids, reset_slots) = pool.pre_process(&mut state);

            for (b, (prev, curr)) in prev_channel_ids.iter().zip(ref_channel_ids.iter()).enumerate()
            {
                if prev.is_some() && curr.is_none() {
                    if let Some(ref ws) = ws_sender {
                        let _ = ws.send(crate::backend_ws::OutboundWsMsg::CallEnd { slot_id: b });
                    }
                }
            }
            let had_prev: Vec<bool> = prev_channel_ids.iter().map(|c| c.is_some()).collect();
            prev_channel_ids.clone_from(&ref_channel_ids);

            for &bid in &reset_slots {
                if let Err(e) =
                    state.set_prepend_condition_lut(bid, "first_speaker", first_speaker_str)
                {
                    tracing::debug!(?e, bid, "set_prepend_condition_lut (no condition provider)");
                }
                if let Some((ref mut stt_asr, _)) = stt_state {
                    if let Err(e) = stt_asr.reset_batch_idx(bid) {
                        tracing::debug!(?e, bid, "stt reset_batch_idx failed");
                    }
                }
                if let Some(ref turn_managers) = turn_managers {
                    if let Some(tm) = turn_managers.get(bid) {
                        tm.lock().unwrap().reset(init_speaker);
                    }
                }
                if let Some((ref rag_mgr, _)) = rag_manager {
                    rag_mgr.cancel_pending_slot_nonblocking(bid);
                }
                if let Some(ref ws) = ws_sender {
                    if had_prev.get(bid).copied().unwrap_or(false) {
                        let _ = ws.send(crate::backend_ws::OutboundWsMsg::CallEnd { slot_id: bid });
                    }
                    let _ = ws.send(crate::backend_ws::OutboundWsMsg::CallStart {
                        slot_id: bid,
                        caller_phone: None,
                        call_id: None,
                    });
                }
            }

            if let Some((ref rag_mgr, _)) = rag_manager {
                while let Some((slot_id, ref_text)) = rag_mgr.try_recv_result_slot() {
                    let display_text = if let Some(idx) = ref_text.find('\t') {
                        ref_text[idx + 1..].to_string()
                    } else {
                        ref_text.clone()
                    };
                    if !ref_text.trim().is_empty() {
                        if arc_mode_enabled {
                            if let Err(e) =
                                state.set_streaming_sum_condition(slot_id, "reference_with_time", &ref_text)
                            {
                                tracing::warn!(?e, slot_id, "set_streaming_sum_condition");
                            }
                        } else {
                            match inner.text_tokenizer.encode(&display_text) {
                                Ok(pieces) => {
                                    let token_ids: Vec<u32> = pieces.into_iter().map(|p| p.id).collect();
                                    tracing::info!(slot_id, n_tokens = token_ids.len(), "rag: teacher-forcing prefill inject");
                                    match state.prefill_inject_text(slot_id, token_ids) {
                                        Ok(()) => {}
                                        Err(e) => {
                                            tracing::warn!(slot_id, ?e, "ret: prefill_inject_text failed — injection skipped");
                                        }
                                    }
                                    if let Some(ref turn_managers) = turn_managers {
                                        if let Some(tm) = turn_managers.get(slot_id) {
                                            let ctx_block = format!(
                                                "[Injected reference]\n{}\n[End of injected reference]\n\n",
                                                display_text
                                            );
                                            tm.lock().unwrap().append_context(&ctx_block);
                                        }
                                    }
                                }
                                Err(e) => {
                                    tracing::warn!(slot_id, ?e, "rag: failed to tokenize ref_text for teacher-forcing");
                                }
                            }
                        }
                    }
                    let out = if ref_text.trim().is_empty() {
                        StreamOut::TextByRole {
                            text: "[RET_FAILED]".to_string(),
                            role: TextRole::Model,
                        }
                    } else {
                        StreamOut::ReferenceText { text: display_text }
                    };
                    if pool.send_to_slot(slot_id, out).is_err() {
                        tracing::debug!(slot_id, "failed to send RAG result to slot");
                    }
                }
            }

            if let Some(ref rx) = ws_rx {
                while let Ok(inbound) = rx.try_recv() {
                    match inbound {
                        crate::backend_ws::InboundWsMsg::ReferenceInject { slot_id, text } => {
                            tracing::info!(slot_id, text_len = text.len(), "ws: reference_inject received");
                            if !arc_mode_enabled {
                                if let Some(ref turn_managers) = turn_managers {
                                    if let Some(tm) = turn_managers.get(slot_id) {
                                        let ctx_block = format!(
                                            "[Injected reference]\n{}\n[End of injected reference]\n\n",
                                            text
                                        );
                                        tm.lock().unwrap().append_context(&ctx_block);
                                    }
                                }
                                match inner.text_tokenizer.encode(&text) {
                                    Ok(pieces) => {
                                        let token_ids: Vec<u32> = pieces.into_iter().map(|p| p.id).collect();
                                        tracing::info!(slot_id, n_tokens = token_ids.len(), "ws: teacher-forcing prefill inject");
                                        match state.prefill_inject_text(slot_id, token_ids) {
                                            Ok(()) => {}
                                            Err(e) => {
                                                tracing::warn!(slot_id, ?e, "ws: prefill_inject_text failed — injection skipped");
                                            }
                                        }
                                    }
                                    Err(e) => {
                                        tracing::warn!(slot_id, ?e, "ws: failed to tokenize reference_inject text");
                                    }
                                }
                                let out = StreamOut::ReferenceText { text };
                                if pool.send_to_slot(slot_id, out).is_err() {
                                    tracing::debug!(slot_id, "failed to send ws reference_inject to slot");
                                }
                            } else {
                                tracing::warn!(slot_id, "ws: reference_inject received but arc_mode_enabled=true — backend should send arc_inject; ignoring");
                            }
                        }
                        crate::backend_ws::InboundWsMsg::ArcInject { slot_id, reference_text } => {
                            if arc_mode_enabled {
                                let prefixed = format!("backend\t{}", reference_text);
                                if let Err(e) = state.set_streaming_sum_condition(
                                    slot_id,
                                    "reference_with_time",
                                    &prefixed,
                                ) {
                                    tracing::warn!(?e, slot_id, "ws arc_inject set_streaming_sum_condition");
                                }
                            } else {
                                tracing::warn!(slot_id, "ws: arc_inject received but arc_mode_enabled=false, ignoring");
                            }
                        }
                        crate::backend_ws::InboundWsMsg::TriggerFired { slot_id, label, action_type } => {
                            tracing::info!(slot_id, %label, %action_type, "ws: trigger_fired");
                        }
                    }
                }
            }

            let any_active = mask.iter().any(|&b| b);
            if loop_counter % 500 == 0 || (loop_counter < 50 && loop_counter % 10 == 0) {
                let active_slots: Vec<usize> = mask.iter().enumerate().filter(|(_, &b)| b).map(|(i, _)| i).collect();
                let has_channels: Vec<usize> = ref_channel_ids.iter().enumerate().filter(|(_, c)| c.is_some()).map(|(i, _)| i).collect();
                tracing::info!(loop_counter, any_active, ?active_slots, ?has_channels, "model loop pre_process result");
            }
            let mut msgs_for_pools: Vec<moshi::batched_lm_generate_multistream::StreamingOutMsg> =
                Vec::new();
            if any_active {
                let batch_stt_pcm = batch_pcm.clone();
                if let Some(thresh_db64) = inner.config.power_threshold {
                    let thresh_db = thresh_db64 as f32;
                    let frame_size = pool.frame_size;
                    for (b, &active) in mask.iter().enumerate().take(batch_size) {
                        if !active {
                            continue;
                        }
                        let start = b * frame_size;
                        let end = start + frame_size;
                        if end > batch_pcm.len() {
                            continue;
                        }
                        apply_power_threshold_frame(&mut batch_pcm[start..end], thresh_db);
                    }
                }

                let stt_config = moshi::lm_generate_multistream::Config::v0_1_stt();
                if let Some((ref mut stt_asr, _)) = stt_state {
                    let stt_dev = stt_asr.device().clone();
                    let batch_stt_pcm = candle::Tensor::new(batch_stt_pcm.as_slice(), &stt_dev)?
                        .reshape((batch_size, 1, pool.frame_size))?;
                    let stt_mask = moshi::StreamMask::new(mask.clone(), &stt_dev)?;
                    let stt_msgs =
                        stt_asr.step_pcm(batch_stt_pcm, None, &stt_mask, |_, _, _| ())?;

                    let stt_word_count = stt_msgs.iter().filter(|m| matches!(m, moshi::asr::AsrMsg::Word { .. })).count();
                    let stt_step_count = stt_msgs.iter().filter(|m| matches!(m, moshi::asr::AsrMsg::Step { .. })).count();
                    let stt_end_count = stt_msgs.iter().filter(|m| matches!(m, moshi::asr::AsrMsg::EndWord { .. })).count();
                    if stt_word_count > 0 || stt_end_count > 0 || loop_counter % 5 == 0 {
                        tracing::info!(loop_counter, stt_word_count, stt_step_count, stt_end_count, stt_delay=stt_asr.asr_delay_in_tokens(), "STT step_pcm result");
                    }
                    for msg in &stt_msgs {
                        if let moshi::asr::AsrMsg::Word { tokens, batch_idx, .. } = msg {
                            tracing::info!(loop_counter, batch_idx, ?tokens, "STT Word event");
                        }
                    }

                    for msg in &stt_msgs {
                        if let moshi::asr::AsrMsg::Step { prs, .. } = msg {
                            if let Some(ref turn_managers) = turn_managers {
                                for b in 0..batch_size {
                                    let vad_value = prs
                                        .get(BATCH_VAD_HEAD_INDEX)
                                        .and_then(|v| v.get(b).copied())
                                        .unwrap_or(0.0);
                                    if let Some(tm) = turn_managers.get(b) {
                                        tm.lock().unwrap().update_vad(1.0 - vad_value);
                                    }
                                }
                            }
                        }
                    }

                    for msg in &stt_msgs {
                        if let moshi::asr::AsrMsg::Word { tokens, batch_idx, .. } = msg {
                            let mut prev = state
                                .last_stt_text_token(*batch_idx)
                                .unwrap_or(stt_config.text_start_token);
                            for &token in tokens {
                                msgs_for_pools.push(moshi::batched_lm_generate_multistream::StreamingOutMsg::TextToken {
                                    batch_idx: *batch_idx,
                                    prev_token: prev,
                                    token,
                                    role: moshi::batched_lm_generate_multistream::TextRole::User,
                                });
                                prev = token;
                            }
                            state.set_last_stt_text_token(*batch_idx, prev);
                        }
                    }
                }

                let pcm_tensor = candle::Tensor::from_vec(
                    batch_pcm,
                    (pool.batch_size, 1, pool.frame_size),
                    &device,
                )?;
                let stream_mask = moshi::StreamMask::new(mask, &device)?;
                let t_step_start = std::time::Instant::now();
                let msgs = state.step_pcm(&pcm_tensor, &stream_mask)?;
                let step_ms = t_step_start.elapsed().as_millis();
                if step_ms > 200 || loop_counter < 20 || loop_counter % 100 == 0 {
                    tracing::info!(step_ms, loop_counter, batch_size, "step_pcm timing");
                }

                {
                    let rag_token_id = rag_manager.as_ref().map(|(_, id)| *id);
                    for msg in &msgs {
                        if let moshi::batched_lm_generate_multistream::StreamingOutMsg::TextToken {
                            batch_idx,
                            prev_token: _,
                            token,
                            ..
                        } = msg
                        {
                            if let Some(rag_tid) = rag_token_id {
                                if *token == rag_tid {
                                    let bid = *batch_idx;
                                    if let Some(ref ws) = ws_sender {
                                        let _ = ws.send(crate::backend_ws::OutboundWsMsg::RetToken {
                                            slot_id: bid,
                                        });
                                    }
                                    if let Some((ref rag_mgr, _)) = rag_manager {
                                        if let Some(ref turn_managers) = turn_managers {
                                            if let Some(tm_arc) = turn_managers.get(bid) {
                                                let tm_ptr = Arc::clone(tm_arc);
                                                rag_mgr.trigger_background_generation_slot(
                                                    bid,
                                                    stt_wait_secs,
                                                    rag_timeout,
                                                    move || -> String {
                                                        tm_ptr.lock().unwrap().get_context().to_string()
                                                    },
                                                );
                                            }
                                        }
                                    }
                                    let _ = pool.send_to_slot(
                                        bid,
                                        StreamOut::TextByRole {
                                            text: "[RET]".to_string(),
                                            role: crate::turn_manager::TextRole::Model,
                                        },
                                    );
                                }
                            }
                        }
                    }
                }

                let text_tokens_from_step: Vec<(usize, u32, u32)> = msgs.iter().filter_map(|m| {
                    if let moshi::batched_lm_generate_multistream::StreamingOutMsg::TextToken { batch_idx, prev_token, token, .. } = m {
                        Some((*batch_idx, *prev_token, *token))
                    } else { None }
                }).collect();
                let pcm_from_step = msgs.iter().filter(|m| matches!(m, moshi::batched_lm_generate_multistream::StreamingOutMsg::Pcm { .. })).count();
                if !text_tokens_from_step.is_empty() || pcm_from_step > 0 || loop_counter % 100 == 0 {
                    tracing::info!(?text_tokens_from_step, pcm_from_step, loop_counter, "step output");
                }
                msgs_for_pools.extend(msgs);
                let text_token_total = msgs_for_pools.iter().filter(|m| matches!(m, moshi::batched_lm_generate_multistream::StreamingOutMsg::TextToken { .. })).count();
                let pcm_total = msgs_for_pools.iter().filter(|m| matches!(m, moshi::batched_lm_generate_multistream::StreamingOutMsg::Pcm { .. })).count();
                if loop_counter % 100 == 0 {
                    tracing::info!(text_token_total, pcm_total, loop_counter, "model loop iteration stats");
                }
                for b in 0..batch_size {
                    let msgs_this_batch: Vec<
                        &moshi::batched_lm_generate_multistream::StreamingOutMsg,
                    > = msgs_for_pools
                        .iter()
                        .filter(|msg| {
                            matches!(
                                msg,
                                moshi::batched_lm_generate_multistream::StreamingOutMsg::TextToken {
                                    batch_idx,
                                    ..
                                }
                                | moshi::batched_lm_generate_multistream::StreamingOutMsg::Pcm {
                                    batch_idx,
                                    ..
                                } if *batch_idx == b
                            )
                        })
                        .collect();
                    let mut model_text = String::new();
                    let mut user_text = String::new();
                    for msg in &msgs_this_batch {
                        match msg {
                            moshi::batched_lm_generate_multistream::StreamingOutMsg::TextToken { role, prev_token, token, .. } => {
                                let (tokenizer, config) = match role {
                                    moshi::batched_lm_generate_multistream::TextRole::Model => {
                                        (&inner.text_tokenizer, &gen_config)
                                    }
                                    moshi::batched_lm_generate_multistream::TextRole::User => {
                                        match stt_state {
                                            Some((_, ref stt_tokenizer)) => (stt_tokenizer, &stt_config),
                                            None => continue,
                                        }
                                    }
                                };
                                if let Some(text) =
                                    decode_text_piece(tokenizer, *prev_token, *token, config)
                                {
                                    match role {
                                        moshi::batched_lm_generate_multistream::TextRole::Model => model_text.push_str(&text),
                                        moshi::batched_lm_generate_multistream::TextRole::User => user_text.push_str(&text),
                                    }
                                }
                            },
                            moshi::batched_lm_generate_multistream::StreamingOutMsg::Pcm { batch_idx, pcm } => {
                                pool.post_process(
                                    StreamOut::Pcm { pcm: pcm.clone() },
                                    *batch_idx,
                                    &ref_channel_ids,
                                )?;
                            }
                        }
                    }
                    if let Some(ref ws) = ws_sender {
                        if !model_text.is_empty() {
                            let _ = ws.send(crate::backend_ws::OutboundWsMsg::Token {
                                slot_id: b,
                                role: "model".to_string(),
                                text: model_text.clone(),
                            });
                        }
                        if !user_text.is_empty() {
                            let _ = ws.send(crate::backend_ws::OutboundWsMsg::Token {
                                slot_id: b,
                                role: "user".to_string(),
                                text: user_text.clone(),
                            });
                        }
                    }
                    if !user_text.is_empty() {
                        pool.post_process(
                            StreamOut::TextByRole { text: user_text.clone(), role: crate::turn_manager::TextRole::User },
                            b,
                            &ref_channel_ids,
                        )?;
                    }
                    if let Some(ref turn_managers) = turn_managers {
                        let outputs: Vec<(String, crate::turn_manager::TextRole)> =
                            turn_managers.get(b).unwrap().lock().unwrap().handle_spoken_text(
                                Some(model_text.as_str()),
                                Some(user_text.as_str()),
                            );
                        for (text, role) in &outputs {
                            if *role == crate::turn_manager::TextRole::Model {
                                pool.post_process(
                                    StreamOut::TextByRole { text: text.clone(), role: *role },
                                    b,
                                    &ref_channel_ids,
                                )?;
                            }
                        }
                    } else {
                        if !model_text.is_empty() {
                            tracing::info!(batch_idx=b, text=%model_text, "emitting TextByRole for model (no TM)");
                            pool.post_process(
                                StreamOut::TextByRole { text: model_text.clone(), role: crate::turn_manager::TextRole::Model },
                                b,
                                &ref_channel_ids,
                            )?;
                        }
                        if !user_text.is_empty() {
                            tracing::info!(batch_idx=b, text=%user_text, "emitting TextByRole for user (no TM)");
                            pool.post_process(
                                StreamOut::TextByRole { text: user_text.clone(), role: crate::turn_manager::TextRole::User },
                                b,
                                &ref_channel_ids,
                            )?;
                        }
                    }
                }
            } else {
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
        }
    }
}

#[derive(serde::Deserialize, Debug, Clone)]
pub struct SessionConfigReq {
    pub text_temperature: Option<f64>,
    pub text_topk: Option<usize>,
    pub audio_temperature: Option<f64>,
    pub audio_topk: Option<usize>,
    pub max_steps: Option<usize>,
    pub audio_seed: Option<u64>,
    pub text_seed: Option<u64>,
    pub email: Option<String>,
    pub pad_mult: Option<f32>,
    pub repetition_penalty_context: Option<usize>,
    pub repetition_penalty: Option<f32>,
}

#[derive(serde::Serialize, Debug, Clone)]
pub struct SessionConfig {
    pub text_temperature: f64,
    pub text_topk: usize,
    pub audio_temperature: f64,
    pub audio_topk: usize,
    pub max_steps: usize,
    pub audio_seed: u64,
    pub text_seed: u64,
    pub pad_mult: Option<f32>,
    pub repetition_penalty: Option<(usize, f32)>,
    pub email: Option<String>,
    pub user_feedback: Option<usize>,
}

#[allow(dead_code)]
#[derive(serde::Serialize, Debug, Clone)]
struct SessionSummary<'a> {
    #[serde(flatten)]
    session_config: &'a SessionConfig,
    last_step_idx: usize,
    transcript: String,
    addr: Option<String>,
    lm_model_file: &'a str,
    mimi_model_file: &'a str,
    #[serde(flatten)]
    lm_config: &'a Option<moshi::lm_generate_multistream::Config>,
}

impl SessionConfigReq {
    fn into_session_config(self) -> SessionConfig {
        use rand::Rng;

        let repetition_penalty = self.repetition_penalty_context.zip(self.repetition_penalty);
        SessionConfig {
            text_temperature: self.text_temperature.unwrap_or(0.8),
            text_topk: self.text_topk.unwrap_or(250),
            text_seed: self.text_seed.unwrap_or_else(|| rand::rng().random()),
            audio_temperature: self.audio_temperature.unwrap_or(0.8),
            audio_topk: self.audio_topk.unwrap_or(250),
            audio_seed: self.audio_seed.unwrap_or_else(|| rand::rng().random()),
            email: self.email,
            user_feedback: None,
            max_steps: self.max_steps.unwrap_or(4500).min(4500),
            pad_mult: self.pad_mult,
            repetition_penalty,
        }
    }
}

/// Default session config for the batched model loop (no per-connection params).
fn default_session_config_batched() -> SessionConfig {
    SessionConfigReq {
        text_temperature: None,
        text_topk: None,
        audio_temperature: None,
        audio_topk: None,
        max_steps: None,
        audio_seed: None,
        text_seed: None,
        email: None,
        pad_mult: None,
        repetition_penalty_context: None,
        repetition_penalty: None,
    }
    .into_session_config()
}

/// Serializable metadata for retrieval backend entries (legacy HTTP-WS metadata handshake).
#[allow(dead_code)]
#[derive(serde::Serialize, serde::Deserialize, Debug, Clone)]
pub struct RetrievalBackendMeta {
    pub id: String,
}

/// Serializable session metadata (legacy HTTP-WS handshake; retained for protocol documentation).
#[allow(dead_code)]
#[derive(serde::Serialize, serde::Deserialize, Debug, Clone)]
pub struct MetaData {
    text_temperature: f64,
    text_topk: usize,
    audio_temperature: f64,
    audio_topk: usize,
    pad_mult: f32,
    repetition_penalty_context: usize,
    repetition_penalty: f32,
    lm_model_file: String,
    mimi_model_file: String,
    build_info: crate::utils::BuildInfo,
    instance_name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub retrieval_backends: Option<Vec<RetrievalBackendMeta>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub retrieval_backend_default: Option<String>,
}

/// Re-export for protocol (ColoredText by role).
pub use crate::turn_manager::TextRole;

#[derive(Debug, Clone)]
pub enum StreamOut {
    Ready,
    /// For display of transcript with speaker (ColoredText: 0x07 + color_id + utf8).
    /// Fields read when text output via interconnect text port is wired in a future step.
    TextByRole {
        #[allow(dead_code)]
        text: String,
        #[allow(dead_code)]
        role: TextRole,
    },
    /// Reference text from RAG (ColoredReferenceText: 0x09 + color_id 4 + utf8).
    /// Field read when reference text output via interconnect text port is wired in a future step.
    ReferenceText {
        #[allow(dead_code)]
        text: String,
    },
    Pcm {
        pcm: Vec<f32>,
    },
}

/// WebSocket message type protocol constants (legacy HTTP-WS server mode; retained as protocol documentation).
#[allow(dead_code)]
#[derive(Debug, Clone, Copy)]
pub enum MsgType {
    Handshake,
    Audio,
    Text,
    Control,
    Metadata,
    Error,
    Ping,
    ColoredText,
    ReferenceText,
    ColoredReferenceText,
}

#[allow(dead_code)]
impl MsgType {
    pub fn from_u8(v: u8) -> Result<Self> {
        let s = match v {
            0 => MsgType::Handshake,
            1 => MsgType::Audio,
            2 => MsgType::Text,
            3 => MsgType::Control,
            4 => MsgType::Metadata,
            5 => MsgType::Error,
            6 => MsgType::Ping,
            7 => MsgType::ColoredText,
            8 => MsgType::ReferenceText,
            9 => MsgType::ColoredReferenceText,
            _ => anyhow::bail!("unexpected msg type {v}"),
        };
        Ok(s)
    }

    pub fn to_u8(self) -> u8 {
        match self {
            MsgType::Handshake => 0,
            MsgType::Audio => 1,
            MsgType::Text => 2,
            MsgType::Control => 3,
            MsgType::Metadata => 4,
            MsgType::Error => 5,
            MsgType::Ping => 6,
            MsgType::ColoredText => 7,
            MsgType::ReferenceText => 8,
            MsgType::ColoredReferenceText => 9,
        }
    }
}
