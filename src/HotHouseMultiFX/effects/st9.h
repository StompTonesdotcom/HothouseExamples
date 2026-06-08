// ST-9 — Tube Screamer (TS-9) circuit model
// Ported from ST-9 JUCE plugin (StompTones).
//
// Parameters exposed for runtime control:
//   gain  — pre-gain before clipper (K1 maps 0.5–4.0)
//   drive — TS-9 drive control 0–1; shapes atan clipper curve and mid-boost gain
//   tone  — passive RC tone stack 0–1 (0=bass heavy, 0.5=noon, 1=treble bright)
//
// Note: JUCE original uses 8× oversampling around the clipper; this port runs
// at base sample rate (no oversampling implemented).

#pragma once
#include <cmath>

class ST9
{
public:
    float gain  = 1.0f;   // Pre-gain before clipper (0.5=soft, 4.0=heavy)
    float drive = 0.5f;   // TS-9 drive: 0=gentle, 1=full saturation
    float tone  = 0.5f;   // Tone stack: 0=bass heavy, 0.5=noon, 1=treble bright

    void Init(float sampleRate) noexcept
    {
        sr = sampleRate;

        // Mid boost biquad: gain varies 4–8 dB with drive (JUCE: 4+4*drive dB)
        makePeak(720.0f, 1.2f, 4.0f + 4.0f * drive,
                 midB0, midB1, midB2, midA1, midA2);

        updateToneStack(tone);
        prevDrive = drive;
        prevTone  = tone;

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

    // Process one sample for a given channel.
    float Process(float in, int channel) noexcept
    {
        // Update filter coefficients when drive or tone changes (control-rate cost)
        if (drive != prevDrive)
        {
            makePeak(720.0f, 1.2f, 4.0f + 4.0f * drive,
                     midB0, midB1, midB2, midA1, midA2);
            prevDrive = drive;
        }
        if (tone != prevTone)
        {
            updateToneStack(tone);
            prevTone = tone;
        }

        const int c = channel & 1;

        float clipped = diodeClipper(in * gain, drive);

        // Mid boost (TS-9 signature: 720 Hz peaking, gain varies with drive)
        clipped = biquad(clipped, mid_x1[c], mid_x2[c], mid_y1[c], mid_y2[c],
                         midB0, midB1, midB2, midA1, midA2);

        // Tone stack: low shelf + high shelf
        clipped = biquad(clipped, tone_x1[c], tone_x2[c], tone_y1[c], tone_y2[c],
                         toneB0, toneB1, toneB2, toneA1, toneA2);
        clipped = biquad(clipped, tone2_x1[c], tone2_x2[c], tone2_y1[c], tone2_y2[c],
                         tone2B0, tone2B1, tone2B2, tone2A1, tone2A2);

        // DC blocker
        float blocked = clipped - dc[c];
        dc[c] = dc[c] * 0.999f + clipped * 0.001f;

        // Level: drive compensation scales output to maintain consistent loudness
        const float driveComp = 1.0f - 0.528f * drive + 0.280f * (drive * drive);
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

    static constexpr float kUnityGain = 1.0f; // TUNE ON HARDWARE

    float prevDrive = -1.0f;
    float prevTone  = -1.0f;

    float sr = 48000.0f;

    float midB0=1,midB1=0,midB2=0,midA1=0,midA2=0;
    float toneB0=1,toneB1=0,toneB2=0,toneA1=0,toneA2=0;
    float tone2B0=1,tone2B1=0,tone2B2=0,tone2A1=0,tone2A2=0;

    float mid_x1[2]={}, mid_x2[2]={}, mid_y1[2]={}, mid_y2[2]={};
    float tone_x1[2]={}, tone_x2[2]={}, tone_y1[2]={}, tone_y2[2]={};
    float tone2_x1[2]={}, tone2_x2[2]={}, tone2_y1[2]={}, tone2_y2[2]={};
    float dc[2] = {};
};
