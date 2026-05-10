// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

use anyhow::Result;
use std::sync::Arc;

use crate::{stream_both, StandaloneArgs};

#[derive(serde::Deserialize, Debug, Clone)]
pub struct Config {
    #[serde(default)]
    cert_dir: String,
    #[serde(default)]
    pub static_dir: String,
    #[serde(default)]
    addr: String,
    #[serde(default)]
    port: u16,
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
        if let Some(ref mut s) = config.stream.arc_encoder_tokenizer_path {
            *s = crate::utils::replace_env_vars(s);
        }
        if let Some(ref mut s) = config.stream.arc_encoder_model_file {
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

        let dtype = if device.is_cuda() || device.is_metal() { candle::DType::BF16 } else { candle::DType::F32 };
        let batch_size = if config.batch_size > 1 { config.batch_size } else { 1 };
        // Uses config.use_rag() to choose LM loader.
        let lm_model = if config.use_rag() {
            let arc_encoder_tokenizer_path = config
                .arc_encoder_tokenizer_path
                .as_deref()
                .expect("RAG config requires arc_encoder_tokenizer_path");
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
            let fake_pcm =
                candle::Tensor::zeros((1, 1, frame_length), candle::DType::F32, mimi_device)?;
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
            config: config.clone(),
        })
    }
}

impl stream_both::AppStateRag {
    pub fn new(args: &StandaloneArgs, config: &stream_both::Config) -> Result<Self> {
        let inner = stream_both::AppStateInner::new(args, config)?;
        let inner = std::sync::Arc::new(inner);
        tracing::info!("RAG models loaded (main LM + ARC encoder). Main device: {:?}", inner.device);
        Ok(Self { inner })
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

    if config.use_rag() {
        let arc_path = config
            .arc_encoder_tokenizer_path
            .as_mut()
            .expect("RAG requires arc_encoder_tokenizer_path");
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
    let _state: Arc<stream_both::AppStateVariant> = if config.stream.batch_size > 1 {
        let inner = Arc::new(stream_both::AppStateInner::new(args, &config.stream)?);
        let batched_state = stream_both::BatchedState::new(inner)?;
        Arc::new(stream_both::AppStateVariant::Batched(Arc::new(batched_state)))
    } else if config.stream.use_rag() {
        Arc::new(stream_both::AppStateVariant::Rag(Arc::new(stream_both::AppStateRag::new(
            args,
            &config.stream,
        )?)))
    } else {
        Arc::new(stream_both::AppStateVariant::Standard(Arc::new(stream_both::AppStateInner::new(
            args,
            &config.stream,
        )?)))
    };
    tracing::info!(
        "model loaded (Batch size: {}, RAG: {})",
        config.stream.batch_size,
        config.stream.use_rag()
    );
    let _ = log_forwarder;
    tracing::info!("prodigy bridge will be wired in a future step");
    Ok(())
}
