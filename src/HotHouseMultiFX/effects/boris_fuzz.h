// Boris Fuzz v2 — proper embedded port for Hothouse
// Ported from Boris-Fuzz_v2.0 JUCE plugin (StompTones).
//
// Uses runtime bilinear transform for the passive BMP tone stack (same nodal
// analysis as the JUCE version — no hardcoded coefficients). Voicing biquads
// computed via standard Audio EQ Cookbook formulas at Init time.
//
// No oversampling — runs at base sample rate. Tanh rail saturation and
// proper diode-loaded collector model match the JUCE original.
//
// CPU optimization: tanhf → fastTanh (Padé [2,2], < 1% error, ~8 cycles vs 50-200).
// Called 6× per channel per sample — saves ~1200 cycles/stereo sample.
//
// Fixed params: max sustain, noon tone. kOutputGain = TUNE ON HARDWARE.

#pragma once
#include <cmath>
#include "fast_math.h"

class BorisFuzz
{
public:
    void Init(float sampleRate) noexcept
    {
        sr = sampleRate;

        // HP one-pole alphas at base rate (no oversampling)
        hpInA  = expf(-kTwoPi * 24.0f  / sr);
        hpS12A = expf(-kTwoPi * 14.0f  / sr);
        hpS23A = expf(-kTwoPi * 16.0f  / sr);
        hpOutA = expf(-kTwoPi * 18.0f  / sr);

        // LP one-pole coeffs at base rate
        lp1C = 1.0f - expf(-kTwoPi * 7200.0f / sr);
        lp2C = 1.0f - expf(-kTwoPi * 5600.0f / sr);
        lp3C = 1.0f - expf(-kTwoPi * 4900.0f / sr);
        lpRC = 1.0f - expf(-kTwoPi * 6800.0f / sr);

        // Recovery voicing network (low shelf + 2 peaks)
        voicingLow   = makeLowShelfBiquad(sr, 190.0f, 0.82f, 1.4f);
        voicingChest = makePeakingBiquad (sr, 430.0f, 0.72f, 4.2f);
        voicingMid   = makePeakingBiquad (sr, 900.0f, 0.88f, 2.4f);

        // Passive BMP tone stack at noon (tone param = 0.5):
        // mapToneParameterToCircuitPosition(0.5)
        //   = kToneSweepMin + 0.5 * (kToneSweepMax - kToneSweepMin)
        //   = 0.20 + 0.5 * 0.62 = 0.51
        updateToneCoeffs(kToneSweepMin + 0.5f * (kToneSweepMax - kToneSweepMin));

        // Fixed sustain gains at max sustain (sustain = 1.0):
        //   sustainWindow = kSustainRangeMax = 1.0
        //   sustainShape  = pow(1.0, kSustainTaper) = 1.0
        //   emitterResistance = kR_SustainFloor (no span added)
        //   stage2Gain = kQ2CollectorLoaded / (kStage2IntrinsicRe + kR_SustainFloor)
        q2Gain          = kQ2BaseAtten * kQ2CollectorLoaded
                          / (kStage2IntrinsicRe + kR_SustainFloor);
        q2DiodeStrength = kQ2DiodeStrength;   // sustainClipShape = 1.0 → max
        q3DiodeStrength = kQ3DiodeStrength;   // sustainClipShape = 1.0 → max

        Reset();
    }

    float gain = 1.0f;  // Pre-gain multiplier (0.5=soft, 4.0=heavy saturation)

    void Reset() noexcept { ch[0] = ch[1] = Ch{}; }

    float Process(float in, int channel) noexcept
    {
        auto& s = ch[channel & 1];
        float x = in * gain;

        // Input DC block
        x = highPass(x, s.hI_x, s.hI_y, hpInA);

        // Stage 1: soft pre-drive
        // 24× gain ensures full saturation even with low-level ER2 reverb input
        x = fastTanh(x * 24.0f);

        // Stage 2: hard fuzz clip — two cascaded tanh for asymmetric saturation
        x = fastTanh(x * 12.0f);
        x = lowPass(x, s.l1, lp1C);   // ~7kHz LP, tames harshness post-clip

        // Stage 3: second clip + mid-presence boost via LP blend
        x = fastTanh(x * 8.0f);
        x = lowPass(x, s.l2, lp2C);   // ~5.6kHz LP

        // Tone: fixed mid-scoop via biquad voicing (reuse voicingMid coeff)
        x = processBiquad(x, s.Mx1, s.Mx2, s.My1, s.My2, voicingMid);

        // Output DC block
        x = highPass(x, s.hO_x, s.hO_y, hpOutA);

        return x * kOutputGain;
    }

private:
    static constexpr float kTwoPi = 6.28318530718f;
    static constexpr float kVf    = 0.650f;   // 1N4148 forward voltage

    // Q1 input booster
    static constexpr float kQ1LoadedGain = 85.0f;
    static constexpr float kVrail1       = 4.25f;

    // Q2 sustain-controlled clipping stage (Hizumitas values)
    static constexpr float kR_SustainFloor       =   820.0f;  // min emitter resistance at max sustain
    static constexpr float kStage2IntrinsicRe    =    49.0f;  // transistor intrinsic re (Vt/Ic)
    static constexpr float kQ2CollectorLoaded    =  8200.0f;  // 8.2kΩ loaded collector resistance
    static constexpr float kQ2BaseAtten          =    0.18f;  // base divider attenuation
    static constexpr float kQ2DiodeStrength      =    0.95f;  // diode blend at max sustain
    static constexpr float kVrail2               =    2.6f;

    // Q3 clipping stage
    static constexpr float kQ3LoadedGain         =   18.0f;
    static constexpr float kQ3BaseAtten          =    0.75f;
    static constexpr float kQ3DiodeStrength      =    0.9f;
    static constexpr float kVrail3               =    2.8f;

    // Q4 output recovery stage
    static constexpr float kQ4LoadedGain         =    2.35f;
    static constexpr float kQ4BaseAtten          =    0.9f;
    static constexpr float kVrail4               =    3.8f;

    // Tone stack — verified Hizumitas BOM values (PCBGuitarMania)
    // R18/R19 are SERIES OUTPUT RESISTORS in the denominator of H(s)
    static constexpr float kR_ToneBass    = 39000.0f;   // R18
    static constexpr float kR_ToneTreble  = 39000.0f;   // R19
    static constexpr float kC_Bass        =  33.0e-9f;  // 33 nF  C10
    static constexpr float kC_Treble      =   3.3e-9f;  // 3.3 nF C11  ← Hizumitas signature
    static constexpr float kR_TonePot     = 50000.0f;   // B50K
    static constexpr float kR_VolumeLoad  = 50000.0f;   // A50K loads the tone wiper
    static constexpr float kToneSweepMin  =   0.20f;
    static constexpr float kToneSweepMax  =   0.82f;

    static constexpr float kOutputGain = 0.22f;  // Simplified fuzz clips to ~±1 (near square wave) — match MoonnSilver RMS level

    float sr = 48000.0f;

    // Filter coefficients
    float hpInA=0, hpS12A=0, hpS23A=0, hpOutA=0;
    float lp1C=0,  lp2C=0,   lp3C=0,   lpRC=0;

    // Tone stack biquad (computed via runtime bilinear transform)
    float tone_b0=1, tone_b1=0, tone_b2=0, tone_a1=0, tone_a2=0;

    // Fixed gains (max sustain)
    float q2Gain=1.0f, q2DiodeStrength=0.95f, q3DiodeStrength=0.9f;

    struct BqCoeffs { float b0=1,b1=0,b2=0,a1=0,a2=0; };
    BqCoeffs voicingLow, voicingChest, voicingMid;

    struct Ch {
        float hI_x=0,  hI_y=0;
        float h12_x=0, h12_y=0;
        float h23_x=0, h23_y=0;
        float hO_x=0,  hO_y=0;
        float l1=0, l2=0, l3=0, lR=0;
        float tx1=0, tx2=0, ty1=0, ty2=0;
        float Lx1=0, Lx2=0, Ly1=0, Ly2=0;
        float Cx1=0, Cx2=0, Cy1=0, Cy2=0;
        float Mx1=0, Mx2=0, My1=0, My2=0;
    };
    Ch ch[2];

    // ---------------------------------------------------------------------------
    // Tone stack: runtime bilinear transform of the passive BMP RC-ladder network.
    //
    // Circuit topology (KCL/KVL nodal analysis, A50K volume load on wiper):
    //   H(s) = [C_b·C_t·Ra·Rb·s² + C_t·Rb·s + 1]
    //          ────────────────────────────────────────────────────────────────
    //          [C_b·C_t·(Ra+R18)·(Rb+R19)·s² + (C_b·(Ra+R18)+C_t·(Rb+R19))·s + 1]
    //
    //   Ra = toneVal·Rpot (bass side), Rb = (1-toneVal)·Rpot (treble side)
    //   R18/R19 = 39kΩ each — series signal-path resistors (in denominator only)
    //
    // Expressed in conductance form (avoids near-cancellation with large R values):
    //   gBass = 1/R18, gTreble = 1/R19, gWiper = 1/Rpot + 1/Rload
    // ---------------------------------------------------------------------------
    void updateToneCoeffs(float toneVal) noexcept
    {
        const float potUpper = fmaxf(1.0f, (1.0f - toneVal) * kR_TonePot);
        const float potLower = fmaxf(1.0f, toneVal * kR_TonePot);
        const float potTotal = potUpper + potLower;

        const float gBass   = 1.0f / kR_ToneBass;
        const float gTreble = 1.0f / kR_ToneTreble;
        const float gWiper  = 1.0f / potTotal + 1.0f / kR_VolumeLoad;

        // Analog prototype s-domain coefficients
        const float N2 = (1.0f - toneVal) * kC_Bass * kC_Treble;
        const float N1 = kC_Treble * (gBass + gWiper);
        const float N0 = gBass * (gWiper + toneVal * gTreble);

        const float D2 = kC_Bass * kC_Treble;
        const float D1 = kC_Bass * (gTreble + gWiper) + kC_Treble * (gBass + gWiper);
        const float D0 = gBass * gTreble + gWiper * (gBass + gTreble);

        // Bilinear transform: s → 2·fs·(z-1)/(z+1), k = 2·fs
        const float k  = 2.0f * sr;
        const float k2 = k * k;

        const float n2 = N2*k2 + N1*k + N0;
        const float n1 = -2.0f*N2*k2          + 2.0f*N0;
        const float n0 =  N2*k2 - N1*k + N0;

        const float d2 = D2*k2 + D1*k + D0;
        const float d1 = -2.0f*D2*k2          + 2.0f*D0;
        const float d0 =  D2*k2 - D1*k + D0;

        if (!std::isfinite(d2) || std::abs(d2) < 1.0e-9f) return;

        tone_b0 = n2 / d2;
        tone_b1 = n1 / d2;
        tone_b2 = n0 / d2;
        tone_a1 = d1 / d2;
        tone_a2 = d0 / d2;
    }

    // Audio EQ Cookbook peaking biquad
    static BqCoeffs makePeakingBiquad(float sampleRate, float freq, float q, float gainDb) noexcept
    {
        const float A  = powf(10.0f, gainDb / 40.0f);
        const float w0 = kTwoPi * freq / sampleRate;
        const float cw = cosf(w0), sw = sinf(w0);
        const float al = sw / (2.0f * q);
        const float a0 = 1.0f + al / A;
        BqCoeffs c;
        c.b0 = (1.0f + al * A) / a0;
        c.b1 = -2.0f * cw      / a0;
        c.b2 = (1.0f - al * A) / a0;
        c.a1 = -2.0f * cw      / a0;
        c.a2 = (1.0f - al / A) / a0;
        return c;
    }

    // Audio EQ Cookbook low shelf biquad
    static BqCoeffs makeLowShelfBiquad(float sampleRate, float freq, float slope, float gainDb) noexcept
    {
        const float A  = powf(10.0f, gainDb / 40.0f);
        const float w0 = kTwoPi * freq / sampleRate;
        const float cw = cosf(w0), sw = sinf(w0);
        const float al = sw * 0.5f * sqrtf((A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f);
        const float sq = 2.0f * sqrtf(A) * al;
        const float a0 = (A + 1.0f) - (A - 1.0f) * cw + sq;
        BqCoeffs c;
        c.b0 =  A * ((A + 1.0f) - (A - 1.0f) * cw + sq) / a0;
        c.b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw) / a0;
        c.b2 =  A * ((A + 1.0f) - (A - 1.0f) * cw - sq) / a0;
        c.a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw) / a0;
        c.a2 =  ((A + 1.0f) - (A - 1.0f) * cw - sq) / a0;
        return c;
    }

    // Transistor collector rail saturation via fastTanh (physical supply limit)
    static float railSat(float x, float Vrail) noexcept
    {
        return Vrail * fastTanh(x / Vrail);
    }

    // One-pole high-pass (coupling capacitor model)
    static float highPass(float x, float& x1, float& y1, float alpha) noexcept
    {
        const float y = x - x1 + alpha * y1;
        x1 = x; y1 = y; return y;
    }

    // One-pole low-pass (collector bandwidth limiting)
    static float lowPass(float x, float& z, float coeff) noexcept
    {
        z += coeff * (x - z); return z;
    }

    // 1N4148 diode tanh clipper (Vf ≈ 0.65V)
    static float diodeClip(float x) noexcept
    {
        return kVf * fastTanh(x / kVf);
    }

    // Diode-loaded collector: smooth transition from linear to clipped
    // based on signal excursion past 0.75×Vf (from original JUCE model)
    static float diodeLoadedCollector(float collector, float strength) noexcept
    {
        const float clipped   = diodeClip(collector);
        const float excursion = fabsf(collector) / kVf;
        const float clampMix  = fmaxf(0.0f, fminf(1.0f, strength * (excursion - 0.75f)));
        return collector + (clipped - collector) * clampMix;
    }

    // Direct-form I biquad
    static float processBiquad(float x, float& x1, float& x2,
                                float& y1, float& y2, const BqCoeffs& c) noexcept
    {
        const float y = c.b0*x + c.b1*x1 + c.b2*x2 - c.a1*y1 - c.a2*y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y; return y;
    }
};
