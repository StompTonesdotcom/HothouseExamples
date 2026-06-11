// Moonn Silver — two-stage LM386 fuzz, 4x oversampled
// Ported from Moonn-Silver JUCE plugin (StompTones).
// Fixed params: unity output gain at full volume position.
//
// All filter and clip stages run at 192 kHz (4x) to match JUCE plugin's 8x OS
// character as closely as the Cortex-M7 budget allows.  Tone LP filter runs
// at base 48 kHz after downsampling.
//
// Parameters:
//   gain  — pre-gain multiplier (0.5=soft, 4.0=heavy saturation)
//   tone  — post-fuzz LP cutoff: 0=dark (~500Hz), 1=open (~8kHz)
//
// Original plugin is mono; both output channels carry the same signal.

#pragma once
#include <cmath>
#include "os2.h"

class MoonnSilver
{
public:
    float gain = 1.0f;
    float tone = 1.0f;

    void Init(float sampleRate) noexcept
    {
        sr = sampleRate;
        const float sr4 = 4.0f * sr;  // 4x oversampled rate (192 kHz)

        // Main chain filters computed at 4x rate
        interstageHP_a = std::exp(-6.28318f * 2.34f  / sr4);
        interstageLP_a = std::exp(-6.28318f * 497.0f / sr4);
        outputHP_a     = std::exp(-6.28318f * 1.59f  / sr4);

        toneAlpha = 1.0f;
        prevTone  = -1.0f;

        Reset();
    }

    void Reset() noexcept
    {
        ihpX = ihpY = ilpY = ohpX = ohpY = 0.0f;
        toneSL = toneSR = 0.0f;
        os.reset();
    }

    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        if (tone != prevTone)
        {
            const float fc = 500.0f + tone * (8000.0f - 500.0f);
            toneAlpha = 1.0f - expf(-6.28318f * fc / sr);
            prevTone = tone;
        }

        const float monoIn = inL * gain * kInputScale;

        // 4x upsample
        float u0, u1, u2, u3;
        os.up(monoIn, u0, u1, u2, u3);

        // Run full clip chain at 4x rate for all four upsampled samples
        float d0 = chainStep(u0);
        float d1 = chainStep(u1);
        float d2 = chainStep(u2);
        float d3 = chainStep(u3);

        // 4x downsample then output trim
        float out = os.dn(d0, d1, d2, d3) * kOutputGain;

        // Tone LP at base rate (same for L and R — mono circuit)
        toneSL += toneAlpha * (out - toneSL);
        toneSR += toneAlpha * (out - toneSR);
        outL = toneSL;
        outR = toneSR;
    }

private:
    // Full LM386 signal chain — called four times per base-rate sample (at 4x rate)
    float chainStep(float x) noexcept
    {
        // Stage 1: 200× gain + asymmetric soft-knee clip
        x = lm386Stage(x);

        // Interstage HP ~2.34 Hz (DC block)
        const float hp = x - ihpX + interstageHP_a * ihpY;
        ihpX = x; ihpY = hp;

        // Interstage LP ~497 Hz (Model T tonal character)
        const float lp = ilpY + interstageLP_a * (hp - ilpY);
        ilpY = lp;

        // Stage 2: 200× gain + asymmetric soft-knee clip
        float s2 = lm386Stage(lp);

        // Output HP ~1.59 Hz (DC block)
        const float oh = s2 - ohpX + outputHP_a * ohpY;
        ohpX = s2; ohpY = oh;

        return oh;
    }

    static float lm386Stage(float x) noexcept
    {
        x *= 200.0f;
        constexpr float posThresh = 0.956f, negThresh = 0.922f;
        constexpr float posKnee   = posThresh * 0.80f;
        constexpr float negKnee   = negThresh * 0.80f;

        if (x >= posThresh)  return posThresh;
        if (x > posKnee) {
            float t = (x - posKnee) / (posThresh - posKnee);
            return posKnee + (posThresh - posKnee) * t * (1.0f + t * (1.0f - t));
        }
        if (x <= -negThresh) return -negThresh;
        if (x < -negKnee) {
            float t = (-x - negKnee) / (negThresh - negKnee);
            return -(negKnee + (negThresh - negKnee) * t * (1.0f + t * (1.0f - t)));
        }
        return x;
    }

    // ── State ──────────────────────────────────────────────────────────────
    float sr = 48000.0f;

    // Chain filter state (evolves at 4x rate)
    float interstageHP_a = 0.0f;
    float interstageLP_a = 0.0f;
    float outputHP_a     = 0.0f;
    float ihpX = 0.0f, ihpY = 0.0f;
    float ilpY = 0.0f;
    float ohpX = 0.0f, ohpY = 0.0f;

    // Tone filter state (base rate)
    float toneAlpha = 1.0f;
    float prevTone  = -1.0f;
    float toneSL    = 0.0f, toneSR = 0.0f;

    // 4x oversampler
    OS4 os;

    // kInputScale: normalized ±1 → supply rail (1V / 4.5V ≈ 0.222)
    static constexpr float kInputScale = 0.222f;
    // kOutputGain: hardware-verified trim for unity gain out of fuzz
    static constexpr float kOutputGain = 0.22f;
};
