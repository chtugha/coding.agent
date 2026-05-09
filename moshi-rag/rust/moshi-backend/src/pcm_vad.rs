// Copyright (c) Kyutai, all rights reserved.
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

pub fn compute_vad_probability(pcm_frame: &[f32]) -> f32 {
    if pcm_frame.is_empty() {
        return 0.0;
    }
    let rms = (pcm_frame.iter().map(|x| x * x).sum::<f32>() / pcm_frame.len() as f32).sqrt();
    let rms_db = 20.0 * (rms + 1e-8).log10();
    ((rms_db + 50.0) / 30.0).clamp(0.0, 1.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn silence_returns_near_zero() {
        let frame = vec![0.0_f32; 1920];
        let prob = compute_vad_probability(&frame);
        assert!(prob < 0.01, "silence should be near 0.0, got {prob}");
    }

    #[test]
    fn loud_signal_returns_near_one() {
        let frame: Vec<f32> = (0..1920)
            .map(|i| 0.5 * (2.0 * std::f32::consts::PI * 440.0 * i as f32 / 24000.0).sin())
            .collect();
        let prob = compute_vad_probability(&frame);
        assert!(prob > 0.95, "loud signal should be near 1.0, got {prob}");
    }

    #[test]
    fn intermediate_level() {
        let amplitude = 0.01_f32;
        let frame: Vec<f32> = (0..1920)
            .map(|i| amplitude * (2.0 * std::f32::consts::PI * 440.0 * i as f32 / 24000.0).sin())
            .collect();
        let prob = compute_vad_probability(&frame);
        assert!(prob > 0.05 && prob < 0.95, "intermediate signal should be between 0 and 1, got {prob}");
    }

    #[test]
    fn empty_frame_returns_zero() {
        let prob = compute_vad_probability(&[]);
        assert_eq!(prob, 0.0);
    }

    #[test]
    fn very_quiet_returns_zero() {
        let amplitude = 1e-7_f32;
        let frame = vec![amplitude; 1920];
        let prob = compute_vad_probability(&frame);
        assert!(prob < 0.01, "very quiet should be ~0.0, got {prob}");
    }
}
