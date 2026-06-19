/*
  Hot House Multi-FX  —  StompTones  —  stomptones.com

  Signal chain (Toggle 3 controls order):
    T3 UP/MID: IN → [FS1: Toggle1 effect] → [FS2: Toggle2 effect] → K1 vol → OUT
    T3 DOWN:   IN → [FS2: Toggle2 effect] → [FS1: Toggle1 effect] → K1 vol → OUT

  Toggle 1 (FS1 on/off, LED1) — fixed settings, no knob controls:
    UP   = Early Reflections 2 (reverse, room=20, predelay=75ms, 100% wet)
    MID  = Mini Me Chorus (default settings)
    DOWN = Faze 9 (fixed noon speed ~0.69 Hz)

  Toggle 2 (FS2 on/off, LED2) — K2–K6 control parameters:
    UP   = Loveless Reverse Reverb (K2=bloom K3=sway K4=wash K5=mix K6=predelay)
    MID  = Comet Tail / shimmer off (K2=sustain K3=decay K4=texture K5=tone K6=mix)
    DOWN = Kid Amnesia (K2=delay K3=feedback K4=blend K5=chrvib K6=depth)

  K1 = Global output volume (all positions)

  DFU entry: hold BOTH footswitches simultaneously for 2s
             OR hold FS2 alone for 4s
*/

#include "daisysp.h"
#include "hothouse.h"

#include "effects/early_reflections2.h"
#include "effects/mini_me_chorus.h"
#include "effects/faze9.h"
#include "effects/loveless_reverb.h"
#include "effects/comet_tail.h"
#include "effects/kid_amnesia.h"

using namespace daisysp;
using clevelandmusicco::Hothouse;

// ============================================================================
// SDRAM buffers
// ============================================================================
float DSY_SDRAM_BSS er2_buf            [EarlyReflections2::kBufMax];

float DSY_SDRAM_BSS loveless_combL     [LovelessReverb::kCombTotalL];
float DSY_SDRAM_BSS loveless_combR     [LovelessReverb::kCombTotalR];
float DSY_SDRAM_BSS loveless_apL       [LovelessReverb::kAPTotalL];
float DSY_SDRAM_BSS loveless_apR       [LovelessReverb::kAPTotalR];
float DSY_SDRAM_BSS loveless_swayL     [LovelessReverb::kSwayLen];
float DSY_SDRAM_BSS loveless_swayR     [LovelessReverb::kSwayLen];

float DSY_SDRAM_BSS comet_combL        [CometTail::kCombBufMax];
float DSY_SDRAM_BSS comet_combR        [CometTail::kCombBufMax];
float DSY_SDRAM_BSS comet_cL           [CometTail::kChorusLen];
float DSY_SDRAM_BSS comet_cR           [CometTail::kChorusLen];

float DSY_SDRAM_BSS amnesia_buf        [KidAmnesia::kMaxDelaySamples];

// ============================================================================
// Hardware + effects
// ============================================================================
Hothouse hw;

EarlyReflections2 er2;
MiniMeChorus      miniMe;
Faze9             faze9;

LovelessReverb loveless;
CometTail      cometTail;
KidAmnesia     kidAmnesia;

// ============================================================================
// State
// ============================================================================
bool effect1On = false; // FS1 / Toggle 1 effects
bool effect2On = false; // FS2 / Toggle 2 effects

float eff2FadeGain   = 1.0f;
int   eff2FadeRemain = 0;
static constexpr int kFadeSamples = 14400; // 300ms @ 48kHz

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

    if (hw.switches[6].RisingEdge()) effect1On = !effect1On;
    if (hw.switches[7].RisingEdge()) {
        effect2On = !effect2On;
        if (effect2On) { eff2FadeGain = 0.0f; eff2FadeRemain = kFadeSamples; }
    }

    const auto t1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
    const auto t2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
    const auto t3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

    // Toggle 3 DOWN = reversed chain; UP and MID both = normal order
    const bool chainReversed = (t3 == Hothouse::TOGGLESWITCH_DOWN);

    UpdateKnobs();
    const float k1 = smoothedKnob[0]; // global volume
    const float k2 = smoothedKnob[1];
    const float k3 = smoothedKnob[2];
    const float k4 = smoothedKnob[3];
    const float k5 = smoothedKnob[4];
    const float k6 = smoothedKnob[5];

    // Update Toggle 2 effect parameters (K2–K6)
    if (t2 == Hothouse::TOGGLESWITCH_UP)
    {
        // Loveless Reverse Reverb
        // K2=bloom(0.1–3s) K3=sway(0–1) K4=wash(200–20kHz) K5=mix K6=predelay(0–200ms)
        loveless.bloom    = Map(k2, 0.1f, 3.0f);
        loveless.sway     = k3;
        loveless.wash     = Map(k4, 200.0f, 20000.0f);
        loveless.mix      = k5;
        loveless.predelay = k6 * 200.0f;
    }
    else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
    {
        // Comet Tail — shimmer fixed at 0 (5 knobs)
        // K2=sustain(0–1) K3=decay(0.5–10s) K4=texture(0–1) K5=tone(0–100) K6=mix
        cometTail.shimmer = 0.0f;
        cometTail.sustain = k2;
        cometTail.decay   = Map(k3, 0.5f, 10.0f);
        cometTail.texture = k4;
        cometTail.tone    = Map(k5, 0.0f, 100.0f);
        cometTail.mix     = k6;
    }
    else
    {
        // Kid Amnesia — matches JUCE Amnesia plugin parameter ranges
        // K2=delay(20–550ms) K3=feedback(0–1.05) K4=blend(0–1) K5=chrvib(0–1) K6=depth(0–1)
        kidAmnesia.delay    = Map(k2, 20.0f, 550.0f);
        kidAmnesia.feedback = Map(k3, 0.0f, 1.05f);
        kidAmnesia.blend    = k4;
        kidAmnesia.chrvib   = k5;
        kidAmnesia.depth    = k6;
    }

    for (size_t i = 0; i < size; i++)
    {
        float sigL = in[0][i];
        float sigR = in[1][i];

        // Toggle 1 effect block (applied when FS1 is on)
        auto applyToggle1 = [&]() {
            if (!effect1On) return;
            if (t1 == Hothouse::TOGGLESWITCH_UP)
            {
                float eL, eR;
                er2.Process(sigL, sigR, eL, eR);
                sigL = eL; sigR = eR;
            }
            else if (t1 == Hothouse::TOGGLESWITCH_MIDDLE)
            {
                float eL, eR;
                miniMe.Process(sigL, sigR, eL, eR);
                sigL = eL; sigR = eR;
            }
            else
            {
                float eL, eR;
                faze9.Process(sigL, sigR, eL, eR);
                sigL = eL; sigR = eR;
            }
        };

        // Toggle 2 effect block (applied when FS2 is on)
        auto applyToggle2 = [&]() {
            if (!effect2On) return;
            const float dryL = sigL, dryR = sigR;
            float eL, eR;
            if (t2 == Hothouse::TOGGLESWITCH_UP)
                loveless.Process(sigL, sigR, eL, eR);
            else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
                cometTail.Process(sigL, sigR, eL, eR);
            else
                kidAmnesia.Process(sigL, sigR, eL, eR);
            if (eff2FadeRemain > 0) {
                const float g = eff2FadeGain;
                sigL = dryL * (1.0f - g) + eL * g;
                sigR = dryR * (1.0f - g) + eR * g;
                eff2FadeGain += 1.0f / kFadeSamples;
                if (eff2FadeGain > 1.0f) eff2FadeGain = 1.0f;
                --eff2FadeRemain;
            } else {
                sigL = eL; sigR = eR;
            }
        };

        if (!chainReversed)
        {
            applyToggle1();
            applyToggle2();
        }
        else
        {
            applyToggle2();
            applyToggle1();
        }

        // K1 global output volume — NaN guard prevents hard fault from effect blow-up
        const float outL = sigL * k1;
        const float outR = sigR * k1;
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
    faze9.Init(sr);
    loveless.Init(sr,
                  loveless_combL, loveless_combR,
                  loveless_apL,   loveless_apR,
                  loveless_swayL, loveless_swayR);
    cometTail.Init(sr, comet_combL, comet_combR, comet_cL, comet_cR);
    kidAmnesia.Init(sr, amnesia_buf);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    uint32_t bothHoldMs = 0;
    uint32_t fs2HoldMs  = 0;

    while (true)
    {
        hw.DelayMs(6);

        led1.Set(effect1On ? 1.0f : 0.0f);
        led2.Set(effect2On ? 1.0f : 0.0f);
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
