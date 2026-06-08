// Faze 9 — MXR Phase 90 / M101 phaser model
// Ported from Faze-9 JUCE plugin (StompTones).
// Fixed at 11 o'clock position: speed param ≈ 0.40 normalized.
// 4 all-pass stages with R28 feedback (M101 block-logo topology).

#pragma once
#include <cmath>

class Faze9
{
public:
    void Init(float sampleRate) noexcept
    {
        sr = sampleRate;
        // 11 o'clock = ~0.40 normalized on a 0-1 knob
        // Log rate: kMin × (kMax/kMin)^speed = 0.10 × (4.8/0.10)^0.40 ≈ 0.67 Hz
        constexpr float kMinHz = 0.10f;
        constexpr float kMaxHz = 4.80f;
        lfoFreq = kMinHz * std::pow(kMaxHz / kMinHz, 0.40f);
        lfoPhase = 0.0f;
        feedbackL = feedbackR = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            stateL[i] = stateR[i] = 0.0f;
        }
    }

    void Reset() noexcept
    {
        lfoPhase = 0.0f;
        feedbackL = feedbackR = 0.0f;
        for (int i = 0; i < 4; ++i)
            stateL[i] = stateR[i] = 0.0f;
    }

    // Process one stereo sample pair.
    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        lfoPhase += lfoFreq / sr;
        if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;

        // Triangle LFO
        const float tri = (lfoPhase < 0.5f)
                          ? (4.0f * lfoPhase - 1.0f)
                          : (3.0f - 4.0f * lfoPhase);

        // Sweep all-pass pole logarithmically (FET control law)
        const float norm = (tri + 1.0f) * 0.5f;
        const float fc   = kMinPoleHz * std::pow(kMaxPoleHz / kMinPoleHz, norm);
        const float t    = std::tan(3.14159265f * fc / sr);
        const float coeff = (t - 1.0f) / (t + 1.0f);

        outL = processChannel(inL, stateL, feedbackL, coeff);
        outR = processChannel(inR, stateR, feedbackR, coeff);
    }

private:
    // R28 feedback from stage 4 back into stage 2
    static constexpr float kFeedback   = 0.45f;
    static constexpr float kMinPoleHz  = 150.0f;
    static constexpr float kMaxPoleHz  = 1600.0f;
    static constexpr float kOutputMakeup = 1.0f; // unity

    static float allPass(float in, float& state, float coeff) noexcept
    {
        const float out = coeff * in + state;
        state = in - coeff * out;
        return out;
    }

    float processChannel(float dry, float* stages, float& feedback, float coeff) noexcept
    {
        const float s1  = allPass(dry,              stages[0], coeff);
        const float s2  = allPass(s1 + kFeedback * feedback, stages[1], coeff);
        const float s3  = allPass(s2, stages[2], coeff);
        const float wet = allPass(s3, stages[3], coeff);
        feedback = wet;
        return (0.5f * dry + 0.5f * wet) * kOutputMakeup;
    }

    float sr = 48000.0f;
    float lfoFreq = 0.67f;
    float lfoPhase = 0.0f;
    float feedbackL = 0.0f, feedbackR = 0.0f;
    float stateL[4] = {};
    float stateR[4] = {};
};
