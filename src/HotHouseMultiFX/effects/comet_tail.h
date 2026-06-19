// Comet Tail — Infinite sustain reverb
// Ported from TheCometTail JUCE plugin (StompTones).
// Freeverb core: 8-comb + 4-allpass per channel (standard 48kHz lengths).
//
// Matches juce::dsp::Reverb internals:
//   - Stereo: separate L/R comb lengths (+26 samples spread for R)
//   - Input scaled by fixedGain = 0.015f (Freeverb normalization)
//   - roomSize → combFb via Freeverb formula: fb = roomSize*0.28 + 0.70
//   - Allpass: JUCE variant — buf[wp] = x + g*buf[wp]; return buf[wp-N] - g*x
//   - Separate 4-allpass chains for L and R (true stereo diffusion)
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
    static constexpr int kChorusLen  = 1444;    // 30ms @ 48kHz
    static constexpr int kGrainBufLen = 19200;  // 400ms @ 48kHz (shimmer; nullable)

    static constexpr int kCombTotalL   = 11998;
    static constexpr int kCombTotalR   = 12200;
    static constexpr int kCombBufMax   = kCombTotalR;  // allocate this for both external buffers

    // Freeverb allpass lengths at 48kHz — L and R differ by ~25-sample stereo spread
    static constexpr int kAPL0 = 605, kAPL1 = 480, kAPL2 = 371, kAPL3 = 245;
    static constexpr int kAPR0 = 630, kAPR1 = 505, kAPR2 = 396, kAPR3 = 270;

    float sustain = 0.0f;
    float shimmer = 0.0f;
    float decay   = 5.0f;
    float texture = 0.0f;
    float mix     = 0.5f;
    float tone    = 50.0f;

    // externalCombL/R:   kCombBufMax floats each (SDRAM)
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
        if (grainBuf)
        {
            const int grainSize = kGrainBufLen / 4;

            const float omegaL = 6.28318f * kPitchL / static_cast<float>(grainSize);
            winSinIncL = sinf(omegaL);
            winCosIncL = cosf(omegaL);

            const float omegaR = 6.28318f * kPitchR / static_cast<float>(grainSize);
            winSinIncR = sinf(omegaR);
            winCosIncR = cosf(omegaR);

            for (int k = 0; k < 4; ++k)
            {
                grainPhaseL[k] = static_cast<float>(k * grainSize);
                grainPhaseR[k] = static_cast<float>(k * grainSize + grainSize / 2);

                const float initPh = static_cast<float>(k) * 1.57080f;
                winCosL[k] = cosf(initPh);
                winSinL[k] = sinf(initPh);
                winCosR[k] = winCosL[k];
                winSinR[k] = winSinL[k];

                grainSafetyL[k] = grainSafetyR[k] = 1.0f;
            }

            grainWritePos = 0;
            grainWriteLP  = 0.0f;
            shimLPL = shimLPR = 0.0f;

            grainAAAlpha = 1.0f - expf(-6.28318f * 10000.0f / sampleRate);
            shimLPAlpha  = 1.0f - expf(-6.28318f * 4000.0f  / sampleRate);
            grainSafetyZone  = static_cast<int>(sampleRate * 0.005f);
            grainSafetyCoeff = expf(-1.0f / (0.010f * sampleRate));
        }

        Reset();
    }

    void Reset() noexcept
    {
        if (combBufL)  for (int i = 0; i < kCombTotalL; ++i) combBufL[i]  = 0.0f;
        if (combBufR)  for (int i = 0; i < kCombTotalR; ++i) combBufR[i]  = 0.0f;
        if (chorusBufL) for (int i = 0; i < kChorusLen; ++i) chorusBufL[i] = 0.0f;
        if (chorusBufR) for (int i = 0; i < kChorusLen; ++i) chorusBufR[i] = 0.0f;
        if (grainBuf)  for (int i = 0; i < kGrainBufLen; ++i) grainBuf[i]  = 0.0f;

        for (int i = 0; i < 8; ++i)
            combWpL[i] = combWpR[i] = 0, combLPL[i] = combLPR[i] = 0.0f;
        for (int i = 0; i < 4; ++i) apWpL[i] = apWpR[i] = 0;
        for (int i = 0; i < kAPL0; ++i) apBufL0[i] = 0.0f;
        for (int i = 0; i < kAPL1; ++i) apBufL1[i] = 0.0f;
        for (int i = 0; i < kAPL2; ++i) apBufL2[i] = 0.0f;
        for (int i = 0; i < kAPL3; ++i) apBufL3[i] = 0.0f;
        for (int i = 0; i < kAPR0; ++i) apBufR0[i] = 0.0f;
        for (int i = 0; i < kAPR1; ++i) apBufR1[i] = 0.0f;
        for (int i = 0; i < kAPR2; ++i) apBufR2[i] = 0.0f;
        for (int i = 0; i < kAPR3; ++i) apBufR3[i] = 0.0f;

        chorusWritePos = 0;
        lfoSin = 0.0f; lfoCos = 1.0f;
        prevTexture = -99.0f;
        svfLpS1L = svfLpS2L = svfLpS1R = svfLpS2R = 0.0f;
        svfHpS1L = svfHpS2L = svfHpS1R = svfHpS2R = 0.0f;

        grainWriteLP = 0.0f;
        shimLPL = shimLPR = 0.0f;
    }

    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        if (!combBufL) { outL = inL; outR = inR; return; }

        const float dryL = inL, dryR = inR;

        // Freeverb roomSize → combFb mapping (matches juce::dsp::Reverb internals)
        const float maxRoomSize = 0.72f + (decay - 0.5f) / 9.5f * 0.27f;
        const float roomSize    = fminf(0.99f, 0.50f + sustain * (maxRoomSize - 0.50f));
        const float combFb      = roomSize * 0.28f + 0.70f;
        // Freeverb damping: juce::Reverb applies dampScaleFactor=0.4 to the user
        // parameter before using it as the LP coefficient (confirmed in juce_Reverb.h).
        // CometTail sets reverbParams.damping=0.35, so actual LP coefficient = 0.35*0.4 = 0.14.
        // Using 0.35 directly (old value) produced a reverb ~2.5x more muffled than the plugin.
        static constexpr float damp = 0.14f;

        // Freeverb fixedGain: prevents comb build-up from clipping at max feedback
        const float scaledMono = (inL + inR) * 0.5f * 0.015f;

        // Freeverb 8-comb lengths at 48kHz (scaled from 44.1kHz originals)
        static const int kLensL[8]  = {1214,1293,1390,1476,1548,1622,1695,1760};
        static const int kLensR[8]  = {1240,1318,1415,1501,1573,1648,1720,1785};
        static const int kOffsL[8]  = {0,1214,2507,3897,5373,6921,8543,10238};
        static const int kOffsR[8]  = {0,1240,2558,3973,5474,7047,8695,10415};

        // 8 parallel comb filters per channel (different lengths = stereo width)
        float combSumL = 0.0f, combSumR = 0.0f;
        for (int c = 0; c < 8; ++c)
        {
            {
                float* buf = combBufL + kOffsL[c];
                int&   wp  = combWpL[c];
                float& lp  = combLPL[c];
                const float o = buf[wp];
                lp = lp * damp + o * (1.0f - damp);
                buf[wp] = scaledMono + lp * combFb;
                wp = (wp + 1 < kLensL[c]) ? wp + 1 : 0;
                combSumL += o;
            }
            {
                float* buf = combBufR + kOffsR[c];
                int&   wp  = combWpR[c];
                float& lp  = combLPR[c];
                const float o = buf[wp];
                lp = lp * damp + o * (1.0f - damp);
                buf[wp] = scaledMono + lp * combFb;
                wp = (wp + 1 < kLensR[c]) ? wp + 1 : 0;
                combSumR += o;
            }
        }

        // 4 allpass diffusors per channel — separate L/R for true stereo
        float wetL = allpass(combSumL, apBufL0, apWpL[0], kAPL0);
        wetL = allpass(wetL, apBufL1, apWpL[1], kAPL1);
        wetL = allpass(wetL, apBufL2, apWpL[2], kAPL2);
        wetL = allpass(wetL, apBufL3, apWpL[3], kAPL3);

        float wetR = allpass(combSumR, apBufR0, apWpR[0], kAPR0);
        wetR = allpass(wetR, apBufR1, apWpR[1], kAPR1);
        wetR = allpass(wetR, apBufR2, apWpR[2], kAPR2);
        wetR = allpass(wetR, apBufR3, apWpR[3], kAPR3);

        if (fabsf(tone - prevTone) > 0.5f)
            updateToneCache();

        // Tone: 2nd-order Butterworth TPT SVF (LP and HP in parallel, crossfaded at noon)
        // Matches juce::dsp::StateVariableTPTFilter used in the plugin.
        float lpOutL, lpOutR, hpOutL, hpOutR;
        {
            const float g = cachedLpG, h = cachedLpH;
            float yH, yB, yL;
            yH = h * (wetL - (kSvfK + g) * svfLpS1L - svfLpS2L);
            yB = g * yH + svfLpS1L; svfLpS1L = g * yH + yB;
            yL = g * yB + svfLpS2L; svfLpS2L = g * yB + yL;
            lpOutL = yL;
            yH = h * (wetR - (kSvfK + g) * svfLpS1R - svfLpS2R);
            yB = g * yH + svfLpS1R; svfLpS1R = g * yH + yB;
            yL = g * yB + svfLpS2R; svfLpS2R = g * yB + yL;
            lpOutR = yL;
        }
        {
            const float g = cachedHpG, h = cachedHpH;
            float yH, yB, yL;
            yH = h * (wetL - (kSvfK + g) * svfHpS1L - svfHpS2L);
            yB = g * yH + svfHpS1L; svfHpS1L = g * yH + yB;
            yL = g * yB + svfHpS2L; svfHpS2L = g * yB + yL;
            hpOutL = yH;
            yH = h * (wetR - (kSvfK + g) * svfHpS1R - svfHpS2R);
            yB = g * yH + svfHpS1R; svfHpS1R = g * yH + yB;
            yL = g * yB + svfHpS2R; svfHpS2R = g * yB + yL;
            hpOutR = yH;
        }
        wetL = cachedLpMix * lpOutL + cachedHpMix * hpOutL;
        wetR = cachedLpMix * lpOutR + cachedHpMix * hpOutR;

        // Shimmer — 4-grain granular pitch shift, no transcendentals in inner loop
        if (grainBuf)
        {
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
                    grainPhaseL[k] += kPitchL;
                    if (grainPhaseL[k] >= kGrainBufLen) grainPhaseL[k] -= kGrainBufLen;
                    grainPhaseR[k] += kPitchR;
                    if (grainPhaseR[k] >= kGrainBufLen) grainPhaseR[k] -= kGrainBufLen;

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

                    const float winL = (0.5f - 0.5f * winCosL[k]) * grainSafetyL[k];
                    const float winR = (0.5f - 0.5f * winCosR[k]) * grainSafetyR[k];

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
    static constexpr float kPitchL = 1.4998f;
    static constexpr float kPitchR = 2.0002f;

    void updateToneCache() noexcept
    {
        prevTone = tone;
        const float lpFreq = 2000.0f * std::pow(15000.0f / 2000.0f, fminf(tone, 50.0f) / 50.0f);
        const float hpFreq = 80.0f   * std::pow(500.0f   / 80.0f,
                                                 fmaxf(tone - 50.0f, 0.0f) / 50.0f);
        cachedLpG = std::tan(3.14159265f * lpFreq / sr);
        cachedLpH = 1.0f / (1.0f + kSvfK * cachedLpG + cachedLpG * cachedLpG);
        cachedHpG = std::tan(3.14159265f * hpFreq / sr);
        cachedHpH = 1.0f / (1.0f + kSvfK * cachedHpG + cachedHpG * cachedHpG);
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

    // JUCE Reverb allpass (confirmed in juce_Reverb.h line 299-306):
    //   buf[wp] = input + buf[wp] * 0.5f;  return buf[wp-N] - input
    // The 0.5 factor is only in the write; the return value is bufOut - input (no scaling).
    static float allpass(float in, float* buf, int& wp, int len) noexcept
    {
        const float bufOut = buf[wp];
        buf[wp] = in + bufOut * 0.5f;
        wp = (wp + 1 < len) ? wp + 1 : 0;
        return bufOut - in;
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
    // SVF coefficients (2nd-order Butterworth TPT, precomputed on tone change)
    static constexpr float kSvfK = 1.41421356f; // sqrt(2), Butterworth Q=1/sqrt(2)
    float cachedLpG = 0.0f, cachedLpH = 1.0f;
    float cachedHpG = 0.0f, cachedHpH = 1.0f;
    float cachedLpMix = 0.0f, cachedHpMix = 1.0f;

    int   combWpL[8] = {}, combWpR[8] = {};
    float combLPL[8] = {}, combLPR[8] = {};
    int   apWpL[4] = {}, apWpR[4] = {};

    // Allpass delay buffers — separate L/R, sized exactly to their lengths (SRAM)
    float apBufL0[kAPL0] = {}, apBufL1[kAPL1] = {}, apBufL2[kAPL2] = {}, apBufL3[kAPL3] = {};
    float apBufR0[kAPR0] = {}, apBufR1[kAPR1] = {}, apBufR2[kAPR2] = {}, apBufR3[kAPR3] = {};

    int   chorusWritePos = 0;
    // SVF integrator states: 2 per filter per channel
    float svfLpS1L = 0.0f, svfLpS2L = 0.0f;
    float svfLpS1R = 0.0f, svfLpS2R = 0.0f;
    float svfHpS1L = 0.0f, svfHpS2L = 0.0f;
    float svfHpS1R = 0.0f, svfHpS2R = 0.0f;

    // Shimmer grain state
    int   grainWritePos = 0;
    float grainWriteLP  = 0.0f;
    float grainAAAlpha  = 0.0f;
    float shimLPAlpha   = 0.0f;
    float shimLPL = 0.0f, shimLPR = 0.0f;
    int   grainSafetyZone  = 240;
    float grainSafetyCoeff = 0.998f;

    float grainPhaseL[4] = {};
    float grainPhaseR[4] = {};
    float grainSafetyL[4] = { 1,1,1,1 };
    float grainSafetyR[4] = { 1,1,1,1 };

    float winSinL[4] = {}, winCosL[4] = { 1,0,-1,0 };
    float winSinR[4] = {}, winCosR[4] = { 1,0,-1,0 };
    float winSinIncL = 0.0f, winCosIncL = 1.0f;
    float winSinIncR = 0.0f, winCosIncR = 1.0f;
};
