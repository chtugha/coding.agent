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
    #[serde(default = "default_zero_usize")]
    pub moshi_gpu_id: usize,
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
    pub fn load<P: AsRef<std::path::Path>>(p: P) -> Result<Self> {
        let config = std::fs::read_to_string(p)?;
        let mut config: Self = serde_json::from_str(&config)?;
        config.log_dir = crate::utils::replace_env_vars(&config.log_dir);
        config.text_tokenizer_file = crate::utils::replace_env_vars(&config.text_tokenizer_file);
        config.mimi_model_file = crate::utils::replace_env_vars(&config.mimi_model_file);
        config.lm_model_file = crate::utils::replace_env_vars(&config.lm_model_file);
        if let Some(ref mut s) = config.arc_encoder_tokenizer_path {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.arc_encoder_model_file {
            *s = crate::utils::replace_env_vars(s);
        }

        parse_retrieval_llms_json(&mut config)?;

        Ok(config)
    }

    pub fn use_rag(&self) -> bool {
        self.arc_encoder_tokenizer_path.is_some()
    }

    pub fn requires_model_download(&self) -> bool {
        let mut paths: Vec<&str> = vec![
            self.lm_model_file.as_str(),
            self.mimi_model_file.as_str(),
            self.text_tokenizer_file.as_str(),
        ];
        if self.use_rag() {
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

pub type AppState = Arc<AppStateInner>;

/// Unified state for standalone: standard (single LM) or RAG (LM + ARC encoder) or batched (standard or RAG).
pub enum AppStateVariant {
    Standard(Arc<AppStateInner>),
    Rag(Arc<AppStateRag>),
    Batched(Arc<BatchedState>),
}

pub struct AppStateInner {
    pub lm_model: moshi::lm::LmModel,
    pub mimi_model: moshi::mimi::Mimi,
    pub text_tokenizer: sentencepiece::SentencePieceProcessor,
    pub device: candle::Device,
    pub config: Config,
}

impl AppStateInner {
    fn text(
        &self,
        prev_text_token: u32,
        text_token: u32,
        config: &moshi::lm_generate_multistream::Config,
    ) -> Option<String> {
        decode_text_piece(&self.text_tokenizer, prev_text_token, text_token, config)
    }
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

/// RAG state: main LM + Mimi (via inner). STT removed — VAD is now PCM-energy-based.
pub struct AppStateRag {
    pub inner: AppState,
}

/// Batched state: one shared model loop & channel pool. Used when batch_size > 1.
pub struct BatchedState {
    pub inner: Arc<AppStateInner>,
    pub pool: Arc<crate::batched_channels::BatchedStreamingChannels>,
    pub rag_retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
    _loop_handle: Option<std::thread::JoinHandle<()>>,
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
        let inner_clone = inner.clone();
        let pool_clone = pool.clone();
        let rag_for_loop = rag_retrieval.clone();
        let loop_handle = std::thread::spawn(move || {
            if let Err(e) = Self::run_loop(inner_clone, pool_clone, rag_for_loop) {
                // Max step_idx reached is a normal end-of-stream; close thread without logging.
                if !e.to_string().contains("max step_idx reached") {
                    tracing::error!(err = %e, "batched model loop error");
                }
            }
        });
        Ok(Self { inner, pool, rag_retrieval, _loop_handle: Some(loop_handle) })
    }

    fn run_loop(
        inner: Arc<AppStateInner>,
        pool: Arc<crate::batched_channels::BatchedStreamingChannels>,
        rag_retrieval: Arc<crate::rag_retrieval::RagRetrievalEndpoints>,
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
        let batch_size = inner.lm_model.batch_size();
        let device = inner.device.clone();
        let rag_manager = inner.config.rag_token_id.map(|rag_token_id| {
            (
                crate::rag_manager::RagManager::new_with_two_step(
                    rag_retrieval.clone(),
                    inner.config.rag_timeout,
                ),
                rag_token_id,
            )
        });
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
        let turn_managers: Vec<Arc<Mutex<crate::turn_manager::TurnManager>>> =
            (0..batch_size)
                .map(|_| {
                    Arc::new(Mutex::new(crate::turn_manager::TurnManager::new(
                        vad_window,
                        vad_threshold,
                        vad_wait_steps,
                        init_speaker,
                    )))
                })
                .collect();

        tracing::info!("batched model loop started");
        loop {
            // Reset LM instance if necessary. Main LM reset inside pre_process.
            let (mut batch_pcm, mask, ref_channel_ids, reset_slots) = pool.pre_process(&mut state);

            for &bid in &reset_slots {
                if let Err(e) =
                    state.set_prepend_condition_lut(bid, "first_speaker", first_speaker_str)
                {
                    tracing::debug!(?e, bid, "set_prepend_condition_lut (no condition provider)");
                }
                if let Some(tm) = turn_managers.get(bid) {
                    tm.lock().unwrap().reset(init_speaker);
                }
            }

            // Check whether any retrieval task has completed and deliver result to the right slot.
            if let Some((ref rag_mgr, _)) = rag_manager {
                if let Some((slot_id, ref_text)) = rag_mgr.try_recv_result_slot() {
                    // Encoding of reference text is very fast and can definitely be done within the time of one frame.
                    if let Err(e) =
                        state.set_streaming_sum_condition(slot_id, "reference_with_time", &ref_text)
                    {
                        tracing::warn!(?e, slot_id, "set_streaming_sum_condition");
                    }
                    let out = if ref_text.trim().is_empty() {
                        StreamOut::TextByRole {
                            text: "[RET_FAILED]".to_string(),
                            role: TextRole::Model,
                        }
                    } else {
                        StreamOut::ReferenceText { text: ref_text }
                    };
                    if pool.send_to_slot(slot_id, out).is_err() {
                        tracing::debug!(slot_id, "failed to send RAG result to slot");
                    }
                }
            }

            let any_active = mask.iter().any(|&b| b);
            if any_active {
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

                for (b, &active) in mask.iter().enumerate().take(batch_size) {
                    if !active {
                        continue;
                    }
                    let start = b * pool.frame_size;
                    let end = start + pool.frame_size;
                    if end <= batch_pcm.len() {
                        let vad_value = crate::pcm_vad::compute_vad_probability(&batch_pcm[start..end]);
                        if let Some(tm) = turn_managers.get(b) {
                            tm.lock().unwrap().update_vad(vad_value);
                        }
                    }
                }

                let pcm_tensor = candle::Tensor::from_vec(
                    batch_pcm,
                    (pool.batch_size, 1, pool.frame_size),
                    &device,
                )?;
                let stream_mask = moshi::StreamMask::new(mask, &device)?;
                let msgs = state.step_pcm(&pcm_tensor, &stream_mask)?;

                if let Some((ref rag_mgr, rag_token_id)) = rag_manager {
                    for msg in &msgs {
                        if let moshi::batched_lm_generate_multistream::StreamingOutMsg::TextToken {
                            batch_idx,
                            prev_token: _,
                            token,
                            ..
                        } = msg
                        {
                            if *token == rag_token_id {
                                let bid = *batch_idx;
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

                for msg in &msgs {
                    match msg {
                        moshi::batched_lm_generate_multistream::StreamingOutMsg::TextToken { batch_idx, prev_token, token, role } => {
                            if matches!(role, moshi::batched_lm_generate_multistream::TextRole::Model) {
                                if let Some(text) =
                                    decode_text_piece(&inner.text_tokenizer, *prev_token, *token, &gen_config)
                                {
                                    let b = *batch_idx;
                                    let outputs = turn_managers.get(b).unwrap().lock().unwrap().handle_spoken_text(
                                        Some(text.as_str()),
                                        None,
                                    );
                                    for (text, role) in &outputs {
                                        pool.post_process(
                                            StreamOut::TextByRole { text: text.clone(), role: *role },
                                            b,
                                            &ref_channel_ids,
                                        )?;
                                    }
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
            text_seed: self.text_seed.unwrap_or_else(|| rand::thread_rng().gen()),
            audio_temperature: self.audio_temperature.unwrap_or(0.8),
            audio_topk: self.audio_topk.unwrap_or(250),
            audio_seed: self.audio_seed.unwrap_or_else(|| rand::thread_rng().gen()),
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

#[derive(serde::Serialize, serde::Deserialize, Debug, Clone)]
pub struct RetrievalBackendMeta {
    pub id: String,
}

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
    InputPcm {
        pcm_len: usize,
    },
    MetaData {
        metadata: Box<MetaData>,
    },
    StepStart {
        step: usize,
    },
    StepPostSampling {
        step: usize,
    },
    #[allow(dead_code)]
    Text {
        text: String,
    },
    /// For display of transcript with speaker (ColoredText: 0x07 + color_id + utf8).
    TextByRole {
        text: String,
        role: TextRole,
    },
    /// Reference text from RAG (ColoredReferenceText: 0x09 + color_id 4 + utf8).
    ReferenceText {
        text: String,
    },
    Pcm {
        pcm: Vec<f32>,
    },
}

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
