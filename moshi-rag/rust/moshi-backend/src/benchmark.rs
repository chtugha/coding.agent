// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

use crate::stream_both::{AppStateInner, Config, StreamOut};
use anyhow::Result;

#[derive(serde::Serialize)]
#[serde(tag = "type")]
enum Event {
    InputPcm { pcm_len: usize, time: f64 },
    Step { step: usize, time: f64 },
    StepPostSampling { step: usize, time: f64 },
    SendPcm { pcm_len: usize, time: f64 },
}

#[derive(serde::Serialize)]
struct StatsTracker {
    events: Vec<Event>,
}

fn system_time() -> f64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs_f64()
}

impl StatsTracker {
    fn new() -> Self {
        Self { events: vec![] }
    }

    fn on_update(&mut self, out: StreamOut) {
        match out {
            StreamOut::Pcm { pcm } => {
                self.events.push(Event::SendPcm { time: system_time(), pcm_len: pcm.len() });
            }
            StreamOut::MetaData { metadata } => {
                tracing::info!(?metadata, "send-metadata");
            }
            StreamOut::Text { text } => {
                tracing::info!(text, "send-text");
            }
            StreamOut::InputPcm { pcm_len } => {
                self.events.push(Event::InputPcm { time: system_time(), pcm_len });
            }
            StreamOut::StepStart { step } => {
                self.events.push(Event::Step { time: system_time(), step });
            }
            StreamOut::StepPostSampling { step } => {
                self.events.push(Event::StepPostSampling { time: system_time(), step });
            }
            StreamOut::Ready => {}
            StreamOut::TextByRole { .. } | StreamOut::ReferenceText { .. } => {}
        }
    }
}

pub async fn run(args: &crate::BenchmarkArgs, config: &Config) -> Result<()> {
    tracing::info!(
        avx = ?candle::utils::with_avx(),
        neon = ?candle::utils::with_neon(),
        simd128 = ?candle::utils::with_simd128(),
        f16c = ?candle::utils::with_f16c(),
        ?config,
        "cpu"
    );
    tracing::info!(?config, "starting benchmark");
    if args.mimi_only {
        let device = crate::standalone::create_device(args.cpu, config.moshi_gpu_id)?;
        let mimi_device = if config.use_cpu_for_mimi { &candle::Device::Cpu } else { &device };
        let mut mimi_model = moshi::mimi::load(
            &config.mimi_model_file,
            Some(config.mimi_num_codebooks),
            mimi_device,
        )?;
        let config = mimi_model.config();
        let frame_length = (config.sample_rate / config.frame_rate).ceil() as usize;
        for _step in 0..args.steps {
            let fake_pcm =
                candle::Tensor::zeros((1, 1, frame_length), candle::DType::F32, mimi_device)?;
            let codes = mimi_model.encode_step(&fake_pcm.into(), &().into())?;
            let ys = mimi_model.decode_step(&codes, &().into())?;
            if ys.as_option().is_none() {
                anyhow::bail!("Expected mimi to output some stuff, but nothing came out.");
            }
            device.synchronize()?;
        }
    } else {
        let standalone_args = crate::StandaloneArgs { cpu: args.cpu };
        let _state = std::sync::Arc::new(AppStateInner::new(&standalone_args, config)?);
        tracing::warn!("full LM benchmark not available (StreamingModel removed); use batched mode or mimi-only");
    }
    Ok(())
}
