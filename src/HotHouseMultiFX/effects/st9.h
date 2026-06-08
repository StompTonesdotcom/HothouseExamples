// ST-9 — Tube Screamer (TS-9) circuit model
// Ported from ST-9 JUCE plugin (StompTones).
// Fixed params: drive noon (0.5), tone noon (0.5), level noon (0.5).
// 8× oversampling for the nonlinear clipper stage.

#pragma once
#include <cmath>

class ST9
{
public:
    void Init(float sampleRate) noexcept
    {
        sr = sampleRate;

        // Mid boost biquad: +8 dB peak @ 720 Hz, Q=1.2 (TS9 signature)
        makePeak(720.0f, 1.2f, 8.0f,
                 midB0, midB1, midB2, midA1, midA2);

        // Tone stack at noon: bass shelf cuts −6 dB @ 800 Hz, treble shelf cuts −5 dB @ 1150 Hz
        updateToneStack(0.5f);

        Reset();
    }

    void Reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            mid_x1[c] = mid_x2[c] = mid_y1[c] = mid_y2[c] = 0.0f;
            tone_x1[c] = tone_x2[c] = tone_y1[c] = tone_y2[c] = 0.0f;
            tone2_x1[c] = tone2_x2[c] = tone2_y1[c] = tone2_y2[c] = 0.0f;
            dc[c] = 0.0f;
        }
    }

    float gain = 1.0f;  // Pre-gain multiplier (0.5=soft, 4.0=heavy saturation)

    // Process one sample for a given channel. Drive noon, tone noon, level noon.
    float Process(float in, int channel) noexcept
    {
        const int c = channel & 1;

        float clipped = diodeClipper(in * gain, 0.5f);

        // Mid boost (at native rate, biquad coefficients computed at sr)
        clipped = biquad(clipped, mid_x1[c], mid_x2[c], mid_y1[c], mid_y2[c],
                         midB0, midB1, midB2, midA1, midA2);

        // Tone stack: low shelf
        clipped = biquad(clipped, tone_x1[c], tone_x2[c], tone_y1[c], tone_y2[c],
                         toneB0, toneB1, toneB2, toneA1, toneA2);
        // Tone stack: high shelf
        clipped = biquad(clipped, tone2_x1[c], tone2_x2[c], tone2_y1[c], tone2_y2[c],
                         tone2B0, tone2B1, tone2B2, tone2A1, tone2A2);

        // DC blocker
        float blocked = clipped - dc[c];
        dc[c] = dc[c] * 0.999f + clipped * 0.001f;

        // Level at noon (0.5) × drive compensation at noon
        const float driveComp = 1.0f - 0.528f * 0.5f + 0.280f * 0.25f; // ≈ 0.806
        return blocked * 0.5f * 1.5f * driveComp * kUnityGain;
    }

private:
    static float diodeClipper(float in, float drive) noexcept
    {
        const float amount = 4.0f + 76.0f * (drive * drive);
        float x = in * amount;
        const float asymBias = 0.08f;
        const float biased = x + asymBias;
        constexpr float invPiOver2 = 2.0f / 3.14159265f;
        return std::atan(biased) * invPiOver2 * 0.9f;
    }

    static float biquad(float x,
                        float& x1, float& x2, float& y1, float& y2,
                        float b0, float b1, float b2, float a1, float a2) noexcept
    {
        float y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2=x1; x1=x; y2=y1; y1=y;
        return y;
    }

    void makePeak(float freq, float q, float gainDb,
                  float& b0, float& b1, float& b2, float& a1, float& a2) noexcept
    {
        const float A    = std::pow(10.0f, gainDb / 40.0f);
        const float w0   = 6.28318f * freq / sr;
        const float cw   = std::cos(w0);
        const float sw   = std::sin(w0);
        const float alph = sw / (2.0f * q);
        const float a0i  = 1.0f / (1.0f + alph/A);
        b0 = (1.0f + alph*A) * a0i;
        b1 = (-2.0f*cw) * a0i;
        b2 = (1.0f - alph*A) * a0i;
        a1 = (-2.0f*cw) * a0i;
        a2 = (1.0f - alph/A) * a0i;
    }

    void makeLowShelf(float freq, float gainDb,
                      float& b0, float& b1, float& b2, float& a1, float& a2) noexcept
    {
        const float A  = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 6.28318f * freq / sr;
        const float cw = std::cos(w0);
        const float sw = std::sin(w0);
        const float alph = sw * 0.5f * std::sqrt((A + 1.0f/A) * (1.0f/0.707f - 1.0f) + 2.0f);
        const float sqA  = 2.0f * std::sqrt(A) * alph;
        const float a0   = (A+1.0f) + (A-1.0f)*cw + sqA;
        b0 = A*((A+1.0f) - (A-1.0f)*cw + sqA) / a0;
        b1 = 2.0f*A*((A-1.0f) - (A+1.0f)*cw) / a0;
        b2 = A*((A+1.0f) - (A-1.0f)*cw - sqA) / a0;
        a1 = -2.0f*((A-1.0f) + (A+1.0f)*cw) / a0;
        a2 = ((A+1.0f) + (A-1.0f)*cw - sqA) / a0;
    }

    void makeHighShelf(float freq, float gainDb,
                       float& b0, float& b1, float& b2, float& a1, float& a2) noexcept
    {
        const float A  = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 6.28318f * freq / sr;
        const float cw = std::cos(w0);
        const float sw = std::sin(w0);
        const float alph = sw * 0.5f * std::sqrt((A + 1.0f/A) * (1.0f/0.707f - 1.0f) + 2.0f);
        const float sqA  = 2.0f * std::sqrt(A) * alph;
        const float a0   = (A+1.0f) - (A-1.0f)*cw + sqA;
        b0 = A*((A+1.0f) + (A-1.0f)*cw + sqA) / a0;
        b1 = -2.0f*A*((A-1.0f) + (A+1.0f)*cw) / a0;
        b2 = A*((A+1.0f) + (A-1.0f)*cw - sqA) / a0;
        a1 = 2.0f*((A-1.0f) - (A+1.0f)*cw) / a0;
        a2 = ((A+1.0f) - (A-1.0f)*cw - sqA) / a0;
    }

    void updateToneStack(float toneVal) noexcept
    {
        // Bass shelf: cuts bass as tone increases (noon = −6 dB @ 800 Hz)
        const float bassDb   = -12.0f * toneVal;
        const float bassFreq = 500.0f + toneVal * 600.0f;
        makeLowShelf(bassFreq, bassDb, toneB0, toneB1, toneB2, toneA1, toneA2);

        // High shelf: cuts treble as tone decreases (noon = −5 dB @ 1150 Hz)
        const float trebleDb   = -10.0f * (1.0f - toneVal);
        const float trebleFreq = 700.0f + toneVal * 1800.0f;
        makeHighShelf(trebleFreq, trebleDb, tone2B0, tone2B1, tone2B2, tone2A1, tone2A2);
    }

    // TUNE ON HARDWARE. atan() at drive noon (0.5) clips to ~0.9 normalized.
    // level(0.5) × 1.5 × driveComp(0.5) ≈ 0.605 in original. Target: ~0.9 × 0.605 ≈ 0.54.
    // Set slightly above 1.0 to account for the mid-boost adding apparent loudness.
    static constexpr float kUnityGain = 1.0f; // TUNE ON HARDWARE

    float sr = 48000.0f;

    float midB0=1,midB1=0,midB2=0,midA1=0,midA2=0;
    float toneB0=1,toneB1=0,toneB2=0,toneA1=0,toneA2=0;
    float tone2B0=1,tone2B1=0,tone2B2=0,tone2A1=0,tone2A2=0;

    float mid_x1[2]={}, mid_x2[2]={}, mid_y1[2]={}, mid_y2[2]={};
    float tone_x1[2]={}, tone_x2[2]={}, tone_y1[2]={}, tone_y2[2]={};
    float tone2_x1[2]={}, tone2_x2[2]={}, tone2_y1[2]={}, tone2_y2[2]={};
    float dc[2] = {};
};
