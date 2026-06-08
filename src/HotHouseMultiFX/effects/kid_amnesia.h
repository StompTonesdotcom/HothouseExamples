// Kid Amnesia — Deluxe Memory Man BBD delay model
// Ported from Amnesia JUCE plugin (StompTones).
// Runs at base sample rate (no oversampling needed).
// Delay buffer supplied externally (SDRAM).
//
// CPU optimizations: LFO uses quadrature recurrence oscillator (no sin per sample),
// std::tanh replaced with fastTanh polynomial.
//
// Parameters (knobs):
//   delay    20–550 ms  — delay time
//   feedback 0–1.05     — repeats (>1 allows self-oscillation)
//   blend    0–1        — dry/wet
//   chrvib   0–1        — LFO rate: chorus (slow) → vibrato (fast)
//   depth    0–1        — LFO depth (0=none, 1=±5ms)

#pragma once
#include <cmath>
#include "fast_math.h"

class KidAmnesia
{
public:
    static constexpr int kMaxDelaySamples = 124096; // 2.5s @ 48kHz + headroom

    float delay    = 300.0f;
    float feedback = 0.5f;
    float blend    = 0.5f;
    float chrvib   = 0.0f;
    float depth    = 0.3f;

    // externalBuf: kMaxDelaySamples floats in SDRAM
    void Init(float sampleRate, float* externalBuf) noexcept
    {
        sr       = sampleRate;
        delayBuf = externalBuf;

        // NE570 compander time constants at base rate
        compAtt = 1.0f - std::exp(-1.0f / (0.001f * sr));
        compRel = 1.0f - std::exp(-1.0f / (0.250f * sr));
        expAtt  = compAtt;
        expRel  = compRel;

        // Pre-emphasis: +6 dB high shelf @ 2.3 kHz (into BBD)
        makeHS(2300.0f, 2.0f, preB0, preB1, preB2, preA1, preA2);
        // De-emphasis: -6 dB high shelf @ 2.3 kHz (out of BBD)
        makeHS(2300.0f, 0.5f, deB0,  deB1,  deB2,  deA1,  deA2);

        // BBD input/output LP (updated per-block when delay changes)
        lastCutoff = -1.0f;
        updateBBD(delay);

        smoothDelay  = delay * sr / 1000.0f;
        lfoSin = 0.0f; lfoCos = 1.0f; prevChrvib = -99.0f;
        writePos    = 0;
        compEnv = expEnv = 0.0f;

        Reset();
    }

    void Reset() noexcept
    {
        if (delayBuf)
            for (int i = 0; i < kMaxDelaySamples; ++i)
                delayBuf[i] = 0.0f;
        writePos = 0;
        compEnv = expEnv = 0.0f;
        lfoSin = 0.0f; lfoCos = 1.0f; prevChrvib = -99.0f;
        preS = {}; deS = {}; bbdInS = {}; bbdOutS = {};
    }

    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        if (!delayBuf) { outL = inL; outR = inR; return; }

        updateBBD(delay);

        // Smooth delay (first-order ramp, ~50ms TC)
        const float target = fmaxf(20.0f * sr / 1000.0f,
                                   fminf(550.0f * sr / 1000.0f, delay * sr / 1000.0f));
        smoothDelay += 0.00208f * (target - smoothDelay); // ≈ 1/(0.050×48kHz/4samples)

        // LFO — quadrature oscillator (no sin/cos per sample)
        // Increments recomputed only when chrvib knob changes
        constexpr float kChR = 0.4f, kVibR = 4.0f;
        if (fabsf(chrvib - prevChrvib) > 0.001f) {
            prevChrvib = chrvib;
            const float omega = (kChR + chrvib * (kVibR - kChR)) * 6.28318f / sr;
            lfoSinInc = sinf(omega);   // exact, called once per control change
            lfoCosInc = cosf(omega);
        }
        // Rotate by one sample — 4 mul + 2 add, no transcendentals
        const float ns = lfoSin * lfoCosInc + lfoCos * lfoSinInc;
        lfoCos = lfoCos * lfoCosInc - lfoSin * lfoSinInc;
        lfoSin = ns;

        const float modMs   = lfoSin * depth * 5.0f;
        const float delayMs = fmaxf(20.0f, fminf(550.0f, delay + modMs));
        const float delaySamples = delayMs * sr / 1000.0f;

        // Mono sum (DMM is a mono pedal)
        const float monoIn = (inL + inR) * 0.5f;

        // Step 1: Read + BBD output path
        float delayed = read(delaySamples);
        delayed = biquad(delayed, bbdOutS, bbdOB0, bbdOB1, bbdOB2, bbdOA1, bbdOA2);
        delayed = ne570Exp(delayed);
        delayed = biquad(delayed, deS, deB0, deB1, deB2, deA1, deA2);
        delayed = fastTanh(delayed * 0.6f) / 0.6f;

        // Step 2: Input path + write
        const float fbSig = delayed * feedback;
        const float preEmp = biquad(monoIn, preS, preB0, preB1, preB2, preA1, preA2);
        const float comp   = ne570Comp(preEmp);
        const float toWrite = biquad(comp + fbSig, bbdInS, bbdIB0, bbdIB1, bbdIB2, bbdIA1, bbdIA2);
        delayBuf[writePos] = fastTanh(toWrite);
        writePos = (writePos + 1) % kMaxDelaySamples;

        // Output mix with equal-power makeup
        const float dryG = 1.0f - blend, wetG = blend;
        const float mkup = 1.0f / std::sqrt(dryG*dryG + wetG*wetG + 1e-12f);
        outL = (inL * dryG + delayed * wetG) * mkup;
        outR = (inR * dryG + delayed * wetG) * mkup;
    }

private:
    struct BqState { float x1=0,x2=0,y1=0,y2=0; };

    float read(float ds) const noexcept
    {
        float rp = static_cast<float>(writePos) - ds;
        if (rp < 0.0f) rp += static_cast<float>(kMaxDelaySamples);
        const int i1 = static_cast<int>(rp) % kMaxDelaySamples;
        const float fr = rp - std::floor(rp);
        return delayBuf[i1] + fr * (delayBuf[(i1+1)%kMaxDelaySamples] - delayBuf[i1]);
    }

    static float biquad(float x, BqState& s,
                         float b0, float b1, float b2, float a1, float a2) noexcept
    {
        float y = b0*x + b1*s.x1 + b2*s.x2 - a1*s.y1 - a2*s.y2;
        s.x2=s.x1; s.x1=x; s.y2=s.y1; s.y1=y;
        return y;
    }

    float ne570Comp(float in) noexcept
    {
        const float x2 = in*in;
        const float c  = (x2 > compEnv) ? compAtt : compRel;
        compEnv += (x2 - compEnv) * c;
        const float lvl = std::sqrt(compEnv + 1e-12f);
        constexpr float thr = 0.25f;
        return (lvl > thr) ? in * std::sqrt(thr / lvl) : in;
    }

    float ne570Exp(float in) noexcept
    {
        const float x2 = in*in;
        const float c  = (x2 > expEnv) ? expAtt : expRel;
        expEnv += (x2 - expEnv) * c;
        const float lvl = std::sqrt(expEnv + 1e-12f);
        constexpr float thr = 0.25f;
        if (lvl <= thr) return in;
        constexpr float maxG = 4.0f;
        float g = std::sqrt(lvl / thr);
        if (g > 1.0f) g = 1.0f + (maxG-1.0f) * fastTanh((g-1.0f)/(maxG-1.0f));
        return in * g;
    }

    float bbdCutoff(float ms) noexcept
    {
        const float t = std::log(fmaxf(20.0f, fminf(550.0f, ms)) / 20.0f) / std::log(27.5f);
        return 8000.0f * std::pow(0.4f, t);
    }

    void updateBBD(float ms) noexcept
    {
        const float fc = bbdCutoff(ms);
        if (std::abs(fc - lastCutoff) < 1.0f) return;
        lastCutoff = fc;
        makeLP(fc, bbdIB0, bbdIB1, bbdIB2, bbdIA1, bbdIA2);
        makeLP(fc, bbdOB0, bbdOB1, bbdOB2, bbdOA1, bbdOA2);
    }

    void makeLP(float fc, float& b0, float& b1, float& b2, float& a1, float& a2) noexcept
    {
        const float w0 = 6.28318f * fc / sr;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float al = sw / 1.414f;
        const float a0i = 1.0f / (1.0f + al);
        b0 = (1.0f - cw) * 0.5f * a0i;
        b1 = (1.0f - cw) * a0i;
        b2 = b0;
        a1 = -2.0f * cw * a0i;
        a2 = (1.0f - al) * a0i;
    }

    void makeHS(float fc, float linGain,
                float& b0, float& b1, float& b2, float& a1, float& a2) noexcept
    {
        const float A  = std::sqrt(linGain);
        const float w0 = 6.28318f * fc / sr;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float al = sw * 0.5f * std::sqrt((A + 1.0f/A) * (1.0f/0.707f - 1.0f) + 2.0f);
        const float sq = 2.0f * std::sqrt(A) * al;
        const float a0 = (A+1.0f) - (A-1.0f)*cw + sq;
        b0 = A*((A+1.0f) + (A-1.0f)*cw + sq) / a0;
        b1 = -2.0f*A*((A-1.0f) + (A+1.0f)*cw) / a0;
        b2 = A*((A+1.0f) + (A-1.0f)*cw - sq) / a0;
        a1 = 2.0f*((A-1.0f) - (A+1.0f)*cw) / a0;
        a2 = ((A+1.0f) - (A-1.0f)*cw - sq) / a0;
    }

    float sr = 48000.0f;
    float* delayBuf = nullptr;
    int    writePos = 0;
    // LFO quadrature oscillator (replaces lfoPhase + sin/cos per sample)
    float  lfoSin = 0.0f, lfoCos = 1.0f;
    float  lfoSinInc = 0.0f, lfoCosInc = 1.0f;
    float  prevChrvib = -99.0f;
    float  smoothDelay = 0.0f;
    float  lastCutoff  = -1.0f;
    float  compEnv = 0.0f, expEnv = 0.0f;
    float  compAtt = 0.0f, compRel = 0.0f;
    float  expAtt  = 0.0f, expRel  = 0.0f;

    float preB0=1,preB1=0,preB2=0,preA1=0,preA2=0;
    float deB0=1, deB1=0, deB2=0, deA1=0, deA2=0;
    float bbdIB0=1,bbdIB1=0,bbdIB2=0,bbdIA1=0,bbdIA2=0;
    float bbdOB0=1,bbdOB1=0,bbdOB2=0,bbdOA1=0,bbdOA2=0;

    BqState preS, deS, bbdInS, bbdOutS;
};
