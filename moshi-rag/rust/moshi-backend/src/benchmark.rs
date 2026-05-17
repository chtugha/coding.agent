// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

use crate::stream_both::{AppStateInner, Config};
use anyhow::Result;

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
