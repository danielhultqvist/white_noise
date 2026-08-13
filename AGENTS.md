# AGENTS.md

## Project Overview

A battery-powered **ocean wave** sound box built around an ESP32-C3 driving a MAX98357A I2S amplifier and a bare EC11 rotary encoder with a push switch. The device synthesizes ocean-wave sound entirely in real time (filtered white noise + stochastic envelopes) and plays it continuously through an attached speaker.

## Hardware

See `blueprint.md` for the complete bill of materials, pin assignments, and wiring guide. Key points agents must respect when touching code:

- **Board:** ESP32-C3 (`esp32:esp32:esp32c3:CDCOnBoot=cdc`).
- **I2S amplifier (MAX98357A):** BCLK=GPIO 4, LRC=GPIO 5, DIN=GPIO 6.
- **Rotary encoder (EC11):** CLK=GPIO 2, DT=GPIO 7, SW=GPIO 3.
- **Pin constraints:**
  - GPIO 3 is required for the push switch because it is an RTC-capable pin used for `esp_deep_sleep` wake-up.
  - GPIO 1 (UART0 TX) and the strapping pins (GPIO 0, 8/9, 2 depending on variant) must not be used for the encoder; using GPIO 1 crashes the quadrature ISR because `Serial.print` drives the pad.
  - Internal `INPUT_PULLUP` is enabled in software on all encoder pins; no external pull-ups are required.

## Code Layout

Single-sketch Arduino project:

- `white_noise.ino` — all firmware. Top-level sections: pin definitions, I2S setup (`initI2S`), ocean-wave synthesizer (`generateOceanSample`, three-band noise + `processAudio` stochastic modulation), startup tunes (`playStartupTunes`), rotary-encoder ISR (`encoderISR`, quadrature lookup table), push-button state machine (`loop`), and deep-sleep power management (`enterDeepSleep`, `checkTurnOnCondition`).
- `ALGORITHM.md` — authoritative documentation of the synthesis algorithm (signal flow, filter coefficients, crest scheduler, `seaStates` table). **Must be kept in sync with any algorithm/coefficient/tuning change** (see "Maintenance contract" in that file).
- `blueprint.md` — hardware wiring reference (authoritative for pin assignments).
- `compile.sh` — builds the sketch with `arduino-cli` for the `esp32c3` board.
- `flash.sh` — uploads the compiled binary to `/dev/ttyACM0`.

## Build and Flash

```sh
./compile.sh    # builds the firmware
./flash.sh      # uploads to /dev/ttyACM0 (CDC USB serial)
```

Requires the `arduino-cli` core `esp32:esp32:esp32c3` to be installed. No external Arduino libraries are used; only built-in ESP-IDF `driver/i2s.h`, `esp_sleep.h`, and `driver/gpio.h` headers.

## Runtime Behavior

- **Power on:** The device boots directly into deep sleep. Press and hold the encoder push switch for at least 1 second to wake and start playback. A shorter press returns it to deep sleep.
- **Power off:** While running, hold the push switch for 3 seconds to re-enter deep sleep.
- **Volume:** Rotate the encoder. Each EC11 detent (2 quadrature transitions) adjusts `currentVolume` by 0.05 (5%), clamped to [0.0, 1.0].
- **Noise type:** Short press (< 1 s, with debounce) cycles the sea state: calm → normal → rough → calm. Each state retunes crest duration/arrival and band gains (see `ALGORITHM.md`).

## Conventions for Agents

- All firmware lives in a single `.ino` file; do not split it unless asked.
- Do not add comments to existing code unless explicitly requested.
- **Keep `ALGORITHM.md` in sync with any change to the synthesis logic, filter coefficients, modulation model, or `seaStates` tuning table.** This is a hard maintenance contract — see the "Maintenance contract" section in `ALGORITHM.md`.
- Do not modify `blueprint.md` wiring without confirming with the user — it is the source of truth for hardware and several pin choices are load-bearing (RTC wake-up, UART conflict avoidance).
- After code changes, verify with `./compile.sh` before considering the work done.