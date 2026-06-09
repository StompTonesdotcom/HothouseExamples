// Hot Shoegaze — StompTones
//
// Signal chain:
//   IN → [Toggle1: ER2 / Off / Faze9] → [Toggle3 routing] → OUT
//
// Toggle 1 (pre-effect, always first, no footswitch):
//   UP   = Early Reflections 2 (fixed: room=20, predelay=75ms, 100% wet)
//   MID  = Off
//   DOWN = Faze 9 (fixed: ~0.67 Hz, kFeedback=0.45, 50/50 dry+wet)
//
// FS1 + LED1: Moonn Silver fuzz on/off (unity gain, no knob)
//
// Toggle 2 (reverb algo, controlled by FS2 + LED2):
//   UP   = Comet Tail    (K1=sustain  K2=texture  K3=decay K4=mix    K5=tone)
//   MID  = Kid Amnesia   (K1=delay    K2=feedback K3=blend K4=chrvib K5=depth)
//   DOWN = Loveless      (K1=bloom    K2=sway     K3=wash  K4=mix    K5=predelay)
//
// Toggle 3 (chain order; UP position is broken, treat same as MID):
//   UP / MID = Fuzz → Reverb
//   DOWN     = Reverb → Fuzz
//
// K6: Global output volume
//
// Moonn Silver has no dedicated knob — it is unity-gain; hardware verification
// of the kOutputGain constant may be needed on first test.

#include "daisysp.h"
#include "hothouse.h"
#include "../HotHouseMultiFX/effects/early_reflections2.h"
#include "../HotHouseMultiFX/effects/faze9.h"
#include "../HotHouseMultiFX/effects/moonn_silver.h"
#include "../HotHouseMultiFX/effects/comet_tail.h"
#include "../HotHouseMultiFX/effects/kid_amnesia.h"
#include "../HotHouseMultiFX/effects/loveless_reverb.h"

using clevelandmusicco::Hothouse;

// ---------------------------------------------------------------------------
// SDRAM buffers — large allocations live outside the main 512KB SRAM
// ---------------------------------------------------------------------------
float DSY_SDRAM_BSS er2_buf       [EarlyReflections2::kBufMax];
float DSY_SDRAM_BSS comet_combL   [CometTail::kCombTotal];
float DSY_SDRAM_BSS comet_combR   [CometTail::kCombTotal];
float DSY_SDRAM_BSS comet_cL      [CometTail::kChorusLen];
float DSY_SDRAM_BSS comet_cR      [CometTail::kChorusLen];
float DSY_SDRAM_BSS comet_grainBuf[CometTail::kGrainBufLen]; // shimmer
float DSY_SDRAM_BSS amnesia_buf   [KidAmnesia::kMaxDelaySamples];
float DSY_SDRAM_BSS loveless_combL [LovelessReverb::kCombTotalL];
float DSY_SDRAM_BSS loveless_combR [LovelessReverb::kCombTotalR];
float DSY_SDRAM_BSS loveless_apL   [LovelessReverb::kAPTotalL];
float DSY_SDRAM_BSS loveless_apR   [LovelessReverb::kAPTotalR];
float DSY_SDRAM_BSS loveless_swayL [LovelessReverb::kSwayLen];
float DSY_SDRAM_BSS loveless_swayR [LovelessReverb::kSwayLen];

// ---------------------------------------------------------------------------
// Hardware and effect instances
// ---------------------------------------------------------------------------
Hothouse hw;

EarlyReflections2 er2;
Faze9             faze9;
MoonnSilver       moonnSilver;
CometTail         cometTail;
KidAmnesia        kidAmnesia;
LovelessReverb    loveless;

daisy::Led led1, led2;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
bool fuzzOn  = false;
bool reverbOn = false;

float sk1=0.5f, sk2=0.5f, sk3=0.5f, sk4=0.5f, sk5=0.5f, sk6=1.0f;
static constexpr float kSmooth = 0.05f;

// Moonn Silver output trim — hardware-confirmed at 0.35f for unity gain
static constexpr float kSilverTrim = 0.35f;

// CometTail output trim — Schroeder combs at noon sustain (~fb=0.68) amplify
// the reverb to ~2x dry level before mix. Bring it in line with KidAmnesia/LovelessReverb.
// Tune on hardware: increase if CometTail sounds too quiet, decrease if still too loud.
static constexpr float kCometTrim = 0.5f;

// ---------------------------------------------------------------------------
// Helper
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

    if (hw.switches[6].RisingEdge()) fuzzOn  = !fuzzOn;
    if (hw.switches[7].RisingEdge()) reverbOn = !reverbOn;

    sk1 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_1) - sk1);
    sk2 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_2) - sk2);
    sk3 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_3) - sk3);
    sk4 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_4) - sk4);
    sk5 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_5) - sk5);
    sk6 += kSmooth * (hw.GetKnobValue(Hothouse::KNOB_6) - sk6);

    const auto t1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
    const auto t2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
    const auto t3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

    // Assign reverb params based on active algo (Toggle 2)
    if (t2 == Hothouse::TOGGLESWITCH_UP)
    {
        // Comet Tail — shimmer fixed at 0, 5 knobs: K6 is global volume
        cometTail.sustain = sk1;
        cometTail.texture = sk2;
        cometTail.decay   = Map(sk3, 0.5f, 10.0f);
        cometTail.mix     = sk4 * 0.85f;  // cap 85%: prevents cold-buffer silence at 100% wet
        cometTail.tone    = Map(sk5, 0.0f, 100.0f);
        cometTail.shimmer = 0.0f;
    }
    else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
    {
        // Kid Amnesia — full 5 knobs
        kidAmnesia.delay    = Map(sk1, 20.0f, 550.0f);
        kidAmnesia.feedback = Map(sk2, 0.0f, 1.05f);
        kidAmnesia.blend    = sk3;
        kidAmnesia.chrvib   = sk4;
        kidAmnesia.depth    = sk5;
    }
    else
    {
        // Loveless — reverse reverb swell
        loveless.bloom    = Map(sk1, 0.1f, 3.0f);
        loveless.sway     = sk2;
        loveless.wash     = Map(sk3, 200.0f, 20000.0f);
        loveless.mix      = sk4;
        loveless.predelay = Map(sk5, 0.0f, 200.0f);
    }

    const float globalVolume = sk6;

    for (size_t i = 0; i < size; i++)
    {
        float sigL = in[0][i];
        float sigR = in[1][i];

        // Stage 1: Pre-effect (Toggle 1) — always first, no footswitch
        if (t1 == Hothouse::TOGGLESWITCH_UP)
        {
            float erL, erR;
            er2.Process(sigL, sigR, erL, erR);
            sigL = erL; sigR = erR;
        }
        else if (t1 == Hothouse::TOGGLESWITCH_DOWN)
        {
            float fzL, fzR;
            faze9.Process(sigL, sigR, fzL, fzR);
            sigL = fzL; sigR = fzR;
        }

        // Stage 2+3: Fuzz and reverb — order by Toggle 3
        // UP is broken hardware — treat same as MID (fuzz before reverb)
        if (t3 == Hothouse::TOGGLESWITCH_DOWN)
        {
            // Reverb → Fuzz
            if (reverbOn)
            {
                float rL, rR;
                if (t2 == Hothouse::TOGGLESWITCH_UP)
                {
                    cometTail.Process(sigL, sigR, rL, rR);
                    rL *= kCometTrim; rR *= kCometTrim;
                }
                else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
                    kidAmnesia.Process(sigL, sigR, rL, rR);
                else
                    loveless.Process(sigL, sigR, rL, rR);
                sigL = rL; sigR = rR;
            }
            if (fuzzOn)
            {
                moonnSilver.Process(sigL, sigR, sigL, sigR);
                sigL *= kSilverTrim;
                sigR *= kSilverTrim;
            }
        }
        else
        {
            // Fuzz → Reverb (Toggle 3 MID or UP)
            if (fuzzOn)
            {
                moonnSilver.Process(sigL, sigR, sigL, sigR);
                sigL *= kSilverTrim;
                sigR *= kSilverTrim;
            }
            if (reverbOn)
            {
                float rL, rR;
                if (t2 == Hothouse::TOGGLESWITCH_UP)
                {
                    cometTail.Process(sigL, sigR, rL, rR);
                    rL *= kCometTrim; rR *= kCometTrim;
                }
                else if (t2 == Hothouse::TOGGLESWITCH_MIDDLE)
                    kidAmnesia.Process(sigL, sigR, rL, rR);
                else
                    loveless.Process(sigL, sigR, rL, rR);
                sigL = rL; sigR = rR;
            }
        }

        // NaN guard — prevents Cortex-M7 hard fault if any effect diverges
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

    led1.Init(hw.seed.GetPin(22), false);
    led2.Init(hw.seed.GetPin(23), false);

    // ER2 fixed settings: room=20, predelay=75ms, 100% wet
    er2.Init(sr, er2_buf);
    er2.room       = 20.0f;
    er2.preDelayMs = 75.0f;
    er2.mix        = 1.0f;

    faze9.Init(sr);
    moonnSilver.Init(sr);

    // CometTail with shimmer grain buffer
    cometTail.Init(sr, comet_combL, comet_combR, comet_cL, comet_cR, comet_grainBuf);

    kidAmnesia.Init(sr, amnesia_buf);
    loveless.Init(sr, loveless_combL, loveless_combR,
                      loveless_apL,   loveless_apR,
                      loveless_swayL, loveless_swayR);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    uint32_t bothHoldMs = 0;
    uint32_t fs2HoldMs  = 0;

    while (true)
    {
        hw.DelayMs(6);

        led1.Set(fuzzOn  ? 1.0f : 0.0f);
        led2.Set(reverbOn ? 1.0f : 0.0f);
        led1.Update();
        led2.Update();

        // DFU entry: hold both footswitches 2s → DaisyBoot QSPI mode
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
