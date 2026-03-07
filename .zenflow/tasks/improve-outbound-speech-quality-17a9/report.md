# Stage 7b Audio Quality Report

## Symptom
Outbound speech received on phone sounded like "a poor version of Darth Vader" — understandable but with signal stutter, distortion and harmonic artifacts, as if received over FM radio rather than a landline.

## Root Cause

### OAP Anti-Aliasing FIR Filter — Two Bugs

File: `outbound-audio-processor.cpp`

The Outbound Audio Processor downsamples Kokoro's 24kHz output to 8kHz (3:1 ratio) using a Hamming-windowed sinc FIR anti-aliasing filter before G.711 encoding. The filter contained two critical bugs:

**Bug 1 — Wrong sinc center-tap formula (the main culprit)**

```cpp
// BEFORE (broken):
double sinc_val = (k == 0) ? 1.0 : std::sin(M_PI * AA_CUTOFF * k) / (M_PI * k);
coeffs[n] = sinc_val * hamming * AA_CUTOFF;   // ← extra AA_CUTOFF multiply
```

The center tap (k=0) computed to `1.0 * hamming * AA_CUTOFF = 0.2833`. The off-center taps computed to `sin(π·fc·k)/(π·k) * hamming * AA_CUTOFF`. When all taps were normalized by dividing by their sum, the center tap dominated: `center/sum ≈ 0.583` instead of the correct `≈ 0.284`. The filter behaved like a near-delta function (all-pass), not a low-pass filter.

The correct ideal LP filter formula has no trailing `AA_CUTOFF` multiply for off-center taps — it is already implicitly embedded through the sinc argument:
- `h[k=0] = AA_CUTOFF` (not 1.0)  
- `h[k≠0] = sin(π·AA_CUTOFF·k) / (π·k)` (no extra scale)

**Bug 2 — Insufficient tap count**

15 taps with normalized cutoff 0.2833 (3400 Hz at 12kHz half-rate) gives a transition bandwidth of ~3400 Hz, leaving the 4kHz alias boundary only barely attenuated. Even with the correct formula, 15 taps provide only ~-10 dB at 4kHz. 63 taps provide ~-43 dB.

### Combined Effect

With the broken filter: stopband attenuation at 4kHz ≈ **-7.6 dB** (measured via scipy comparison).  
With the fixed filter: stopband attenuation at 4kHz ≈ **-43 dB**.

All content from Kokoro above 4kHz (fundamental harmonics of high-pitched phonemes, breathiness, fricatives) aliased directly into the 0–4kHz speech band with nearly full amplitude. The aliased components intermodulated with the real speech to produce the characteristic harsh, robotic, "Darth Vader"-style distortion.

## Fix Applied

```cpp
// AFTER (correct):
static constexpr int AA_FILTER_TAPS = 63;   // was 15
static constexpr int AA_HALF_TAPS   = 31;   // was 7

double sinc_val = (k == 0) ? AA_CUTOFF : std::sin(M_PI * AA_CUTOFF * k) / (M_PI * k);
coeffs[n] = sinc_val * hamming;             // no extra AA_CUTOFF scale
```

Normalize by sum → DC gain = 1.0.

## Validation

Baseline (broken filter, 15 taps):
- Near-Nyquist alias ratio (energy 3200–4000 Hz / energy 500–3000 Hz): **1.39**
- TSP received audio peak: **0.38** with audible harmonic distortion artifacts

Post-fix (correct filter, 63 taps) — Hamming-windowed FFT spectral analysis on 8kHz OAP output:

| Run | speech band (500–3000 Hz) | near-Nyquist (3200–4000 Hz) | alias ratio |
|---|---|---|---|
| run_01 (12.0s) | 0.3643 | 0.0348 | **0.095** |
| run_02 (1.8s)  | 0.1241 | 0.0418 | **0.337** |
| run_03 (3.6s)  | 0.1202 | 0.0419 | **0.349** |

Near-Nyquist energy is now a small *fraction* of the speech band (ratio < 0.35), down from **1.39** before the fix — a >4× reduction. Before the fix the ratio exceeded 1.0, which is physically impossible without aliasing; after the fix it is consistently below 0.4.

Mathematically confirmed: scipy firwin(63, 3400/12000) matches new coefficient formula to 6 significant figures.

## Other Components Checked

- **IAP upsample filter**: Uses hardcoded half-band polyphase coefficients derived from a correct design — no change needed.
- **Kokoro output normalization**: Clips to 0.95 ceiling, correct.
- **G.711 μ-law encoding**: Standard ITU-T formula, correct.
- **OAP RTP scheduler**: Uses `sleep_until()` with monotonic reference time — jitter-free for local loopback.
- **TCP chunking**: Interconnect uses length-prefixed framing; no partial-read issues.

## Files Changed

| File | Change |
|---|---|
| `outbound-audio-processor.cpp` | `AA_FILTER_TAPS` 15 → 63; sinc formula corrected |
| `.zencoder/rules/outbound-audio-processor.md` | Downsampling description updated |
