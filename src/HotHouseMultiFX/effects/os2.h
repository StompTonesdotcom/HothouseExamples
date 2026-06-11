// os2.h — 2x and 4x polyphase IIR halfband oversamplers
//
// OS2: Orfanidis 4th-order halfband design, two first-order allpass branches.
// Stopband attenuation ≈ 60 dB above 0.25*Fs.  CPU cost: ~8 mul-adds/sample.
//
// OS2 usage:
//   OS2 os;  os.reset();
//   float u0, u1;
//   os.up(x, u0, u1);       // one input → two 2x-rate samples
//   // process u0, u1 at 2x rate
//   float y = os.dn(u0, u1); // two 2x-rate samples → one output
//
// OS4: two cascaded OS2 stages, 4x oversampling.  CPU cost: ~48 mul-adds/sample.
//   OS4 os;  os.reset();
//   float u0,u1,u2,u3;
//   os.up(x, u0,u1,u2,u3);        // one input → four 4x-rate samples
//   // process u0..u3 at 4x rate
//   float y = os.dn(u0,u1,u2,u3); // four 4x-rate samples → one output

#pragma once

struct OS2
{
    static constexpr float kA0 = 0.107595f;
    static constexpr float kA1 = 0.536920f;

    float upS0 = 0.0f, upS1 = 0.0f, upPrev1 = 0.0f;
    float dnS0 = 0.0f, dnS1 = 0.0f;

    void reset() noexcept { upS0 = upS1 = upPrev1 = dnS0 = dnS1 = 0.0f; }

    void up(float x, float& y0, float& y1) noexcept
    {
        y0      = ap(x, kA0, upS0);
        y1      = upPrev1;
        upPrev1 = ap(x, kA1, upS1);
    }

    float dn(float y0, float y1) noexcept
    {
        return 0.5f * (ap(y0, kA0, dnS0) + ap(y1, kA1, dnS1));
    }

private:
    static float ap(float x, float a, float& s) noexcept
    {
        float y = a * x + s;
        s = x - a * y;
        return y;
    }
};

struct OS4
{
    OS2 up1, up2a, up2b;
    OS2 dn2a, dn2b, dn1;

    void reset() noexcept
    {
        up1.reset(); up2a.reset(); up2b.reset();
        dn2a.reset(); dn2b.reset(); dn1.reset();
    }

    // Upsample: x (1x) → y0..y3 (4x)
    void up(float x, float& y0, float& y1, float& y2, float& y3) noexcept
    {
        float a0, a1;
        up1.up(x, a0, a1);
        up2a.up(a0, y0, y1);
        up2b.up(a1, y2, y3);
    }

    // Downsample: y0..y3 (4x) → one output (1x)
    float dn(float y0, float y1, float y2, float y3) noexcept
    {
        return dn1.dn(dn2a.dn(y0, y1), dn2b.dn(y2, y3));
    }
};
