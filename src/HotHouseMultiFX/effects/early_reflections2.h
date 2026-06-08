// Early Reflections 2 — SPX90II/STX90 ER2 multi-tap algorithm
// Extracted from STX90_Programs.cpp (StompTones ST-X90 plugin).
// Fixed params: REVERSE type, room size 20, liveness 10, pre-delay 75ms, LPF off, mix 100%.
//
// Algorithm: fixed-gain multi-tap delay line with stereo panning,
// pre-delay buffer, and optional LP filter (disabled here).
// Mix is intentionally 100% wet (no dry signal) — controls externally via bypass.

#pragma once
#include <cmath>

// Large buffers are declared in the main .cpp with DSY_SDRAM_BSS
// and passed to Init() — matches the libDaisy/DaisySP SDRAM pattern.

class EarlyReflections2
{
public:
    float mix        = 1.0f;   // 0=dry, 1=100% wet
    float preDelayMs = 75.0f;  // 0–200ms: controls when the swell starts
    float room       = 20.0f;  // 5–30: tap spacing scale (larger = bigger room)

    // externalBuf must be at least kBufMax floats, allocated in SDRAM by the caller.
    void Init(float sampleRate, float* externalBuf) noexcept
    {
        sr  = sampleRate;
        buf = externalBuf;

        preBufSize      = static_cast<int>(sampleRate * 0.090f) + 2;
        if (preBufSize > kPreBufMax) preBufSize = kPreBufMax;
        preDelaySamples = static_cast<int>(preDelayMs * sampleRate / 1000.0f);
        if (preDelaySamples >= preBufSize) preDelaySamples = preBufSize - 1;

        buildTapPattern();
        prevPreDelayMs = preDelayMs;
        prevRoom       = room;
        Reset();
    }

    void Reset() noexcept
    {
        if (buf) for (int i = 0; i < kBufMax; ++i) buf[i] = 0.0f;
        for (int i = 0; i < kPreBufMax; ++i) preBuf[i] = 0.0f;
        writePos = prePos = 0;
        lpStateL = lpStateR = 0.0f;
    }

    // Process one stereo sample. outL/outR are 100% wet (ER signal only).
    // mix is applied here so knob 1 controls wet level.
    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        if (!buf) { outL = inL; outR = inR; return; }

        // Update pre-delay or tap pattern if knobs changed (control-rate cost, not per-sample)
        if (preDelayMs != prevPreDelayMs)
        {
            preDelaySamples = static_cast<int>(preDelayMs * sr / 1000.0f);
            if (preDelaySamples >= preBufSize) preDelaySamples = preBufSize - 1;
            prevPreDelayMs = preDelayMs;
        }
        if (room != prevRoom)
        {
            buildTapPattern();
            prevRoom = room;
        }

        const float monoIn = (inL + inR) * 0.5f;

        preBuf[prePos] = monoIn;
        const int preRead = (prePos - preDelaySamples + preBufSize) % preBufSize;
        const float delayed = preBuf[preRead];
        prePos = (prePos + 1 < preBufSize) ? prePos + 1 : 0;

        buf[writePos] = delayed;

        float sumL = 0.0f, sumR = 0.0f;
        for (int t = 0; t < tapCount; ++t)
        {
            const int readIdx = (writePos - taps[t].delaySamples + kBufMax) % kBufMax;
            const float s = buf[readIdx];
            sumL += s * taps[t].gainL;
            sumR += s * taps[t].gainR;
        }

        writePos = (writePos + 1 < kBufMax) ? writePos + 1 : 0;

        // LPF disabled (fixed at 0 per user settings)
        // mix: 0 = dry passthrough, 1 = 100% ER wet (user wants 100% wet)
        outL = inL * (1.0f - mix) + sumL * mix;
        outR = inR * (1.0f - mix) + sumR * mix;
    }

    static constexpr int kBufMax    = 65536; // ~1.36s @ 48kHz — declare in SDRAM from main
    static constexpr int kPreBufMax = 8192;  // >170ms @ 48kHz — SRAM is fine

private:
    struct Tap { int delaySamples; float gainL, gainR; };
    static constexpr int kMaxTaps   = 13;

    void buildTapPattern() noexcept
    {
        // REVERSE type (index 2 in the STX90 kTapFamilies table):
        // widening stereo spread as taps age — pairs with rising gain envelope.
        // Room size 20, liveness 10 (maximum).
        static constexpr float kDelayMs[kMaxTaps] = {
            0.30f, 2.55f, 5.00f, 7.55f, 10.30f, 13.35f, 16.70f,
            20.40f, 24.40f, 28.75f, 33.45f, 38.45f, 43.75f
        };
        static constexpr float kPanPos[kMaxTaps] = {
            -0.08f, 0.08f, -0.20f, 0.20f, -0.34f, 0.34f, -0.50f,
             0.50f, -0.64f, 0.64f, -0.76f, 0.76f, -0.90f
        };

        constexpr int activeTapCount = 13; // ER2 uses all 13 taps (vs ER1's 5)
        const float kRoomSize = room;  // runtime — use member value

        // Rising gain envelope (REVERSE type, liveness 10 → riseSpanDb = 10 dB)
        constexpr float riseSpanDb = 10.0f;

        float baseGains[kMaxTaps];
        float totalBase = 0.0f;
        for (int i = 0; i < activeTapCount; ++i)
        {
            const float nr = (activeTapCount > 1) ? float(i) / float(activeTapCount - 1) : 0.0f;
            const float dB = -riseSpanDb + nr * riseSpanDb; // rises from -10 dB to 0 dB
            baseGains[i] = std::pow(10.0f, dB / 20.0f);
            totalBase += baseGains[i];
        }

        // Normalise to target level (ER2, liveness=10 → targetBase = 1.05)
        constexpr float targetBase = 1.05f;
        const float normScale = (totalBase > 1.0e-6f) ? (targetBase / totalBase) : 1.0f;

        tapCount = 0;
        for (int i = 0; i < activeTapCount; ++i)
        {
            const float delayMs = kRoomSize * kDelayMs[i];
            const float pan     = kPanPos[i];
            const float gain    = baseGains[i] * normScale;

            // Constant-power panning
            const float angle = (1.0f + pan) * 3.14159265f * 0.25f;
            taps[tapCount].delaySamples = static_cast<int>(delayMs * sr / 1000.0f + 0.5f);
            if (taps[tapCount].delaySamples < 1) taps[tapCount].delaySamples = 1;
            if (taps[tapCount].delaySamples >= kBufMax) taps[tapCount].delaySamples = kBufMax - 1;
            taps[tapCount].gainL = gain * std::cos(angle);
            taps[tapCount].gainR = gain * std::sin(angle);
            ++tapCount;
        }

        // Equalise L/R channel energy
        float totalL = 0.0f, totalR = 0.0f;
        for (int t = 0; t < tapCount; ++t)
        {
            totalL += taps[t].gainL * taps[t].gainL;
            totalR += taps[t].gainR * taps[t].gainR;
        }
        if (totalL > 1.0e-12f && totalR > 1.0e-12f)
        {
            const float avg    = (totalL + totalR) * 0.5f;
            const float scaleL = std::sqrt(avg / totalL);
            const float scaleR = std::sqrt(avg / totalR);
            for (int t = 0; t < tapCount; ++t)
            {
                taps[t].gainL *= scaleL;
                taps[t].gainR *= scaleR;
            }
        }
    }

    float prevPreDelayMs = -1.0f;
    float prevRoom       = -1.0f;

    float sr = 48000.0f;
    int   preBufSize = 0;
    int   preDelaySamples = 0;
    int   tapCount = 0;
    Tap   taps[kMaxTaps] = {};
    int   writePos = 0;
    int   prePos   = 0;
    float lpStateL = 0.0f, lpStateR = 0.0f;

    // Pointer to external SDRAM buffer (set by Init)
    float* buf = nullptr;
    // Pre-delay: 32KB in SRAM is fine
    float preBuf[kPreBufMax] = {};
};
