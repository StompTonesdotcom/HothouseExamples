// Comet Tail — Infinite sustain reverb
// Ported from TheCometTail JUCE plugin (StompTones).
// Inline Schroeder reverb (4 comb + 2 allpass).
//
// CPU optimizations vs. original:
//   - Shimmer pitch-shifter removed (was 4 grain loops + fmod + cos per sample)
//   - Tone filter pow/exp cached (recompute only when tone knob changes)
//   - Texture LFO uses quadrature recurrence oscillator (no sin/cos per sample)
//
// Parameters:
//   sustain  0-1        — freeze / infinite sustain
//   decay    0.5-10     — tail length in seconds
//   texture  0-1        — chorus modulation depth
//   tone     0-100      — tonal character (50=neutral, <50=dark, >50=bright)
//   mix      0-1        — dry/wet

#pragma once
#include <cmath>
#include "fast_math.h"

class CometTail
{
public:
    // Chorus buffer in SDRAM (caller provides)
    static constexpr int kChorusLen = 1444;  // 30ms @ 48kHz

    // Comb filter delay lengths (prime-ish, tuned for 48kHz)
    static constexpr int kCombL0 = 1557, kCombL1 = 1617, kCombL2 = 1491, kCombL3 = 1422;
    // Allpass delay lengths (in SRAM — small)
    static constexpr int kAP0 = 225, kAP1 = 341;
    // Total comb SDRAM: (kCombTotal) * 2 channels * 4 bytes ≈ 50KB
    static constexpr int kCombTotal = kCombL0 + kCombL1 + kCombL2 + kCombL3;

    float sustain = 0.0f;
    float decay   = 5.0f;
    float texture = 0.0f;
    float mix     = 0.5f;
    float tone    = 50.0f;

    // externalCombL/R: kCombTotal floats each (SDRAM)
    // externalChorusL/R: kChorusLen floats each (SDRAM)
    void Init(float sampleRate,
              float* externalCombL, float* externalCombR,
              float* externalChorusL, float* externalChorusR) noexcept
    {
        sr         = sampleRate;
        combBufL   = externalCombL;
        combBufR   = externalCombR;
        chorusBufL = externalChorusL;
        chorusBufR = externalChorusR;

        // Chorus geometry: 7ms base, ±4ms depth
        chorusBase  = 7.0f * sampleRate / 1000.0f;
        chorusDepth = 4.0f * sampleRate / 1000.0f;

        // Prime tone cache and LFO
        prevTone = -999.0f;
        updateToneCache();
        updateLFO(0.0f);

        Reset();
    }

    void Reset() noexcept
    {
        if (combBufL)  for (int i = 0; i < kCombTotal;  ++i) combBufL[i]  = 0.0f;
        if (combBufR)  for (int i = 0; i < kCombTotal;  ++i) combBufR[i]  = 0.0f;
        if (chorusBufL) for (int i = 0; i < kChorusLen;  ++i) chorusBufL[i] = 0.0f;
        if (chorusBufR) for (int i = 0; i < kChorusLen;  ++i) chorusBufR[i] = 0.0f;

        for (int i = 0; i < 4; ++i)
            combWpL[i] = combWpR[i] = 0, combLPL[i] = combLPR[i] = 0.0f;
        apWp[0] = apWp[1] = 0;
        for (int i = 0; i < kAP0; ++i) apBuf0[i] = 0.0f;
        for (int i = 0; i < kAP1; ++i) apBuf1[i] = 0.0f;

        chorusWritePos = 0;
        lfoSin = 0.0f; lfoCos = 1.0f;
        prevTexture = -99.0f;
        toneLPL = toneLPR = 0.0f;
        toneHPxL = toneHPyL = toneHPxR = toneHPyR = 0.0f;
    }

    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        if (!combBufL) { outL = inL; outR = inR; return; }

        const float dryL = inL, dryR = inR;

        // Map sustain to comb feedback (higher = longer tail / freeze)
        const float maxFb = 0.72f + (decay - 0.5f) / 9.5f * 0.27f; // 0.72–0.99
        const float fb    = fminf(0.99f, 0.50f + sustain * (maxFb - 0.50f));
        const float damp  = 0.2f;  // LP damping inside combs

        // Mono sum into reverb input
        const float mono = (inL + inR) * 0.5f;

        // 4 parallel combs (L and R share lengths, decorrelated by buffer)
        static constexpr int kLens[4] = { kCombL0, kCombL1, kCombL2, kCombL3 };
        static constexpr int kOffs[4] = { 0, kCombL0, kCombL0+kCombL1,
                                          kCombL0+kCombL1+kCombL2 };
        float combSumL = 0.0f, combSumR = 0.0f;
        for (int c = 0; c < 4; ++c)
        {
            const int len = kLens[c];
            const int off = kOffs[c];
            {
                float* buf = combBufL + off;
                int&   wp  = combWpL[c];
                float& lp  = combLPL[c];
                const float o = buf[wp];
                lp = lp * damp + o * (1.0f - damp);
                buf[wp] = mono + lp * fb;
                wp = (wp + 1 < len) ? wp + 1 : 0;
                combSumL += o;
            }
            {
                float* buf = combBufR + off;
                int&   wp  = combWpR[c];
                float& lp  = combLPR[c];
                const float o = buf[wp];
                lp = lp * damp + o * (1.0f - damp);
                buf[wp] = mono + lp * fb;
                wp = (wp + 1 < len) ? wp + 1 : 0;
                combSumR += o;
            }
        }

        // 2 series allpass (mono, then split to stereo)
        float ap = (combSumL + combSumR) * 0.25f;
        ap = allpass(ap, apBuf0, apWp[0], kAP0);
        ap = allpass(ap, apBuf1, apWp[1], kAP1);
        float wetL = ap, wetR = ap;

        // Tone filter — coefficients recomputed only when tone changes by >0.5
        if (fabsf(tone - prevTone) > 0.5f)
            updateToneCache();

        toneLPL += (1.0f - cachedLpA) * (wetL - toneLPL);
        toneLPR += (1.0f - cachedLpA) * (wetR - toneLPR);
        const float hpL = wetL - toneHPxL + cachedHpA * toneHPyL;
        toneHPxL = wetL; toneHPyL = hpL;
        const float hpR = wetR - toneHPxR + cachedHpA * toneHPyR;
        toneHPxR = wetR; toneHPyR = hpR;
        wetL = cachedLpMix * toneLPL + cachedHpMix * hpL;
        wetR = cachedLpMix * toneLPR + cachedHpMix * hpR;

        // Texture chorus — quadrature oscillator (no sin/cos per sample)
        // LFO increments recomputed only when texture changes
        if (fabsf(texture - prevTexture) > 0.001f)
            updateLFO(texture);

        // Rotate sin/cos by one step — costs 4 mul + 2 add, no transcendentals
        const float ns = lfoSin * lfoCosInc + lfoCos * lfoSinInc;
        lfoCos = lfoCos * lfoCosInc - lfoSin * lfoSinInc;
        lfoSin = ns;

        if (chorusBufL)
        {
            chorusBufL[chorusWritePos] = wetL;
            chorusBufR[chorusWritePos] = wetR;
            chorusWritePos = (chorusWritePos + 1) % kChorusLen;
            auto readDelay = [&](float* b, float d) noexcept {
                float rp = static_cast<float>(chorusWritePos) - d;
                if (rp < 0.0f) rp += static_cast<float>(kChorusLen);
                const int ri = static_cast<int>(rp) % kChorusLen;
                const float f = rp - static_cast<float>(ri);
                return b[ri] * (1.0f - f) + b[(ri + 1) % kChorusLen] * f;
            };
            const float cMix = texture * 0.4f;
            const float dL = chorusBase + lfoSin * chorusDepth * texture;
            const float dR = chorusBase + lfoCos * chorusDepth * texture;
            wetL = wetL * (1.0f - cMix) + readDelay(chorusBufL, dL) * cMix;
            wetR = wetR * (1.0f - cMix) + readDelay(chorusBufR, dR) * cMix;
        }

        outL = dryL * (1.0f - mix) + wetL * mix;
        outR = dryR * (1.0f - mix) + wetR * mix;
    }

private:
    // Recompute tone filter coefficients (called at control rate, not per sample)
    void updateToneCache() noexcept
    {
        prevTone = tone;
        const float lpFreq = 2000.0f * std::pow(15000.0f / 2000.0f, fminf(tone, 50.0f) / 50.0f);
        const float hpFreq = 80.0f   * std::pow(500.0f   / 80.0f,
                                                 fmaxf(tone - 50.0f, 0.0f) / 50.0f);
        cachedLpA   = std::exp(-6.28318f * lpFreq / sr);
        cachedHpA   = std::exp(-6.28318f * hpFreq / sr);
        cachedLpMix = fmaxf(0.0f, fminf(1.0f, (55.0f - tone) / 10.0f));
        cachedHpMix = 1.0f - cachedLpMix;
    }

    // Recompute LFO quadrature increments (called when texture changes)
    void updateLFO(float tex) noexcept
    {
        prevTexture = tex;
        const float omega = 6.28318f * (0.3f + tex * 2.7f) / sr;
        lfoSinInc = sinf(omega);   // exact, called once per control change
        lfoCosInc = cosf(omega);
    }

    static float allpass(float in, float* buf, int& wp, int len) noexcept
    {
        const float bufOut = buf[wp];
        const float v      = in + bufOut * (-0.5f);
        buf[wp] = in + bufOut * 0.5f;
        wp = (wp + 1 < len) ? wp + 1 : 0;
        return v;
    }

    float sr = 48000.0f;
    float* combBufL   = nullptr;
    float* combBufR   = nullptr;
    float* chorusBufL = nullptr;
    float* chorusBufR = nullptr;

    float chorusBase = 0.0f, chorusDepth = 0.0f;

    // LFO quadrature oscillator state
    float lfoSin = 0.0f, lfoCos = 1.0f;
    float lfoSinInc = 0.0f, lfoCosInc = 1.0f;
    float prevTexture = -99.0f;

    // Tone filter coefficient cache
    float prevTone    = -999.0f;
    float cachedLpA   = 0.0f, cachedHpA   = 0.0f;
    float cachedLpMix = 0.0f, cachedHpMix = 1.0f;

    // Comb state
    int   combWpL[4] = {}, combWpR[4] = {};
    float combLPL[4] = {}, combLPR[4] = {};

    // Allpass state (in SRAM — small enough)
    int   apWp[2] = {};
    float apBuf0[kAP0] = {};
    float apBuf1[kAP1] = {};

    int   chorusWritePos = 0;
    float toneLPL = 0.0f, toneLPR = 0.0f;
    float toneHPxL = 0.0f, toneHPyL = 0.0f;
    float toneHPxR = 0.0f, toneHPyR = 0.0f;
};
