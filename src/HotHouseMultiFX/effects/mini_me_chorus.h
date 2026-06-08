// Mini Me Chorus — EHX Small Clone model
// Ported from Mini-Me JUCE plugin (StompTones).
// Fixed params: default settings (rate=0.40 normalized, shallow depth ±1.5ms).
// Runs at base sample rate (4× OS not needed for chorus).

#pragma once
#include <cmath>

class MiniMeChorus
{
public:
    static constexpr int kMaxDelaySamples = 2048; // >15ms at 48kHz, enough for modulation

    void Init(float sampleRate) noexcept
    {
        sr = sampleRate;

        // Default: rate param 0.40 → lfoHz via exponential: 0.5 × 18^0.40 ≈ 1.72 Hz
        lfoHz = 0.5f * std::pow(18.0f, 0.40f);
        lfoPhase = 0.0;

        // Depth: shallow = ±1.5 ms
        depthMs = 1.5f;
        centerDelaySamples = 7.0f * sampleRate / 1000.0f;

        // Pre/post BBD anti-aliasing LP @ 8kHz (one-pole IIR)
        const float fc = 8000.0f;
        lpCoeff = 1.0f - std::exp(-6.28318f * fc / sampleRate);

        // Delay smoothing: 60 Hz cutoff on the delay value to soften LFO corner
        delaySmoothCoeff = 1.0f - std::exp(-6.28318f * 60.0f / sampleRate);
        smoothedDelay = centerDelaySamples;

        Reset();
    }

    void Reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            for (int i = 0; i < kMaxDelaySamples; ++i)
                delayBuf[c][i] = 0.0f;
            writePos[c] = 0;
            preState[c] = 0.0f;
            reconState[c] = 0.0f;
        }
        lfoPhase = 0.0;
        smoothedDelay = centerDelaySamples;
    }

    // Process one sample per channel. Stereo in/out.
    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        // Advance LFO (triangle wave matching LM358 oscillator)
        lfoPhase += lfoHz / sr;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;
        const float lfoVal = static_cast<float>(
            2.0 * std::abs(2.0 * lfoPhase - 1.0) - 1.0);

        // Target delay time
        const float targetDelay = centerDelaySamples + lfoVal * depthMs * sr / 1000.0f;
        smoothedDelay += delaySmoothCoeff * (targetDelay - smoothedDelay);

        const int   delayI = static_cast<int>(smoothedDelay);
        const float frac   = smoothedDelay - static_cast<float>(delayI);

        const float in[2] = { inL, inR };
        float out[2];

        for (int c = 0; c < 2; ++c)
        {
            // Pre-BBD LP filter
            const float filtered = preState[c] + lpCoeff * (in[c] - preState[c]);
            preState[c] = filtered;

            // Write
            delayBuf[c][writePos[c]] = filtered;

            // Catmull-Rom read
            const int im1 = (writePos[c] + kMaxDelaySamples - delayI + 1) % kMaxDelaySamples;
            const int i0  = (writePos[c] + kMaxDelaySamples - delayI    ) % kMaxDelaySamples;
            const int i1  = (writePos[c] + kMaxDelaySamples - delayI - 1) % kMaxDelaySamples;
            const int i2  = (writePos[c] + kMaxDelaySamples - delayI - 2) % kMaxDelaySamples;

            const float ym1 = delayBuf[c][im1];
            const float y0  = delayBuf[c][i0];
            const float y1  = delayBuf[c][i1];
            const float y2  = delayBuf[c][i2];

            const float cr_a = 0.5f * (-ym1 + y1);
            const float cr_b = 0.5f * (2.0f*ym1 - 5.0f*y0 + 4.0f*y1 - y2);
            const float cr_c = 0.5f * (-ym1 + 3.0f*y0 - 3.0f*y1 + y2);
            const float wet  = y0 + frac * (cr_a + frac * (cr_b + frac * cr_c));

            // Post-BBD LP filter
            const float reconWet = reconState[c] + lpCoeff * (wet - reconState[c]);
            reconState[c] = reconWet;

            // Passive mix: 27/49 dry + 22/49 wet
            out[c] = (in[c] * kDryWeight + reconWet * kWetWeight) * kMakeup;

            writePos[c] = (writePos[c] + 1) % kMaxDelaySamples;
        }

        outL = out[0];
        outR = out[1];
    }

private:
    static constexpr float kDryWeight = 27.0f / 49.0f;
    static constexpr float kWetWeight = 22.0f / 49.0f;
    static constexpr float kMakeup    = 1.18f;

    float sr = 48000.0f;
    float lfoHz = 1.72f;
    float depthMs = 1.5f;
    float centerDelaySamples = 336.0f;
    float lpCoeff = 0.0f;
    float delaySmoothCoeff = 0.0f;
    float smoothedDelay = 336.0f;
    double lfoPhase = 0.0;

    float delayBuf[2][kMaxDelaySamples] = {};
    int   writePos[2] = {};
    float preState[2]   = {};
    float reconState[2] = {};
};
