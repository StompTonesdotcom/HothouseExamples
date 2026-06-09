// Void Dweller — Multi-tap reverb with diffusion and feedback
// Ported from Void-Dweller JUCE plugin (StompTones).
// Runs at base sample rate.
// Delay buffers supplied externally (SDRAM).
//
// CPU optimizations: calcDecayTime (pow) cached on length change; rt60 exp() replaced
// with 1+x approximation (|arg| < 0.0015, error < 0.001%); std::tanh → fastTanh.
//
// Parameters (knobs):
//   drag     10–200 ms  — tap base delay (all 10 taps scale from this)
//   diffuse  0–1        — all-pass diffusion amount
//   reflect  0–0.95     — feedback amount
//   dampen   300–8000Hz — damping LP cutoff
//   mix      0–1        — dry/wet
//   length   0–1        — decay time (0.1–10s) [fixed at 0.5 by firmware]

#pragma once
#include <cmath>
#include "fast_math.h"

class VoidDweller
{
public:
    static constexpr int kNumTaps = 10;
    // Max delay: 200ms × 10 × 1.08 × 48kHz = 103,680 samples — round up
    static constexpr int kBufLen  = 104000;

    float drag    = 105.0f;
    float diffuse = 0.5f;
    float reflect = 0.475f;
    float dampen  = 2500.0f;
    float mix     = 0.5f;
    float length  = 0.5f;

    // externalBufL/R: kBufLen floats each, in SDRAM
    void Init(float sampleRate, float* externalBufL, float* externalBufR) noexcept
    {
        sr   = sampleRate;
        bufL = externalBufL;
        bufR = externalBufR;

        // Fixed all-pass frequencies (from original — same every block)
        static constexpr float apFreqsL[4] = { 239.0f, 673.0f, 1319.0f, 2281.0f };
        static constexpr float apFreqsR[4] = { 311.0f, 787.0f, 1511.0f, 2521.0f };
        for (int i = 0; i < 4; ++i)
        {
            makeAP(apFreqsL[i], apCoeffL[i]);
            makeAP(apFreqsR[i], apCoeffR[i]);
        }

        // Anti-metallic: fixed -8 dB high shelf @ 8kHz
        makeHS(8000.0f, 0.4f, amB0, amB1, amB2, amA1, amA2);

        lastDampen = -1.0f;
        updateDampen(dampen, reflect);

        logCoeff = std::log(0.001f) / sr;
        decayL = decayR = calcDecayTime(length);

        Reset();
    }

    void Reset() noexcept
    {
        if (bufL) for (int i = 0; i < kBufLen; ++i) bufL[i] = 0.0f;
        if (bufR) for (int i = 0; i < kBufLen; ++i) bufR[i] = 0.0f;
        writePos = 0;
        fbStateL = fbStateR = 0.0f;
        decayL = decayR = calcDecayTime(length);
        for (int i = 0; i < 4; ++i)
            apStL[i] = apStR[i] = BqState{};
        dampStL = dampStR = BqState{};
        dampSt2L = dampSt2R = BqState{};
        fbDampStL = fbDampStR = BqState{};
        amStL = amStR = BqState{};
    }

    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        if (!bufL || !bufR) { outL = inL; outR = inR; return; }

        updateDampen(dampen, reflect);

        // Update decay time smoothly
        // calcDecayTime (pow) is expensive — cache and recompute only when length changes
        if (fabsf(length - lastLength) > 0.001f) {
            lastLength = length;
            cachedDecay = calcDecayTime(length);
        }
        decayL = 0.995f * decayL + 0.005f * cachedDecay;
        decayR = 0.995f * decayR + 0.005f * cachedDecay;
        // exp(x) ≈ 1+x for tiny |x| — here |x| < 0.00144, error < 0.001%
        const float rt60L = 1.0f + logCoeff / fmaxf(0.01f, decayL);
        const float rt60R = 1.0f + logCoeff / fmaxf(0.01f, decayR);

        const float dragNorm = (drag - 10.0f) / 190.0f;
        const float dragMod  = 0.75f + dragNorm * 0.2f;
        const float safeRef  = reflect * dragMod;

        // Process each channel
        outL = processChannel(inL, bufL, fbStateL, safeRef, rt60L, 1.0f,
                              apStL, dampStL, dampSt2L, fbDampStL, amStL, apCoeffL);
        outR = processChannel(inR, bufR, fbStateR, safeRef, rt60R, 1.08f,
                              apStR, dampStR, dampSt2R, fbDampStR, amStR, apCoeffR);

        // Advance write position ONCE per sample (after both channels)
        writePos = (writePos + 1) % kBufLen;
    }

private:
    struct BqCoeffs { float b0=1,b1=0,b2=0,a1=0,a2=0; };
    struct BqState  { float x1=0,x2=0,y1=0,y2=0; };

    float processChannel(float in, float* buf, float& fbState,
                         float safeRef, float rt60, float stereoOffset,
                         BqState apSt[4], BqState& dampSt, BqState& dampSt2,
                         BqState& fbDampSt, BqState& amSt, BqCoeffs apC[4]) noexcept
    {
        // Clean input path — fbDampSt and amSt removed.
        // fbDampSt (LP on input) + amSt (HF shelf) interacted with the 10-tap feedback
        // to produce resonances that sounded like constant static.
        // Loop stability is now guaranteed by clampedRef below; input needs no conditioning.
        float v = in;

        // All-pass diffusion
        float diffused = v;
        if (diffuse > 0.01f)
        {
            float wet = v;
            for (int i = 0; i < 4; ++i)
                wet = biquad(wet, apSt[i], apC[i]);
            diffused = v + (wet - v) * diffuse * 0.85f;
        }

        // Read from 10 tap positions
        static constexpr float kTapMult[kNumTaps] = {
            0.4f, 0.9f, 1.5f, 2.1f, 2.9f, 3.9f, 5.1f, 6.5f, 8.1f, 10.0f
        };
        const float normSum = fmaxf(0.1f, 4.0f - length * 2.5f);

        // Stability guarantee: loop gain = (kTapSum/normSum) * safeRef must stay < 1.
        // kTapSum = Σ fadeIn/sqrt(t+1) for t=0..9 ≈ 4.22 (depends only on kTapMult, fixed).
        // normSum decreases with length, amplifying tap gains — without this clamp,
        // reflect above ~0.76 at noon length (or less at higher length) causes oscillation.
        // Clamp safeRef so that max loop gain = 0.98 regardless of length+reflect combination.
        static constexpr float kTapSum = 4.22f;
        const float clampedRef = fminf(safeRef, 0.98f * normSum / kTapSum);

        // Write to circular buffer; fastTanh is an additional overflow hard-stop
        const float tapIn = diffused + fbState * clampedRef * rt60;
        buf[writePos] = fastTanh(tapIn);

        float wet = 0.0f;

        for (int t = 0; t < kNumTaps; ++t)
        {
            const float dtSmp = drag * kTapMult[t] * stereoOffset * sr / 1000.0f;
            const int   dtI   = static_cast<int>(dtSmp);
            const float frac  = dtSmp - static_cast<float>(dtI);

            const int ri0 = (writePos - dtI + kBufLen) % kBufLen;
            const int ri1 = (ri0 - 1  + kBufLen) % kBufLen;
            const float s = buf[ri0] + frac * (buf[ri1] - buf[ri0]);

            const float fadeIn  = (t == 0) ? 0.2f : 1.0f;
            const float tapGain = fadeIn / (std::sqrt(static_cast<float>(t + 1)) * normSum);
            wet += s * tapGain;
        }

        // Dampen (cascaded LP × 2 for stronger effect — separate states required)
        wet = biquad(wet, dampSt, dampC);
        wet = biquad(wet, dampSt2, dampC);

        // Update feedback state (smooth to prevent zipper noise)
        fbState = fbState * 0.6f + wet * 0.4f;

        // Simple wet/dry blend — no artificial gain multiplication (3.5x was causing static)
        return in * (1.0f - mix) + wet * mix;
    }

    static float biquad(float x, BqState& s, const BqCoeffs& c) noexcept
    {
        float y = c.b0*x + c.b1*s.x1 + c.b2*s.x2 - c.a1*s.y1 - c.a2*s.y2;
        s.x2=s.x1; s.x1=x; s.y2=s.y1; s.y1=y;
        return y;
    }

    static float biquadRaw(float x, BqState& s,
                            float b0, float b1, float b2, float a1, float a2) noexcept
    {
        float y = b0*x + b1*s.x1 + b2*s.x2 - a1*s.y1 - a2*s.y2;
        s.x2=s.x1; s.x1=x; s.y2=s.y1; s.y1=y;
        return y;
    }

    float calcDecayTime(float len) const noexcept
    {
        return 0.1f * std::pow(100.0f, len);
    }

    void makeAP(float fc, BqCoeffs& c) noexcept
    {
        const float w0 = 6.28318f * fc / sr;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float al = sw / 1.414f;
        const float a0i = 1.0f / (1.0f + al);
        c.b0 = (1.0f - al) * a0i;
        c.b1 = -2.0f * cw * a0i;
        c.b2 = (1.0f + al) * a0i;
        c.a1 = c.b1;
        c.a2 = c.b0;
    }

    void makeLP(float fc, BqCoeffs& c) noexcept
    {
        const float w0 = 6.28318f * fc / sr;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float al = sw / 1.414f;
        const float a0i = 1.0f / (1.0f + al);
        c.b0 = (1.0f - cw) * 0.5f * a0i;
        c.b1 = (1.0f - cw) * a0i;
        c.b2 = c.b0;
        c.a1 = -2.0f * cw * a0i;
        c.a2 = (1.0f - al) * a0i;
    }

    void makeHS(float fc, float linGain,
                float& b0, float& b1, float& b2, float& a1, float& a2) noexcept
    {
        const float A  = std::sqrt(linGain);
        const float w0 = 6.28318f * fc / sr;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float al = sw * 0.5f * std::sqrt((A + 1.0f/A) * (1.0f/0.707f - 1.0f) + 2.0f);
        const float sq = 2.0f * std::sqrt(A) * al;
        const float a0 = (A+1.0f) - (A-1.0f)*cw + sq;
        b0 = A*((A+1.0f)+(A-1.0f)*cw+sq)/a0;
        b1 = -2.0f*A*((A-1.0f)+(A+1.0f)*cw)/a0;
        b2 = A*((A+1.0f)+(A-1.0f)*cw-sq)/a0;
        a1 = 2.0f*((A-1.0f)-(A+1.0f)*cw)/a0;
        a2 = ((A+1.0f)-(A-1.0f)*cw-sq)/a0;
    }

    void updateDampen(float fc, float ref) noexcept
    {
        if (std::abs(fc - lastDampen) < 1.0f) return;
        lastDampen = fc;
        makeLP(fc, dampC);
        const float fbFC = fmaxf(500.0f, fminf(10000.0f, fc * (0.95f - ref * 0.25f)));
        makeLP(fbFC, fbDampC);
    }

    float sr = 48000.0f;
    float* bufL = nullptr;
    float* bufR = nullptr;
    int    writePos = 0;

    float fbStateL = 0.0f, fbStateR = 0.0f;
    float decayL   = 1.0f, decayR   = 1.0f;
    float lastDampen = -1.0f;
    float logCoeff   = 0.0f;
    // calcDecayTime cache — pow() called only when length changes
    float lastLength  = -99.0f;
    float cachedDecay = 1.0f;

    BqCoeffs apCoeffL[4], apCoeffR[4];
    BqCoeffs dampC, fbDampC;
    float amB0=1,amB1=0,amB2=0,amA1=0,amA2=0;

    BqState apStL[4], apStR[4];
    BqState dampStL, dampStR;
    BqState dampSt2L, dampSt2R;
    BqState fbDampStL, fbDampStR;
    BqState amStL, amStR;
};
