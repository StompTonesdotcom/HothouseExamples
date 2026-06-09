// LovelessReverb — StompTones
// Ported from Loveless JUCE plugin.
//
// A Freeverb-based reverb driven by a triggered bloom envelope: each guitar
// attack causes the reverb tail to swell in and fade over `bloom` seconds,
// creating a reverse-reverb character.  LFO pitch modulation (sway) adds a
// slow, wobbling shimmer.  grit is fixed at 0 internally.
//
// Parameters (5 knobs in HotShoegaze):
//   bloom      0.1–3.0 s   K1 — swell duration / tail length
//   sway       0–1         K2 — pitch modulation depth
//   wash       200–20000Hz K3 — reverb brightness (output LP cutoff)
//   mix        0–1         K4 — wet/dry
//   predelay   0–200 ms    K5 — silence before each bloom swell starts
//
// SDRAM buffers required (declare with DSY_SDRAM_BSS):
//   float combL [LovelessReverb::kCombTotalL]
//   float combR [LovelessReverb::kCombTotalR]
//   float apL   [LovelessReverb::kAPTotalL]
//   float apR   [LovelessReverb::kAPTotalR]
//   float swayL [LovelessReverb::kSwayLen]
//   float swayR [LovelessReverb::kSwayLen]

#pragma once
#include <cmath>
#include <cstring>
#include "fast_math.h"

class LovelessReverb
{
public:
    // ── Parameters ──────────────────────────────────────────────────────────
    float bloom    = 1.5f;     // 0.1–3.0 s
    float sway     = 0.5f;     // 0–1
    float mix      = 0.5f;     // 0–1
    float wash     = 10100.0f; // Hz
    float predelay = 100.0f;   // ms
    // grit is fixed at 0 — no saturation on the wet signal

    // ── Buffer size constants (use for SDRAM declarations) ──────────────────
    static constexpr int kNumCombs  = 8;
    static constexpr int kNumAP     = 4;
    static constexpr int kMaxVoices = 8;
    static constexpr int kSwayLen   = 2048;  // power-of-2 for fast modulo

    // Freeverb comb lengths at 48 kHz  (original 44.1 kHz values × 48000/44100)
    static constexpr int kCombLenL[kNumCombs] = { 1214, 1293, 1390, 1476,
                                                   1548, 1622, 1695, 1760 };
    static constexpr int kCombLenR[kNumCombs] = { 1237, 1316, 1413, 1499,
                                                   1571, 1645, 1718, 1783 };
    static constexpr int kCombTotalL = 1214+1293+1390+1476+1548+1622+1695+1760; // 11998
    static constexpr int kCombTotalR = 1237+1316+1413+1499+1571+1645+1718+1783; // 12182

    // Freeverb all-pass lengths at 48 kHz  (original: 556, 441, 341, 225 + 23)
    static constexpr int kAPLenL[kNumAP] = { 605, 480, 371, 245 };
    static constexpr int kAPLenR[kNumAP] = { 628, 503, 394, 268 };
    static constexpr int kAPTotalL = 605+480+371+245; // 1701
    static constexpr int kAPTotalR = 628+503+394+268; // 1793

    // ── Init ────────────────────────────────────────────────────────────────
    void Init(float sampleRate,
              float* combBufL, float* combBufR,
              float* apBufL,   float* apBufR,
              float* swayBufL, float* swayBufR) noexcept
    {
        sr = sampleRate;

        // Local copies avoid the C++14 constexpr-array ODR issue that would
        // require out-of-class definitions when arrays are indexed at runtime.
        static const int cl[kNumCombs] = { 1214, 1293, 1390, 1476,
                                           1548, 1622, 1695, 1760 };
        static const int cr[kNumCombs] = { 1237, 1316, 1413, 1499,
                                           1571, 1645, 1718, 1783 };
        static const int al[kNumAP]    = { 605, 480, 371, 245 };
        static const int ar[kNumAP]    = { 628, 503, 394, 268 };

        // Wire packed comb buffers
        int ol = 0, or_ = 0;
        for (int i = 0; i < kNumCombs; ++i) {
            combL[i].init(combBufL + ol,  cl[i]);
            combR[i].init(combBufR + or_, cr[i]);
            ol  += cl[i];
            or_ += cr[i];
        }

        // Wire packed all-pass buffers
        int alo = 0, aro = 0;
        for (int i = 0; i < kNumAP; ++i) {
            apL_[i].init(apBufL + alo, al[i]);
            apR_[i].init(apBufR + aro, ar[i]);
            alo += al[i];
            aro += ar[i];
        }

        // Sway delay
        swL  = swayBufL;
        swR  = swayBufR;
        swPos = 0;

        // Zero all SDRAM buffers (safe — called before audio starts)
        std::memset(combBufL, 0, kCombTotalL * sizeof(float));
        std::memset(combBufR, 0, kCombTotalR * sizeof(float));
        std::memset(apBufL,   0, kAPTotalL   * sizeof(float));
        std::memset(apBufR,   0, kAPTotalR   * sizeof(float));
        std::memset(swayBufL, 0, kSwayLen    * sizeof(float));
        std::memset(swayBufR, 0, kSwayLen    * sizeof(float));

        // Envelope / bloom state
        lastEnv  = 0.0f;
        debounce = 0;
        for (int v = 0; v < kMaxVoices; ++v) voices[v] = BloomVoice{};

        // Wash LP state
        washStL = washStR = 0.0f;

        // LFO: quadrature oscillator starts at zero phase
        lfoSin = 0.0f;
        lfoCos = 1.0f;
        lastSway_ = -1.0f;

        // Reverb param cache — force recompute on first Process()
        cachedBloom = cachedWash = -999.0f;

        _updateParams();
    }

    // ── Per-sample process ──────────────────────────────────────────────────
    //   Call once per sample.  outL/outR include the dry signal (internal mix).
    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        _updateParams();
        _stepLFO();

        // ── Freeverb reverb (100% wet) ────────────────────────────────────
        // Both L and R combs receive the same scaled mono input; different
        // comb lengths and AP lengths create natural stereo spread.
        const float mono = (inL + inR) * 0.015f;
        float revL = 0.0f, revR = 0.0f;
        for (int i = 0; i < kNumCombs; ++i) {
            revL += combL[i].process(mono, combFb, combD, combD2);
            revR += combR[i].process(mono, combFb, combD, combD2);
        }
        for (int i = 0; i < kNumAP; ++i) {
            revL = apL_[i].process(revL);
            revR = apR_[i].process(revR);
        }

        // ── Sway pitch modulation ─────────────────────────────────────────
        // Always write to keep the buffer fresh even when sway is off.
        swL[swPos] = revL;
        swR[swPos] = revR;
        if (sway > 0.01f) {
            float dt = 1000.0f + lfoSin * sway * 250.0f;
            // Clamp to valid buffer range (10 to kSwayLen-2)
            if (dt < 10.0f)                  dt = 10.0f;
            if (dt > float(kSwayLen - 2))    dt = float(kSwayLen - 2);
            const int   dtI  = (int)dt;
            const float dtFr = dt - (float)dtI;
            const int   ri0  = (swPos - dtI + kSwayLen) & (kSwayLen - 1);
            const int   ri1  = (ri0 - 1 + kSwayLen)     & (kSwayLen - 1);
            revL = swL[ri0] + dtFr * (swL[ri1] - swL[ri0]);
            revR = swR[ri0] + dtFr * (swR[ri1] - swR[ri0]);
        }
        swPos = (swPos + 1) & (kSwayLen - 1);

        // ── Triggered bloom envelope ──────────────────────────────────────
        // Attack detection: trigger on raw input peak rising by > 0.003 in one sample.
        const float peak  = inL > 0 ? inL : -inL;
        const float peakR = inR > 0 ? inR : -inR;
        const float inputPeak = peak > peakR ? peak : peakR;
        const float delta = inputPeak - lastEnv;
        lastEnv = inputPeak;

        if (debounce > 0) --debounce;

        if (delta > 0.003f && debounce == 0) {
            const int   cd  = (int)(predelay * sr * 0.001f);
            const float inc = 1.0f / (bloom * sr > 1.0f ? bloom * sr : 1.0f);
            for (int v = 0; v < kMaxVoices; ++v) {
                if (!voices[v].active) {
                    voices[v].active    = true;
                    voices[v].countdown = cd;
                    voices[v].phase     = 0.0f;
                    voices[v].phaseInc  = inc;
                    debounce = (int)(0.1f * sr);
                    break;
                }
            }
        }

        // Accumulate envelope across all active voices
        float env = 0.0f;
        for (int v = 0; v < kMaxVoices; ++v) {
            if (!voices[v].active) continue;
            if (voices[v].countdown > 0) { --voices[v].countdown; continue; }
            const float ph = voices[v].phase;
            float e;
            if (ph < 0.65f) {
                const float t = ph / 0.65f;
                e = t * __builtin_sqrtf(t);  // pow(t, 1.5)
            } else {
                const float t = (ph - 0.65f) / 0.35f;
                e = 1.0f - t * t;            // 1 - pow(t, 2)
            }
            env += e;
            voices[v].phase += voices[v].phaseInc;
            if (voices[v].phase >= 1.0f) voices[v].active = false;
        }
        // Limit combined envelope to 2.0 (matches JUCE jlimit)
        if (env > 2.0f) env = 2.0f;

        revL *= env;
        revR *= env;

        // grit = 0 → saturation stage skipped

        // ── Wash LP filter ────────────────────────────────────────────────
        washStL += washAlpha * (revL - washStL);
        washStR += washAlpha * (revR - washStR);

        // ── Auto-gain + wet/dry blend ─────────────────────────────────────
        // Compensates for higher apparent loudness at long bloom / heavy sway.
        // Formula from JUCE source (grit term removed since grit=0):
        //   wetAutoGain = 1 / (1 + (bloomNorm*0.4 + sway*0.3) * 1.5)
        const float bloomNorm   = (bloom - 0.1f) / 2.9f;
        const float wetAutoGain = 1.0f / (1.0f + (bloomNorm * 0.4f + sway * 0.3f) * 1.5f);
        const float wetScale    = mix * 0.6f * wetAutoGain;
        const float dryScale    = 1.0f - mix;

        float ol = inL * dryScale + washStL * wetScale;
        float or_= inR * dryScale + washStR * wetScale;

        // Safety soft clip (matches JUCE): engage only if signal is hot
        if (ol > 0.95f || ol < -0.95f) ol = fastTanh(ol * 0.95f);
        if (or_> 0.95f || or_< -0.95f) or_= fastTanh(or_* 0.95f);

        outL = ol;
        outR = or_;
    }

private:
    // ── Inner types ──────────────────────────────────────────────────────────

    // Freeverb comb filter with 1-pole LP in feedback loop
    struct CombFilter {
        float* buf  = nullptr;
        int    len  = 0, pos = 0;
        float  lpSt = 0.0f;

        void init(float* b, int l) noexcept { buf = b; len = l; pos = 0; lpSt = 0.0f; }

        // damp  = high-frequency damping coefficient (0=bright, 0.4=dark)
        // damp2 = 1 - damp
        float process(float in, float fb, float damp, float damp2) noexcept {
            float out = buf[pos];
            lpSt = out * damp2 + lpSt * damp;   // 1-pole LP on output
            buf[pos] = in + lpSt * fb;
            if (++pos >= len) pos = 0;
            return out;
        }
    };

    // Freeverb all-pass filter (feedback = 0.5, fixed)
    struct AllpassFilter {
        float* buf = nullptr;
        int    len = 0, pos = 0;

        void init(float* b, int l) noexcept { buf = b; len = l; pos = 0; }

        float process(float in) noexcept {
            float out = buf[pos];
            buf[pos]  = in + out * 0.5f;
            if (++pos >= len) pos = 0;
            return out - in;
        }
    };

    struct BloomVoice {
        bool  active    = false;
        int   countdown = 0;    // pre-delay countdown (samples)
        float phase     = 0.0f; // 0→1 over bloom duration
        float phaseInc  = 0.0f; // 1 / (bloom * sr)
    };

    // ── State ────────────────────────────────────────────────────────────────
    float sr = 48000.0f;

    CombFilter    combL[kNumCombs], combR[kNumCombs];
    AllpassFilter apL_[kNumAP],     apR_[kNumAP];

    float* swL  = nullptr;
    float* swR  = nullptr;
    int    swPos = 0;

    float lfoSin  = 0.0f, lfoCos  = 1.0f;
    float lfoC    = 1.0f, lfoS    = 0.0f;  // quadrature step coefficients
    float lastSway_ = -1.0f;

    BloomVoice voices[kMaxVoices];
    int   debounce = 0;
    float lastEnv  = 0.0f;

    float washStL = 0.0f, washStR = 0.0f;
    float washAlpha = 0.7f;

    float combFb = 0.9f, combD = 0.2f, combD2 = 0.8f;
    float cachedBloom = -999.0f, cachedWash = -999.0f;

    // ── Param helpers ────────────────────────────────────────────────────────

    void _updateParams() noexcept {
        const bool boomChanged = (bloom - cachedBloom > 0.005f || cachedBloom - bloom > 0.005f);
        const bool washChanged = (wash  - cachedWash  > 5.0f   || cachedWash  - wash  > 5.0f);

        if (boomChanged || washChanged) {
            cachedBloom = bloom;
            cachedWash  = wash;

            // Reverb feedback from bloom (matches JUCE roomScaleFactor/roomOffset)
            const float roomSize = 0.5f + (bloom / 3.0f) * 0.45f;
            combFb = roomSize * 0.28f + 0.7f;

            // Comb LP damping from wash (higher wash = less damping = brighter)
            const float damping = 0.2f + (1.0f - wash / 20000.0f) * 0.6f;
            combD  = damping * 0.4f;
            combD2 = 1.0f - combD;

            // Wash LP filter coefficient (1-pole)
            washAlpha = 1.0f - expf(-6.28318f * wash / sr);
        }

        if (sway - lastSway_ > 0.01f || lastSway_ - sway > 0.01f) {
            lastSway_ = sway;
            const float freq  = 0.15f + sway * 0.6f;   // 0.15–0.75 Hz
            const float theta = 6.28318f * freq / sr;
            lfoC = cosf(theta);
            lfoS = sinf(theta);
        }
    }

    // Advance quadrature oscillator by one sample (no transcendentals per sample)
    void _stepLFO() noexcept {
        const float s = lfoSin * lfoC + lfoCos * lfoS;
        const float c = lfoCos * lfoC - lfoSin * lfoS;
        lfoSin = s;
        lfoCos = c;
    }
};
