// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

use anyhow::{Context, Result};
use std::sync::Arc;

use crate::{stream_both, StandaloneArgs};

#[derive(serde::Deserialize, Debug, Clone)]
pub struct Config {
    #[serde(default)]
    cert_dir: String,
    #[serde(default)]
    pub static_dir: String,
    /// Retained for backwards-compatible config file deserialization (legacy HTTP server mode).
    #[allow(dead_code)]
    #[serde(default)]
    addr: String,
    /// Retained for backwards-compatible config file deserialization (legacy HTTP server mode).
    #[allow(dead_code)]
    #[serde(default)]
    port: u16,
    /// Retained for backwards-compatible config file deserialization (legacy HTTP server mode).
    #[allow(dead_code)]
    #[serde(default = "default_true")]
    use_https: bool,

    #[serde(flatten)]
    pub stream: stream_both::Config,
}

fn default_true() -> bool {
    true
}

impl Config {
    pub fn load<P: AsRef<std::path::Path>>(p: P) -> Result<Self> {
        let config = std::fs::read_to_string(p)?;
        let mut config: Self = serde_json::from_str(&config)?;
        config.static_dir = crate::utils::replace_env_vars(&config.static_dir);
        config.cert_dir = crate::utils::replace_env_vars(&config.cert_dir);
        config.stream.log_dir = crate::utils::replace_env_vars(&config.stream.log_dir);
        config.stream.text_tokenizer_file =
            crate::utils::replace_env_vars(&config.stream.text_tokenizer_file);
        config.stream.mimi_model_file =
            crate::utils::replace_env_vars(&config.stream.mimi_model_file);
        config.stream.lm_model_file = crate::utils::replace_env_vars(&config.stream.lm_model_file);
        if let Some(ref mut s) = config.stream.stt_lm_model_file {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.stream.stt_text_tokenizer_file {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.stream.stt_mimi_model_file {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.stream.arc_encoder_tokenizer_path {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.stream.arc_encoder_model_file {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.stream.backend_url {
            *s = crate::utils::replace_env_vars(s);
        }
        crate::stream_both::parse_retrieval_llms_json(&mut config.stream)?;
        Ok(config)
    }
}

pub(crate) fn create_device(cpu: bool, gpu_id: usize) -> Result<candle::Device> {
    use candle::Device;
    if cpu {
        Ok(Device::Cpu)
    } else if candle::utils::cuda_is_available() {
        Ok(Device::new_cuda(gpu_id)?)
    } else if candle::utils::metal_is_available() {
        Ok(Device::new_metal(gpu_id)?)
    } else {
        Ok(Device::Cpu)
    }
}

impl stream_both::AppStateInner {
    pub fn new(args: &StandaloneArgs, config: &stream_both::Config) -> Result<Self> {
        let device = create_device(args.cpu, config.moshi_gpu_id)?;
        tracing::info!(
            "Loading Moshi LM on GPU {} (config moshi_gpu_id={}, device={:?})",
            config.moshi_gpu_id,
            config.moshi_gpu_id,
            device
        );
        let stt_device = if config.stt_gpu_id == config.moshi_gpu_id {
            device.clone()
        } else {
            create_device(args.cpu, config.stt_gpu_id)?
        };

        let is_gguf = config.lm_model_file.ends_with(".gguf");
        let dtype = if is_gguf {
            candle::DType::F32
        } else if device.is_cuda() || device.is_metal() {
            candle::DType::BF16
        } else {
            candle::DType::F32
        };
        let batch_size = if config.batch_size > 1 { config.batch_size } else { 1 };
        // Uses config.use_arc() to choose LM loader (ARC encoder only loaded when explicitly enabled).
        let lm_model = if config.use_arc() {
            let arc_encoder_tokenizer_path = config
                .arc_encoder_tokenizer_path
                .as_deref()
                .expect("ARC config requires arc_encoder_tokenizer_path");
            let arc_encoder_weights =
                config.arc_encoder_model_file.as_deref().map(std::path::Path::new);
            moshi::lm::load_streaming_rag_batched(
                batch_size,
                &config.lm_model_file,
                dtype,
                &device,
                arc_encoder_tokenizer_path,
                arc_encoder_weights,
            )?
        } else {
            moshi::lm::load_streaming_batched(batch_size, &config.lm_model_file, dtype, &device)?
        };
        let mimi_device = if config.use_cpu_for_mimi { &candle::Device::Cpu } else { &device };
        tracing::info!(
            "Loading Mimi audio codec on {:?} (use_cpu_for_mimi={})",
            mimi_device,
            config.use_cpu_for_mimi
        );
        let mimi_model = moshi::mimi::load(
            &config.mimi_model_file,
            Some(config.mimi_num_codebooks),
            mimi_device,
        )?;
        let text_tokenizer =
            sentencepiece::SentencePieceProcessor::open(&config.text_tokenizer_file)?;

        // Load in-process STT model when configured.
        let (stt_asr, stt_tokenizer) = if config.use_stt() {
            let stt_lm_file = config.stt_lm_model_file.as_deref().unwrap();
            let stt_mimi_file = config.stt_mimi_model_file.as_deref().unwrap();
            let stt_tok_file = config.stt_text_tokenizer_file.as_deref().unwrap();
            tracing::info!(
                "Loading STT-1b LM and Mimi on {:?} (stt_gpu_id={})",
                stt_device,
                config.stt_gpu_id
            );
            let stt_dtype = if stt_device.is_cuda() || stt_device.is_metal() {
                candle::DType::BF16
            } else {
                candle::DType::F32
            };
            let stt_lm = moshi::lm::load_asr_stt_1b_en_fr(
                batch_size,
                stt_lm_file,
                stt_dtype,
                &stt_device,
            )?;
            let stt_mimi_device =
                if config.use_cpu_for_mimi { &candle::Device::Cpu } else { &stt_device };
            let stt_mimi =
                moshi::mimi::load_b(Some(batch_size), stt_mimi_file, Some(32), stt_mimi_device)?;
            let asr_delay = config.asr_delay_in_tokens.unwrap_or(0);
            let stt_asr_state = moshi::asr::State::new(batch_size, asr_delay, 0.0, stt_mimi, stt_lm)?;
            let stt_tok = sentencepiece::SentencePieceProcessor::open(stt_tok_file)?;
            tracing::info!("STT-1b loaded successfully");
            (Some(stt_asr_state), Some(stt_tok))
        } else {
            (None, None)
        };

        // Warm-up code.
        {
            let mut lm_model = lm_model.clone();
            if batch_size <= 1 {
                let (_v, ys) =
                    lm_model.forward(None, vec![None; config.mimi_num_codebooks], &().into())?;
                let mut lp = candle_transformers::generation::LogitsProcessor::new(123, None, None);
                let _ = lm_model.depformer_sample(&ys, None, &[], &mut lp)?;
            } else {
                // Batched warm-up: one forward with all slots active, then one batched DepFormer step.
                let mask = moshi::StreamMask::new(vec![true; config.batch_size], &device)?;
                let start = lm_model.text_start_token();
                let text_ids = candle::Tensor::from_vec(
                    vec![start; config.batch_size],
                    (config.batch_size, 1),
                    &device,
                )?;
                let (_logits, ys) = lm_model.forward(
                    Some(text_ids),
                    vec![None; config.mimi_num_codebooks],
                    &mask,
                )?;
                let mut audio_lp: Vec<_> = (0..config.batch_size)
                    .map(|_| candle_transformers::generation::LogitsProcessor::new(123, None, None))
                    .collect();
                let text_tokens: Vec<Option<u32>> =
                    (0..config.batch_size).map(|_| Some(start)).collect();
                let forced: Vec<Vec<Option<u32>>> =
                    (0..config.batch_size).map(|_| vec![]).collect();
                let _ = lm_model.depformer_sample_batched(
                    &ys,
                    &text_tokens,
                    &forced,
                    &mask,
                    &mut audio_lp,
                )?;
            }
            let mut mimi_model = mimi_model.clone();
            let mimi_config = mimi_model.config();
            let frame_length = (mimi_config.sample_rate / mimi_config.frame_rate).ceil() as usize;
            let mimi_dtype = if mimi_device.is_cuda() {
                candle::DType::BF16
            } else {
                candle::DType::F32
            };
            let fake_pcm =
                candle::Tensor::zeros((1, 1, frame_length), mimi_dtype, mimi_device)?;
            let codes = mimi_model.encode_step(&fake_pcm.into(), &().into())?;
            let ys = mimi_model.decode_step(&codes, &().into())?;
            if ys.as_option().is_none() {
                anyhow::bail!("Expected Mimi to output some stuff, but nothing came out.");
            }
            device.synchronize()?;
            tracing::info!("model is ready to roll!");
        }
        Ok(Self {
            lm_model,
            mimi_model,
            text_tokenizer,
            device,
            stt_asr: std::sync::Mutex::new(stt_asr),
            stt_tokenizer: std::sync::Mutex::new(stt_tokenizer),
            config: config.clone(),
        })
    }
}

pub async fn download_from_hub(config: &mut stream_both::Config) -> Result<()> {
    let token = std::env::var("HF_TOKEN").or_else(|_| std::env::var("HUGGING_FACE_HUB_TOKEN")).ok();

    let api = if let Some(token) = token {
        hf_hub::api::tokio::ApiBuilder::new().with_token(Some(token)).build()?
    } else {
        hf_hub::api::tokio::ApiBuilder::from_env().build()?
    };
    let hf = (!config.hf_repo.is_empty()).then_some(config.hf_repo.as_str());

    for file_path in
        [&mut config.lm_model_file, &mut config.mimi_model_file, &mut config.text_tokenizer_file]
            .iter_mut()
    {
        crate::hf_path::resolve_model_file(&api, file_path, hf).await?;
    }

    if config.use_stt() {
        if let Some(ref mut path) = config.stt_lm_model_file {
            crate::hf_path::resolve_model_file(&api, path, hf).await?;
        }
        if let Some(ref mut path) = config.stt_mimi_model_file {
            crate::hf_path::resolve_model_file(&api, path, hf).await?;
        }
        if let Some(ref mut path) = config.stt_text_tokenizer_file {
            crate::hf_path::resolve_model_file(&api, path, hf).await?;
        }
    }
    if config.use_arc() {
        let arc_path = config
            .arc_encoder_tokenizer_path
            .as_mut()
            .expect("ARC requires arc_encoder_tokenizer_path");
        crate::hf_path::resolve_model_file(&api, arc_path, hf).await?;
        if let Some(ref mut path) = config.arc_encoder_model_file {
            crate::hf_path::resolve_model_file(&api, path, hf).await?;
        }
    }
    Ok(())
}

pub async fn run(
    args: &StandaloneArgs,
    config: &Config,
    log_forwarder: Arc<crate::log_forwarder::LogForwarder>,
) -> Result<()> {
    let batch_size = if config.stream.batch_size > 1 {
        config.stream.batch_size
    } else {
        anyhow::bail!("prodigy bridge requires batch_size > 1 in config");
    };

    let inner = Arc::new(stream_both::AppStateInner::new(args, &config.stream)?);
    let batched_state = stream_both::BatchedState::new(inner)?;
    let batched_state = Arc::new(batched_state);
    tracing::info!(
        "model loaded (Batch size: {}, RAG: {})",
        config.stream.batch_size,
        config.stream.use_rag()
    );

    let running = Arc::new(std::sync::atomic::AtomicBool::new(true));
    crate::prodigy_bridge::install_signal_handler(running.clone());

    let cmd_listener = crate::interconnect::bind_listen(crate::interconnect::CMD_PORT)?;
    let status_provider = crate::prodigy_bridge::run_prodigy_bridge(
        batched_state.pool.clone(),
        running.clone(),
        batch_size,
        config.stream.lm_model_file.clone(),
        log_forwarder.clone(),
        batched_state.model_loop_ready.clone(),
    )?;

    let cmd_running = running.clone();
    let cmd_fwd = log_forwarder.clone();
    let cmd_handle = std::thread::Builder::new()
        .name("cmd-port".into())
        .spawn(move || {
            if let Err(e) =
                crate::cmd_port::run_cmd_listener(cmd_listener, cmd_running, cmd_fwd, status_provider)
            {
                tracing::error!("CMD listener error: {}", e);
            }
        })
        .context("failed to spawn cmd-port thread")?;

    tracing::info!("moshi-rag-service running, waiting for shutdown signal");

    while running.load(std::sync::atomic::Ordering::Relaxed) {
        tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    }

    tracing::info!("shutdown signal received, stopping");
    running.store(false, std::sync::atomic::Ordering::SeqCst);

    let _ = cmd_handle.join();

    Ok(())
}
