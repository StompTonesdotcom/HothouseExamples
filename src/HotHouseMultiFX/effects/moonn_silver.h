// Moonn Silver — two-stage LM386 fuzz
// Ported from Moonn-Silver JUCE plugin (StompTones).
// Fixed params: unity output gain at full volume position.
// Runs at base sample rate (no oversampling — 48kHz is fine for a fuzz).
//
// GAIN CALIBRATION NOTE:
// The circuit clips any real guitar signal to ±posThresh (0.956) — it's a square-wave fuzz.
// kOutputGain is set so that this clipped output matches the bypass level for a typical
// guitar signal (~0.3 normalized). Tune kOutputGain on hardware if needed:
//   too loud → lower it, too quiet → raise it.
// Current value targets ~unity for a medium guitar signal.

#pragma once
#include <cmath>

class MoonnSilver
{
public:
    void Init(float sampleRate) noexcept
    {
        // All filters at base rate (no oversampling)
        // Interstage HP: C3 1µF + R2 68kΩ → fc ≈ 2.34 Hz
        interstageHP_a = std::exp(-6.28318f * 2.34f / sampleRate);
        // Interstage LP: R2 68kΩ + C4 4.7nF → fc ≈ 497 Hz
        interstageLP_a = std::exp(-6.28318f * 497.0f / sampleRate);
        // Output HP: C5 1µF + R_load 100kΩ → fc ≈ 1.59 Hz
        outputHP_a     = std::exp(-6.28318f * 1.59f / sampleRate);
        Reset();
    }

    void Reset() noexcept
    {
        ihpX = ihpY = ilpY = ohpX = ohpY = 0.0f;
    }

    float Process(float in) noexcept
    {
        float x = in * kInputScale;

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

        // kOutputGain: output saturates to ~±0.956 regardless of input level.
        // Set so engaged level ≈ bypass level. Tune on hardware if needed.
        return oh * kOutputGain;
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

    float interstageHP_a = 0.0f;
    float interstageLP_a = 0.0f;
    float outputHP_a     = 0.0f;
    float ihpX = 0.0f, ihpY = 0.0f;
    float ilpY = 0.0f;
    float ohpX = 0.0f, ohpY = 0.0f;

    // kInputScale: maps normalized ±1 → supply rail scale (1V / 4.5V half-supply ≈ 0.222)
    static constexpr float kInputScale = 0.222f;
    // kOutputGain: clip output is ~±0.956, target bypass match at ~0.3 normalized input.
    // At full hardware volume (pot at max): original applies audioTaperGain(1.0)×kOutputTrim(0.5).
    // For unity at max vol: 0.956 × 1.0 × 0.5 = 0.478. Adjust down slightly for headroom.
    // TUNE ON HARDWARE: start here, raise/lower until bypass ≈ engaged level.
    static constexpr float kOutputGain = 0.22f;  // Halved after hardware test — square wave RMS is ~2x louder than sine at same peak
};
