// Comet Tail — Infinite sustain reverb
// Ported from TheCometTail JUCE plugin (StompTones).
// Inline Schroeder reverb (4 comb + 2 allpass).
//
// CPU optimizations vs. original:
//   - Shimmer windows use quadrature oscillators (no cosf per sample in grain loop)
//   - Tone filter pow/exp cached (recompute only when tone knob changes)
//   - Texture LFO uses quadrature recurrence oscillator (no sin/cos per sample)
//
// Parameters:
//   sustain  0-1        — freeze / infinite sustain
//   shimmer  0-1        — octave pitch-shift blend (4-grain granular; 0=off)
//   decay    0.5-10     — tail length in seconds
//   texture  0-1        — chorus modulation depth
//   tone     0-100      — tonal character (50=neutral, <50=dark, >50=bright)
//   mix      0-1        — dry/wet
//
// Shimmer grain buffer (kGrainBufLen floats) must be supplied externally in SDRAM.
// Pass nullptr to Init() to disable shimmer entirely (backward-compatible).

#pragma once
#include <cmath>
#include "fast_math.h"

class CometTail
{
public:
    // SDRAM buffer sizes
    static constexpr int kChorusLen  = 1444;   // 30ms @ 48kHz
    static constexpr int kGrainBufLen = 19200; // 400ms @ 48kHz (shimmer; nullable)

    // Comb filter delay lengths (prime-ish, tuned for 48kHz)
    static constexpr int kCombL0 = 1557, kCombL1 = 1617, kCombL2 = 1491, kCombL3 = 1422;
    static constexpr int kAP0 = 225, kAP1 = 341;
    static constexpr int kCombTotal = kCombL0 + kCombL1 + kCombL2 + kCombL3;

    float sustain = 0.0f;
    float shimmer = 0.0f;
    float decay   = 5.0f;
    float texture = 0.0f;
    float mix     = 0.5f;
    float tone    = 50.0f;

    // externalCombL/R:   kCombTotal floats each (SDRAM)
    // externalChorusL/R: kChorusLen floats each (SDRAM)
    // externalGrainBuf:  kGrainBufLen floats (SDRAM); pass nullptr to disable shimmer
    void Init(float sampleRate,
              float* externalCombL,  float* externalCombR,
              float* externalChorusL, float* externalChorusR,
              float* externalGrainBuf = nullptr) noexcept
    {
        sr         = sampleRate;
        combBufL   = externalCombL;
        combBufR   = externalCombR;
        chorusBufL = externalChorusL;
        chorusBufR = externalChorusR;
        grainBuf   = externalGrainBuf;

        // Chorus: 7ms base, ±4ms depth
        chorusBase  = 7.0f * sampleRate / 1000.0f;
        chorusDepth = 4.0f * sampleRate / 1000.0f;

        // Tone + LFO caches
        prevTone = -999.0f;
        updateToneCache();
        updateLFO(0.0f);

        // Shimmer: 4-grain granular pitch shifter
        // L side: P5 − 2¢ (slightly flat for dreamy beating against R)
        // R side: Oct + 2¢ (slightly sharp)
        // Window freq per grain: pitchRatio / grainSize cycles/sample
        // — use quadrature oscillator (no cosf in inner loop)
        if (grainBuf)
        {
            const int grainSize = kGrainBufLen / 4;

            const float omegaL = 6.28318f * kPitchL / static_cast<float>(grainSize);
            winSinIncL = sinf(omegaL);
            winCosIncL = cosf(omegaL);

            const float omegaR = 6.28318f * kPitchR / static_cast<float>(grainSize);
            winSinIncR = sinf(omegaR);
            winCosIncR = cosf(omegaR);

            // Grains uniformly distributed; initial window phase = k * π/2
            for (int k = 0; k < 4; ++k)
            {
                grainPhaseL[k] = static_cast<float>(k * grainSize);
                grainPhaseR[k] = static_cast<float>(k * grainSize);

                const float initPh = static_cast<float>(k) * 1.57080f; // k * π/2
                winCosL[k] = cosf(initPh);
                winSinL[k] = sinf(initPh);
                winCosR[k] = winCosL[k];
                winSinR[k] = winSinL[k];

                grainSafetyL[k] = grainSafetyR[k] = 1.0f;
            }

            grainWritePos = 0;
            grainWriteLP  = 0.0f;
            shimLPL = shimLPR = 0.0f;

            // 10kHz anti-aliasing LP on grain writes
            grainAAAlpha = 1.0f - expf(-6.28318f * 10000.0f / sampleRate);
            // 4kHz soft LP on shimmer output (limits harmonic stacking)
            shimLPAlpha  = 1.0f - expf(-6.28318f * 4000.0f / sampleRate);
            // Safety zone: 5ms fade-in when grain read approaches write pos
            grainSafetyZone  = static_cast<int>(sampleRate * 0.005f);
            // Recovery TC = 10ms
            grainSafetyCoeff = expf(-1.0f / (0.010f * sampleRate));
        }

        Reset();
    }

    void Reset() noexcept
    {
        if (combBufL)  for (int i = 0; i < kCombTotal;  ++i) combBufL[i]  = 0.0f;
        if (combBufR)  for (int i = 0; i < kCombTotal;  ++i) combBufR[i]  = 0.0f;
        if (chorusBufL) for (int i = 0; i < kChorusLen; ++i) chorusBufL[i] = 0.0f;
        if (chorusBufR) for (int i = 0; i < kChorusLen; ++i) chorusBufR[i] = 0.0f;
        if (grainBuf)  for (int i = 0; i < kGrainBufLen; ++i) grainBuf[i]  = 0.0f;

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

        grainWriteLP = 0.0f;
        shimLPL = shimLPR = 0.0f;
    }

    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        if (!combBufL) { outL = inL; outR = inR; return; }

        const float dryL = inL, dryR = inR;

        const float maxFb = 0.72f + (decay - 0.5f) / 9.5f * 0.27f;
        const float fb    = fminf(0.99f, 0.50f + sustain * (maxFb - 0.50f));
        const float damp  = 0.2f;

        const float mono = (inL + inR) * 0.5f;

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

        float ap = (combSumL + combSumR) * 0.25f;
        ap = allpass(ap, apBuf0, apWp[0], kAP0);
        ap = allpass(ap, apBuf1, apWp[1], kAP1);
        float wetL = ap, wetR = ap;

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

        // Shimmer — 4-grain granular pitch shift, no transcendentals in inner loop
        // L grains read at P5-2¢ (1.4998×), R grains at Oct+2¢ (2.0002×)
        if (grainBuf)
        {
            // Write AA-filtered mono wet to grain buffer
            const float monoWet = (wetL + wetR) * 0.5f;
            grainWriteLP += grainAAAlpha * (monoWet - grainWriteLP);
            grainBuf[grainWritePos] = grainWriteLP;
            grainWritePos = (grainWritePos + 1) % kGrainBufLen;

            if (shimmer > 0.001f)
            {
                float accumL = 0.0f, wSumL = 0.0f;
                float accumR = 0.0f, wSumR = 0.0f;

                for (int k = 0; k < 4; ++k)
                {
                    // Advance grain phases
                    grainPhaseL[k] += kPitchL;
                    if (grainPhaseL[k] >= kGrainBufLen) grainPhaseL[k] -= kGrainBufLen;
                    grainPhaseR[k] += kPitchR;
                    if (grainPhaseR[k] >= kGrainBufLen) grainPhaseR[k] -= kGrainBufLen;

                    // Advance window oscillators: 4 mul + 2 add, no cosf
                    {
                        const float ns = winSinL[k] * winCosIncL + winCosL[k] * winSinIncL;
                        winCosL[k]     = winCosL[k] * winCosIncL - winSinL[k] * winSinIncL;
                        winSinL[k]     = ns;
                    }
                    {
                        const float ns = winSinR[k] * winCosIncR + winCosR[k] * winSinIncR;
                        winCosR[k]     = winCosR[k] * winCosIncR - winSinR[k] * winSinIncR;
                        winSinR[k]     = ns;
                    }

                    // Safety zone: fade grain when read pos approaches write pos
                    const int lagL = (grainWritePos
                                      - static_cast<int>(grainPhaseL[k])
                                      + kGrainBufLen) % kGrainBufLen;
                    const int lagR = (grainWritePos
                                      - static_cast<int>(grainPhaseR[k])
                                      + kGrainBufLen) % kGrainBufLen;

                    grainSafetyL[k] = (lagL < grainSafetyZone)
                        ? static_cast<float>(lagL) / static_cast<float>(grainSafetyZone)
                        : grainSafetyL[k] * grainSafetyCoeff + (1.0f - grainSafetyCoeff);

                    grainSafetyR[k] = (lagR < grainSafetyZone)
                        ? static_cast<float>(lagR) / static_cast<float>(grainSafetyZone)
                        : grainSafetyR[k] * grainSafetyCoeff + (1.0f - grainSafetyCoeff);

                    // Hann window from oscillator: 0.5 - 0.5*cos(phase)
                    const float winL = (0.5f - 0.5f * winCosL[k]) * grainSafetyL[k];
                    const float winR = (0.5f - 0.5f * winCosR[k]) * grainSafetyR[k];

                    // Interpolated read
                    {
                        const int   ia = static_cast<int>(grainPhaseL[k]) % kGrainBufLen;
                        const float fr = grainPhaseL[k] - static_cast<float>(static_cast<int>(grainPhaseL[k]));
                        const float g  = grainBuf[ia] * (1.0f - fr)
                                       + grainBuf[(ia + 1) % kGrainBufLen] * fr;
                        accumL += g * winL;
                        wSumL  += winL;
                    }
                    {
                        const int   ia = static_cast<int>(grainPhaseR[k]) % kGrainBufLen;
                        const float fr = grainPhaseR[k] - static_cast<float>(static_cast<int>(grainPhaseR[k]));
                        const float g  = grainBuf[ia] * (1.0f - fr)
                                       + grainBuf[(ia + 1) % kGrainBufLen] * fr;
                        accumR += g * winR;
                        wSumR  += winR;
                    }
                }

                // Normalize, apply output LP, add to wet
                const float shimRawL = (wSumL > 0.001f) ? (accumL / wSumL) * shimmer * 0.45f : 0.0f;
                const float shimRawR = (wSumR > 0.001f) ? (accumR / wSumR) * shimmer * 0.45f : 0.0f;
                shimLPL += shimLPAlpha * (shimRawL - shimLPL);
                shimLPR += shimLPAlpha * (shimRawR - shimLPR);

                wetL += shimLPL;
                wetR += shimLPR;
            }
        }

        // Texture chorus
        if (fabsf(texture - prevTexture) > 0.001f)
            updateLFO(texture);

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
    // Shimmer pitch ratios: P5−2¢ on L, Oct+2¢ on R — slight detuning creates slow dreamy beating
    static constexpr float kPitchL = 1.4998f;
    static constexpr float kPitchR = 2.0002f;

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

    void updateLFO(float tex) noexcept
    {
        prevTexture = tex;
        const float omega = 6.28318f * (0.3f + tex * 2.7f) / sr;
        lfoSinInc = sinf(omega);
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
    float* grainBuf   = nullptr;

    float chorusBase = 0.0f, chorusDepth = 0.0f;

    float lfoSin = 0.0f, lfoCos = 1.0f;
    float lfoSinInc = 0.0f, lfoCosInc = 1.0f;
    float prevTexture = -99.0f;

    float prevTone    = -999.0f;
    float cachedLpA   = 0.0f, cachedHpA   = 0.0f;
    float cachedLpMix = 0.0f, cachedHpMix = 1.0f;

    int   combWpL[4] = {}, combWpR[4] = {};
    float combLPL[4] = {}, combLPR[4] = {};
    int   apWp[2] = {};
    float apBuf0[kAP0] = {};
    float apBuf1[kAP1] = {};
    int   chorusWritePos = 0;
    float toneLPL = 0.0f, toneLPR = 0.0f;
    float toneHPxL = 0.0f, toneHPyL = 0.0f;
    float toneHPxR = 0.0f, toneHPyR = 0.0f;

    // Shimmer grain state
    int   grainWritePos = 0;
    float grainWriteLP  = 0.0f;
    float grainAAAlpha  = 0.0f;   // 10kHz AA LP on writes
    float shimLPAlpha   = 0.0f;   // 4kHz LP on shimmer output
    float shimLPL = 0.0f, shimLPR = 0.0f;
    int   grainSafetyZone  = 240; // 5ms at 48kHz
    float grainSafetyCoeff = 0.998f;

    float grainPhaseL[4] = {};
    float grainPhaseR[4] = {};
    float grainSafetyL[4] = { 1,1,1,1 };
    float grainSafetyR[4] = { 1,1,1,1 };

    // Window quadrature oscillators — one per grain per side, no cosf in inner loop
    float winSinL[4] = {}, winCosL[4] = { 1,0,-1,0 };
    float winSinR[4] = {}, winCosR[4] = { 1,0,-1,0 };
    float winSinIncL = 0.0f, winCosIncL = 1.0f;
    float winSinIncR = 0.0f, winCosIncR = 1.0f;
};
