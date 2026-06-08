# Hothouse DIY Pedal — Claude Context

This repo is HothouseExamples cloned from https://github.com/clevelandmusicco/HothouseExamples.
The goal is to build and flash custom DSP effects for the Cleveland Music Co. Hothouse pedal (Daisy Seed based).

## Hardware

**Hothouse controls (from `src/hothouse.h`):**
- 6 knobs: `KNOB_1` through `KNOB_6` — read via `hw.GetKnobValue(Hothouse::KNOB_1)` or `hw.knobs[0].Process()` (returns 0.0–1.0)
- 3 toggle switches: `TOGGLESWITCH_1/2/3` — read via `hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1)` returns `TOGGLESWITCH_UP`, `TOGGLESWITCH_MIDDLE`, or `TOGGLESWITCH_DOWN`
- 2 footswitches: `FOOTSWITCH_1` (left), `FOOTSWITCH_2` (right) — read via `hw.switches[6]` / `hw.switches[7]`, or use `RegisterFootswitchCallbacks()` for single/double/long press handling
- 2 LEDs: `LED_1` (pin 22), `LED_2` (pin 23) — initialized with `led.Init(hw.seed.GetPin(23), false)`
- Audio: stereo in/out, 48kHz sample rate, 4-sample block size is typical

**DFU mode (to flash):**
- First time: hold BOOT button on Daisy Seed + press RESET
- After first flash: hold both footswitches simultaneously for 2 seconds (LEDs flash 3x then reset) — this is the `CheckResetToBootloader()` call in the main loop

## Dev Environment (macOS arm64)

**ARM toolchain:** `~/arm-gnu-toolchain/bin/arm-none-eabi-gcc` (v15.2, downloaded directly from ARM — no Homebrew needed)
- Added to PATH in `~/.zshrc`: `export PATH="$HOME/arm-gnu-toolchain/bin:$PATH"`
- Open a fresh terminal after setup and `arm-none-eabi-gcc --version` should work

**dfu-util:** `/opt/homebrew/bin/dfu-util` (v0.11, installed via Homebrew)
- Homebrew at `/opt/homebrew/` (Apple Silicon), PATH set via `~/.zprofile`

**Libraries:** `libDaisy` and `DaisySP` are git submodules — already built. If you need to rebuild:
```bash
make -C libDaisy
make -C DaisySP
```

## Build & Flash Workflow

```bash
# Build an example
cd src/BasicTremolo
make clean && make

# Flash via USB DFU
make program-dfu
```

The `make program-dfu` error `dfu-util: Error during download get_status` is a **known false positive** — it just means the Daisy rebooted into the new firmware before dfu-util could get a final status. If you see "File downloaded successfully" before it, the flash worked.

## Code Pattern (every effect follows this structure)

```cpp
#include "daisysp.h"
#include "hothouse.h"
using clevelandmusicco::Hothouse;

Hothouse hw;

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    hw.ProcessAllControls();

    // read knobs, switches, footswitches here
    float knob1 = hw.GetKnobValue(Hothouse::KNOB_1); // 0.0 - 1.0

    for (size_t i = 0; i < size; i++) {
        out[0][i] = out[1][i] = /* processed audio */;
    }
}

int main() {
    hw.Init();
    hw.SetAudioBlockSize(4);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    float sample_rate = hw.AudioSampleRate();

    // init DSP objects with sample_rate here

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    while (true) {
        hw.DelayMs(6);
        // update LEDs here
        hw.CheckResetToBootloader(); // always include this
    }
}
```

To create a new effect, copy an existing example dir and modify. The `Makefile` in each example is nearly identical — just change the target name.

## Gotchas & Hard-Won Lessons

- **Charge-only micro-USB cables will not work for flashing.** The Daisy won't appear in `dfu-util -l` or even in the macOS USB device list. Use a known data cable (e.g. Xbox controller cable works).
- **The DFU flash "error" is not an error.** `dfu-util: Error during download get_status` at the end of a successful flash is expected — ignore it, check for "File downloaded successfully" instead.
- **Togglswitches:** If a switch is ON-ON (not ON-OFF-ON), `TOGGLESWITCH_MIDDLE` can never be returned. Write code defensively around this.
- **BasicTremolo uses FOOTSWITCH_2 (right), not FOOTSWITCH_1 (left).** `hw.switches[7]` = right footswitch, `hw.seed.GetPin(23)` = right LED. Left footswitch does nothing in that example. This is a quirk of that demo — not a hardware issue.
- **`hw.knobs[n].Process()` vs `hw.GetKnobValue()`:** Both return 0.0–1.0. `Process()` called directly on the AnalogControl object. `GetKnobValue()` is the cleaner API — prefer it in new code.
- **`hw.ProcessAllControls()`** must be called at the top of the audio callback every time, not just once. It reads ADC + debounces switches.
- **`CheckResetToBootloader()`** must be in the main loop (not the audio callback). Omitting it means you can't enter DFU mode via footswitches and must open the pedal.

## Custom Projects

### HotHouseMultiFX (`src/HotHouseMultiFX/`)

StompTones multi-FX pedal — 8 effects ported from JUCE plugins. Uses `APP_TYPE = BOOT_QSPI` (code in 8MB QSPI flash, not 128KB internal flash).

**Signal chain:** Toggle1 (pre-dist) → FS1 Distortion (Toggle2) → FS2 Main effect (Toggle3)

| Control | Function |
|---|---|
| Toggle 1 UP | Early Reflections 2 (fixed: reverse, room=20, live=10, 75ms pre-delay) |
| Toggle 1 MID | Off |
| Toggle 1 DOWN | Mini Me Chorus (fixed default settings) |
| FS1 + LED1 | Distortion on/off |
| Toggle 2 UP/MID/DOWN | Moonn Silver / Boris Fuzz v2 / ST-9 (all fixed, unity gain) |
| FS2 + LED2 | Main effect on/off |
| Toggle 3 UP | Comet Tail — K1=sustain K2=decay K3=texture K4=tone K5=mix |
| Toggle 3 MID | Kid Amnesia — K1=delay K2=feedback K3=blend K4=chrvib K5=depth |
| Toggle 3 DOWN | Void Dweller — K1=drag K2=diffuse K3=reflect K4=dampen K5=mix (length fixed 0.5) |
| K6 (all positions) | Global output volume (0=silence → 1=unity) |

**Memory:** 146KB QSPI / 69KB SRAM / 1.6MB SDRAM

**Flashing:** Requires the Electrosmith Daisy bootloader on the Seed (NOT raw DFU to internal flash). The `APP_TYPE = BOOT_QSPI` Makefile flag selects the QSPI linker script.

**QSPI flash procedure (Hothouse runs on DC power — USB alone won't reset it):**
1. Power off the pedal completely: remove DC plug AND USB
2. Run this command (it will wait for the device):
   ```bash
   dfu-util -w -d ,0483:df11 -a 0 -s 0x90040000:leave -D build/hot_house_multi_fx.bin
   ```
3. Plug in USB only (no DC) — DaisyBoot presents QSPI DFU in a 2-second window
4. dfu-util connects and flashes automatically
5. "File downloaded successfully" = success. The "get_status" error at the end is a known false positive.

**Alternative DFU entry from within the firmware:**
- Hold both footswitches for 2 seconds (standard) — LEDs flash and device resets to bootloader
- Hold FS2 alone for 4 seconds (single-footswitch alternative)
- Then run step 2 above and plug in USB only

**Large buffers in SDRAM:** Always declare large buffers `DSY_SDRAM_BSS`. All delay/chorus buffers are external SDRAM pointers passed into each effect's `Init()`.

**CPU optimization notes:** libm tanhf / std::tanh / std::exp / std::sin are software-implemented on Cortex-M7 (~50-200 cycles each, no FPU instruction). Replace with:
- `fastTanh(x)` from `effects/fast_math.h` — Padé [2,2], <1% error, ~8 cycles
- Quadrature oscillator for LFOs — 4 mul + 2 add per sample, no transcendentals
- Cache `pow`/`exp` for filter coefficients — only recompute when parameter changes
- `exp(x) ≈ 1+x` for very small |x| < 0.002 (filter decay coefficients)

_This section will grow as custom effects are built. Add notes here about design decisions, bugs found, and non-obvious behaviors._
