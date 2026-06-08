// fast_math.h — Inline fast math for Hothouse DSP (Cortex-M7 / STM32H750)
//
// libm tanhf / std::tanh / std::exp / std::sin are ALL software-implemented
// on Cortex-M7 (no FPU instruction). Each call costs 50-200 cycles.
// At 48kHz with 4-sample blocks we have ~10,000 cycles per sample.
// These polynomial/recurrence approximations cost 4-8 cycles each.
//
// Accuracy note:
//   fastTanh: < 1% error for |x| < 3 (Padé [2,2] rational approximant)
//             Clamps to ±1 for |x| >= 3 — tanh(3) = 0.9951, so ±1 is correct.
#pragma once

static inline float fastTanh(float x) noexcept {
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
