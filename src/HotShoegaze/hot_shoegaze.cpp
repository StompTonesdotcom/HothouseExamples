// Hot Shoegaze — StompTones multi-FX shoegaze profile
//
// Signal chain:
//   Toggle 1 (UP/MID/DOWN): Boris Fuzz / Moonn Silver / ST-9
//   Toggle 2 (UP/MID/DOWN): CometTail "Infinite" / ER2 "Swell" / VoidDweller "Abyss"
//   Toggle 3 (UP/MID/DOWN): Fuzz→Reverb (serial) / Parallel / Reverb→Fuzz (serial)
//   FS1 + LED1: Fuzz on/off
//   FS2 + LED2: Reverb on/off
//   K1: Fuzz Gain   K2: Fuzz Tone   K3: Reverb Mix
//   K4: Reverb Shape (Decay / Pre-delay / Reflect)
//   K5: Reverb Color (Texture / Room / Dampen)
//   K6: Global Output Volume

#include "daisysp.h"
#include "hothouse.h"
#include "../HotHouseMultiFX/effects/boris_fuzz.h"
#include "../HotHouseMultiFX/effects/moonn_silver.h"
#include "../HotHouseMultiFX/effects/st9.h"
#include "../HotHouseMultiFX/effects/comet_tail.h"
#include "../HotHouseMultiFX/effects/early_reflections2.h"
#include "../HotHouseMultiFX/effects/void_dweller.h"
#include "../HotHouseMultiFX/effects/fast_math.h"

using clevelandmusicco::Hothouse;

// ---------------------------------------------------------------------------
// SDRAM buffers — large allocations live outside the main 512KB SRAM
// ---------------------------------------------------------------------------
float DSY_SDRAM_BSS er2_buf      [EarlyReflections2::kBufMax];        // ~256 KB
float DSY_SDRAM_BSS comet_combL  [CometTail::kCombTotal];             // ~24 KB
float DSY_SDRAM_BSS comet_combR  [CometTail::kCombTotal];
float DSY_SDRAM_BSS comet_cL     [CometTail::kChorusLen];             // ~6 KB
float DSY_SDRAM_BSS comet_cR     [CometTail::kChorusLen];
float DSY_SDRAM_BSS vd_bufL      [VoidDweller::kBufLen];              // ~416 KB each
float DSY_SDRAM_BSS vd_bufR      [VoidDweller::kBufLen];

// ---------------------------------------------------------------------------
// Hardware and effect instances
// ---------------------------------------------------------------------------
Hothouse hw;

BorisFuzz   borisFuzz;
MoonnSilver moonnSilver;
ST9         st9;
CometTail   cometTail;
EarlyReflections2 er2;
VoidDweller voidDweller;

daisy::Led led1, led2;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
bool fuzzOn  = false;
bool reverbOn = false;

// Tone LP filter: 1-pole, applied post-fuzz regardless of which algo
float toneAlpha  = 1.0f;   // coefficient (recomputed when K2 changes)
float toneStateL = 0.0f;
float toneStateR = 0.0f;
float prevToneK  = -1.0f;  // cached K2 to avoid recomputing every block

// Smoothed knob values
float k1=0.5f, k2=0.5f, k3=0.5f, k4=0.5f, k5=0.5f, k6=1.0f;
static constexpr float kSmooth = 0.05f;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline float Map(float x, float lo, float hi) { return lo + x * (hi - lo); }

// ---------------------------------------------------------------------------
// Audio callback
// ---------------------------------------------------------------------------
void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    hw.ProcessAllControls();

    // Footswitch latching — toggle on rising edge (button release)
    if (hw.switches[6].RisingEdge()) fuzzOn  = !fuzzOn;
    if (hw.switches[7].RisingEdge()) reverbOn = !reverbOn;

    // Smooth knobs
    k1 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_1) - k1);
    k2 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_2) - k2);
    k3 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_3) - k3);
    k4 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_4) - k4);
    k5 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_5) - k5);
    k6 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_6) - k6);

    // Read toggles
    const auto t1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
    const auto t2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
    const auto t3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

    // K1 → fuzz intensity (pre-gain for Boris/Moonn Silver; drive curve for ST-9)
    // K2 → tone: BMP passive tone stack (Boris), TS-9 tone stack (ST-9),
    //            generic LP 500-8kHz (Moonn Silver — no tone knob in original)

    // Boris Fuzz: K1=gain (0.5–4.0 pre-gain), K2=BMP tone (0=treble, 0.5=mid-scoop, 1=bass)
    borisFuzz.gain = Map(k1, 0.5f, 4.0f);
    borisFuzz.tone = k2;

    // Moonn Silver: K1=gain (0.5–4.0 pre-gain), K2=synthetic LP tone (not in original plugin)
    moonnSilver.gain = Map(k1, 0.5f, 4.0f);
    if (std::abs(k2 - prevToneK) > 0.005f)
    {
        const float fc = Map(k2, 500.0f, 8000.0f);
        toneAlpha = 1.0f - expf(-6.28318f * fc / hw.AudioSampleRate());
        prevToneK = k2;
    }

    // ST-9: K1=drive (0–1 TS-9 drive curve), K2=TS-9 passive tone stack (0–1)
    st9.drive = k1;
    st9.tone  = k2;

    // Reverb mix capped at 85% — prevents silent-cold-buffer issue at 100% wet
    const float reverbMix = k3 * 0.85f;

    // Assign reverb params based on Toggle 2
    if (t2 == Hothouse::TOGGLESWITCH_UP)
    {
        // CometTail "Infinite" — K4=decay(0.5-10s), K5=texture(0-1)
        cometTail.decay   = Map(k4, 0.5f, 10.0f);
        cometTail.texture = k5;
        cometTail.sustain = 0.6f;  // fixed moderate sustain for shoegaze wash
        cometTail.tone    = 40.0f; // slightly dark
        cometTail.mix     = reverbMix;
    }
    else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
    {
        // ER2 "Swell" — K4=pre-delay(0-200ms), K5=room(5-30)
        er2.preDelayMs = Map(k4, 0.0f, 200.0f);
        er2.room       = Map(k5, 5.0f, 30.0f);
        er2.mix        = reverbMix;
    }
    else
    {
        // VoidDweller "Abyss" — K4=reflect(0-0.95), K5=dampen(300-8000Hz)
        voidDweller.reflect = Map(k4, 0.0f, 0.95f);
        voidDweller.dampen  = Map(k5, 300.0f, 8000.0f);
        voidDweller.drag    = 105.0f;   // fixed mid-range tap spacing
        voidDweller.diffuse = 0.5f;     // fixed moderate diffusion
        voidDweller.length  = 0.5f;     // fixed decay (~3s)
        voidDweller.mix     = reverbMix;
    }

    const float globalVolume = k6;

    for (size_t i = 0; i < size; i++)
    {
        const float inL = in[0][i];
        const float inR = in[1][i];

        float sigL, sigR;

        if (t3 == Hothouse::TOGGLESWITCH_UP)
        {
            // Serial: Fuzz → Reverb
            float fL = inL, fR = inR;

            if (fuzzOn)
            {
                if (t1 == Hothouse::TOGGLESWITCH_UP)
                {
                    fL = borisFuzz.Process(fL, 0);
                    fR = borisFuzz.Process(fR, 1);
                }
                else if (t1 == Hothouse::TOGGLESWITCH_MIDDLE)
                {
                    const float m = moonnSilver.Process(fL);
                    fL = fR = m;
                    // Synthetic LP tone (K2) — Moonn Silver has no tone knob in original
                    toneStateL += toneAlpha * (fL - toneStateL); fL = toneStateL;
                    toneStateR += toneAlpha * (fR - toneStateR); fR = toneStateR;
                }
                else
                {
                    fL = st9.Process(fL, 0);
                    fR = st9.Process(fR, 1);
                }
            }

            if (reverbOn)
            {
                float rL, rR;
                if (t2 == Hothouse::TOGGLESWITCH_UP)
                    cometTail.Process(fL, fR, rL, rR);
                else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
                    er2.Process(fL, fR, rL, rR);
                else
                    voidDweller.Process(fL, fR, rL, rR);
                sigL = rL; sigR = rR;
            }
            else
            {
                sigL = fL; sigR = fR;
            }
        }
        else if (t3 == Hothouse::TOGGLESWITCH_DOWN)
        {
            // Serial: Reverb → Fuzz
            float rL = inL, rR = inR;

            if (reverbOn)
            {
                if (t2 == Hothouse::TOGGLESWITCH_UP)
                    cometTail.Process(inL, inR, rL, rR);
                else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
                    er2.Process(inL, inR, rL, rR);
                else
                    voidDweller.Process(inL, inR, rL, rR);
            }

            if (fuzzOn)
            {
                if (t1 == Hothouse::TOGGLESWITCH_UP)
                {
                    rL = borisFuzz.Process(rL, 0);
                    rR = borisFuzz.Process(rR, 1);
                }
                else if (t1 == Hothouse::TOGGLESWITCH_MIDDLE)
                {
                    const float m = moonnSilver.Process(rL);
                    rL = rR = m;
                    // Synthetic LP tone (K2) — Moonn Silver has no tone knob in original
                    toneStateL += toneAlpha * (rL - toneStateL); rL = toneStateL;
                    toneStateR += toneAlpha * (rR - toneStateR); rR = toneStateR;
                }
                else
                {
                    rL = st9.Process(rL, 0);
                    rR = st9.Process(rR, 1);
                }
            }

            sigL = rL; sigR = rR;
        }
        else
        {
            // Parallel: fuzz and reverb both process dry signal, blend 50/50
            float fL = inL, fR = inR;
            float rL = inL, rR = inR;

            if (fuzzOn)
            {
                if (t1 == Hothouse::TOGGLESWITCH_UP)
                {
                    fL = borisFuzz.Process(fL, 0);
                    fR = borisFuzz.Process(fR, 1);
                }
                else if (t1 == Hothouse::TOGGLESWITCH_MIDDLE)
                {
                    const float m = moonnSilver.Process(fL);
                    fL = fR = m;
                    // Synthetic LP tone (K2) — Moonn Silver has no tone knob in original
                    toneStateL += toneAlpha * (fL - toneStateL); fL = toneStateL;
                    toneStateR += toneAlpha * (fR - toneStateR); fR = toneStateR;
                }
                else
                {
                    fL = st9.Process(fL, 0);
                    fR = st9.Process(fR, 1);
                }
            }

            if (reverbOn)
            {
                if (t2 == Hothouse::TOGGLESWITCH_UP)
                    cometTail.Process(inL, inR, rL, rR);
                else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
                    er2.Process(inL, inR, rL, rR);
                else
                    voidDweller.Process(inL, inR, rL, rR);
            }

            // 50/50 blend prevents double-loudness; when one is bypassed its
            // branch stays dry, so the blend is still well-behaved
            sigL = (fL + rL) * 0.5f;
            sigR = (fR + rR) * 0.5f;
        }

        // NaN guard — prevents hard fault if any effect produces inf/NaN
        const float outL = sigL * globalVolume;
        const float outR = sigR * globalVolume;
        out[0][i] = std::isfinite(outL) ? outL : 0.0f;
        out[1][i] = std::isfinite(outR) ? outR : 0.0f;
    }
}


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    hw.Init();
    hw.SetAudioBlockSize(4);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    const float sr = hw.AudioSampleRate();

    // LEDs: LED1 = pin 22 (left footswitch), LED2 = pin 23 (right footswitch)
    led1.Init(hw.seed.GetPin(22), false);
    led2.Init(hw.seed.GetPin(23), false);

    // Init effects
    borisFuzz.Init(sr);
    moonnSilver.Init(sr);
    st9.Init(sr);
    cometTail.Init(sr, comet_combL, comet_combR, comet_cL, comet_cR);
    er2.Init(sr, er2_buf);
    voidDweller.Init(sr, vd_bufL, vd_bufR);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    uint32_t bothHoldMs = 0;
    uint32_t fs2HoldMs  = 0;

    while (true)
    {
        hw.DelayMs(6);

        // Update LEDs to reflect on/off state
        led1.Set(fuzzOn  ? 1.0f : 0.0f);
        led2.Set(reverbOn ? 1.0f : 0.0f);
        led1.Update();
        led2.Update();

        // DFU entry: both footswitches held 2s → DaisyBoot QSPI mode
        const bool fs1 = hw.switches[6].Pressed();
        const bool fs2 = hw.switches[7].Pressed();

        if (fs1 && fs2)
        {
            bothHoldMs += 6;
            fs2HoldMs = 0;
            if (bothHoldMs >= 2000)
                daisy::System::ResetToBootloader(
                    daisy::System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
        }
        else if (fs2 && !fs1)
        {
            fs2HoldMs += 6;
            bothHoldMs = 0;
            if (fs2HoldMs >= 4000)
                daisy::System::ResetToBootloader(
                    daisy::System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
        }
        else
        {
            bothHoldMs = 0;
            fs2HoldMs  = 0;
        }
    }
}
