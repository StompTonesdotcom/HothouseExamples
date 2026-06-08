/*
  Hot House Multi-FX  —  StompTones  —  stomptones.com

  Signal chain:
    IN → [Toggle1: ER2 / Off / Mini Me] → [FS1: Distortion (Toggle2)] → [FS2: Main effect (Toggle3)] → OUT

  Toggle 1 — pre-distortion (no footswitch):
    UP   = Early Reflections 2 (fixed: reverse, room=20, live=10, 75ms pre-delay, 100% wet)
    MID  = Off
    DOWN = Mini Me Chorus (fixed default settings)

  FS1 + Toggle 2 — Distortion (LED1):
    UP   = Moonn Silver   (fixed, unity gain)
    MID  = Boris Fuzz v2  (fixed max sustain, noon tone, unity gain)
    DOWN = ST-9           (fixed noon settings, unity gain)

  FS2 + Toggle 3 — Main effect (LED2):
    UP   = Comet Tail   (K1=sustain K2=decay K3=texture K4=tone K5=mix)
    MID  = Kid Amnesia  (K1=delay K2=feedback K3=blend K4=chrvib K5=depth)
    DOWN = Void Dweller (K1=drag K2=diffuse K3=reflect K4=dampen K5=mix)

  K6 — Global output volume (all toggle/FS positions)

  DFU entry: hold BOTH footswitches simultaneously for 2s  (standard)
             OR hold FS2 alone for 4s  (single-footswitch alternative)
*/

#include "daisysp.h"
#include "hothouse.h"

#include "effects/early_reflections2.h"
#include "effects/mini_me_chorus.h"
#include "effects/moonn_silver.h"
#include "effects/boris_fuzz.h"
#include "effects/st9.h"
#include "effects/comet_tail.h"
#include "effects/kid_amnesia.h"
#include "effects/void_dweller.h"

using namespace daisysp;
using clevelandmusicco::Hothouse;

// ============================================================================
// SDRAM buffers
// ============================================================================
float DSY_SDRAM_BSS er2_buf       [EarlyReflections2::kBufMax];
float DSY_SDRAM_BSS comet_combL   [CometTail::kCombTotal];
float DSY_SDRAM_BSS comet_combR   [CometTail::kCombTotal];
float DSY_SDRAM_BSS comet_cL      [CometTail::kChorusLen];
float DSY_SDRAM_BSS comet_cR      [CometTail::kChorusLen];
float DSY_SDRAM_BSS amnesia_buf   [KidAmnesia::kMaxDelaySamples];
float DSY_SDRAM_BSS vd_bufL       [VoidDweller::kBufLen];
float DSY_SDRAM_BSS vd_bufR       [VoidDweller::kBufLen];

// ============================================================================
// Hardware + effects
// ============================================================================
Hothouse hw;

EarlyReflections2 er2;
MiniMeChorus      miniMe;

MoonnSilver moonnSilver;
BorisFuzz   borisFuzz;
ST9         st9;

CometTail   cometTail;
KidAmnesia  kidAmnesia;
VoidDweller voidDweller;

// ============================================================================
// State
// ============================================================================
bool distortionOn = false;
bool mainEffectOn = false;

// ============================================================================
// Helpers
// ============================================================================
static float smoothedKnob[6] = {};

static void UpdateKnobs()
{
    constexpr float kAlpha = 0.05f;
    for (int i = 0; i < 6; ++i)
    {
        const float raw = hw.GetKnobValue(static_cast<Hothouse::Knob>(i));
        smoothedKnob[i] += kAlpha * (raw - smoothedKnob[i]);
    }
}

static float Map(float v, float lo, float hi) noexcept { return lo + v * (hi - lo); }

// ============================================================================
// Audio callback
// ============================================================================
void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    hw.ProcessAllControls();

    if (hw.switches[6].RisingEdge()) distortionOn = !distortionOn;
    if (hw.switches[7].RisingEdge()) mainEffectOn = !mainEffectOn;

    const auto t1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
    const auto t2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
    const auto t3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

    UpdateKnobs();
    const float k1=smoothedKnob[0], k2=smoothedKnob[1], k3=smoothedKnob[2];
    const float k4=smoothedKnob[3], k5=smoothedKnob[4], k6=smoothedKnob[5];

    // K6 = global output volume (0–1, applied to all output)
    const float globalVolume = k6;

    if (t3 == Hothouse::TOGGLESWITCH_UP)
    {
        // Comet Tail — shimmer removed, 5 knobs: K6 is global volume
        cometTail.sustain = k1;
        cometTail.decay   = Map(k2, 0.5f, 10.0f);
        cometTail.texture = k3;
        cometTail.tone    = Map(k4, 0.0f, 100.0f);
        cometTail.mix     = k5 * 0.85f;  // cap at 85% wet — always some dry signal
    }
    else if (t3 == Hothouse::TOGGLESWITCH_MIDDLE)
    {
        // Kid Amnesia — 5 knobs: K6 is global volume
        kidAmnesia.delay    = Map(k1, 20.0f, 350.0f);  // noon=185ms — obvious echo, distinct from reverb
        kidAmnesia.feedback = Map(k2, 0.0f, 1.05f);
        kidAmnesia.blend    = k3;
        kidAmnesia.chrvib   = k4;
        kidAmnesia.depth    = k5;
    }
    else
    {
        // Void Dweller — 5 knobs: length fixed at 0.5 (1s decay), K6 is global volume
        voidDweller.drag    = Map(k1, 10.0f, 200.0f);
        voidDweller.diffuse = k2;
        voidDweller.reflect = Map(k3, 0.0f, 0.95f);
        voidDweller.dampen  = Map(k4, 300.0f, 8000.0f);
        voidDweller.mix     = k5 * 0.85f; // cap at 85% wet
        voidDweller.length  = 0.5f; // fixed — K6 is global volume
    }

    for (size_t i = 0; i < size; i++)
    {
        float sigL = in[0][i];
        float sigR = in[1][i];

        // Stage 1: Pre-distortion (Toggle 1, always in chain)
        if (t1 == Hothouse::TOGGLESWITCH_UP)
        {
            float erL, erR;
            er2.Process(sigL, sigR, erL, erR);
            sigL = erL; sigR = erR;
        }
        else if (t1 == Hothouse::TOGGLESWITCH_DOWN)
        {
            float cL, cR;
            miniMe.Process(sigL, sigR, cL, cR);
            sigL = cL; sigR = cR;
        }

        // Stage 2: Distortion (FS1)
        if (distortionOn)
        {
            if (t2 == Hothouse::TOGGLESWITCH_UP)
            {
                moonnSilver.Process(sigL, sigR, sigL, sigR);
            }
            else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
            {
                sigL = borisFuzz.Process(sigL, 0);
                sigR = borisFuzz.Process(sigR, 1);
            }
            else
            {
                sigL = st9.Process(sigL, 0);
                sigR = st9.Process(sigR, 1);
            }
        }

        // Stage 3: Main effect (FS2)
        if (mainEffectOn)
        {
            float efL, efR;
            if (t3 == Hothouse::TOGGLESWITCH_UP)
                cometTail.Process(sigL, sigR, efL, efR);
            else if (t3 == Hothouse::TOGGLESWITCH_MIDDLE)
                kidAmnesia.Process(sigL, sigR, efL, efR);
            else
                voidDweller.Process(sigL, sigR, efL, efR);
            sigL = efL; sigR = efR;
        }

        // K6 global output volume — NaN guard prevents hard fault if any effect blows up
        const float outL = sigL * globalVolume;
        const float outR = sigR * globalVolume;
        out[0][i] = std::isfinite(outL) ? outL : 0.0f;
        out[1][i] = std::isfinite(outR) ? outR : 0.0f;
    }
}

// ============================================================================
// Main
// ============================================================================
int main()
{
    hw.Init();
    hw.SetAudioBlockSize(4);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    const float sr = hw.AudioSampleRate();

    daisy::Led led1, led2;
    led1.Init(hw.seed.GetPin(22), false);
    led2.Init(hw.seed.GetPin(23), false);

    er2.Init(sr, er2_buf);
    miniMe.Init(sr);
    moonnSilver.Init(sr);
    borisFuzz.Init(sr);
    st9.Init(sr);
    cometTail.Init(sr, comet_combL, comet_combR, comet_cL, comet_cR);
    kidAmnesia.Init(sr, amnesia_buf);
    voidDweller.Init(sr, vd_bufL, vd_bufR);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    uint32_t bothHoldMs = 0; // dual-footswitch DFU timer
    uint32_t fs2HoldMs  = 0; // single-footswitch DFU timer

    while (true)
    {
        hw.DelayMs(6);

        led1.Set(distortionOn ? 1.0f : 0.0f);
        led2.Set(mainEffectOn ? 1.0f : 0.0f);
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
            // FS2 alone for 4s — single-footswitch alternative
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
