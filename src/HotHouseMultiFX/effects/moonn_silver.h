// Moonn Silver — two-stage LM386 fuzz
// Ported from Moonn-Silver JUCE plugin (StompTones).
// Fixed params: unity output gain at full volume position.
// Runs at base sample rate (no oversampling — 48kHz is fine for a fuzz).
//
// Parameters exposed for runtime control:
//   gain  — pre-gain multiplier (0.5=soft, 4.0=heavy saturation)
//   tone  — post-fuzz LP cutoff: 0=dark (~500Hz), 1=open (~8kHz)
//           Not in original plugin — added for hardware usability.
//
// Original plugin is a mono circuit; both output channels carry the same signal.

#pragma once
#include <cmath>

class MoonnSilver
{
public:
    float gain = 1.0f;  // Pre-gain multiplier (0.5=soft, 4.0=heavy saturation)
    float tone = 1.0f;  // Post-fuzz LP tone: 0=dark (~500Hz), 1=open (~8kHz)

    void Init(float sampleRate) noexcept
    {
        sr = sampleRate;

        // All filters at base rate (no oversampling)
        // Interstage HP: C3 1µF + R2 68kΩ → fc ≈ 2.34 Hz
        interstageHP_a = std::exp(-6.28318f * 2.34f / sr);
        // Interstage LP: R2 68kΩ + C4 4.7nF → fc ≈ 497 Hz
        interstageLP_a = std::exp(-6.28318f * 497.0f / sr);
        // Output HP: C5 1µF + R_load 100kΩ → fc ≈ 1.59 Hz
        outputHP_a = std::exp(-6.28318f * 1.59f / sr);

        toneAlpha = 1.0f;  // initial: wide open (tone=1)
        prevTone  = tone;

        Reset();
    }

    void Reset() noexcept
    {
        ihpX = ihpY = ilpY = ohpX = ohpY = 0.0f;
        toneSL = toneSR = 0.0f;
    }

    // Stereo API — original is a mono circuit, both outputs carry the same processed signal.
    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        if (tone != prevTone)
        {
            const float fc = 500.0f + tone * (8000.0f - 500.0f);
            toneAlpha = 1.0f - expf(-6.28318f * fc / sr);
            prevTone = tone;
        }

        // Mono circuit: use left input, mirror to right
        const float mono = inL * gain * kInputScale;

        float x = mono;

        // Stage 1: 200× gain + asymmetric rail clip
        x = lm386Stage(x);

        // Interstage HP (DC block between stages)
        float hp = x - ihpX + interstageHP_a * ihpY;
        ihpX = x; ihpY = hp;

        // Interstage LP ~497 Hz (the defining Model T tonal character)
        float lp = ilpY + interstageLP_a * (hp - ilpY);
        ilpY = lp;

        // Stage 2: 200× gain + asymmetric rail clip
        lp = lm386Stage(lp);

        // Output HP (DC block)
        float oh = lp - ohpX + outputHP_a * ohpY;
        ohpX = lp; ohpY = oh;

        float out = oh * kOutputGain;

        // Post-fuzz tone LP (L and R tracked separately for phase coherence)
        toneSL += toneAlpha * (out - toneSL);
        toneSR += toneAlpha * (out - toneSR);
        outL = toneSL;
        outR = toneSR;
    }

private:
    static float lm386Stage(float x) noexcept
    {
        x *= 200.0f;
        constexpr float posThresh = 0.956f, negThresh = 0.922f;
        constexpr float posKnee   = posThresh * 0.80f;
        constexpr float negKnee   = negThresh * 0.80f;

        if (x >= posThresh) return posThresh;
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

    float sr = 48000.0f;
    float interstageHP_a = 0.0f;
    float interstageLP_a = 0.0f;
    float outputHP_a     = 0.0f;
    float ihpX = 0.0f, ihpY = 0.0f;
    float ilpY = 0.0f;
    float ohpX = 0.0f, ohpY = 0.0f;
    float toneAlpha = 1.0f;
    float prevTone  = -1.0f;
    float toneSL = 0.0f, toneSR = 0.0f;

    // kInputScale: maps normalized ±1 → supply rail scale (1V / 4.5V half-supply ≈ 0.222)
    static constexpr float kInputScale = 0.222f;
    // kOutputGain: halved after hardware test — square wave RMS is ~2x louder than sine at same peak
    static constexpr float kOutputGain = 0.22f;
};
