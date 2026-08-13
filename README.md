# white-noise

A battery-powered ocean-wave sound box. An ESP32-C3 drives a MAX98357A I2S
amplifier and an EC11 rotary encoder with push switch. The device synthesizes
ocean-wave sound entirely in real time — filtered white noise shaped by slow
brown-noise envelopes plus stochastically-triggered breaker crests — and plays it
continuously through an attached speaker.

## How it works

White noise is split into three frequency bands and individually low-pass
filtered. Slow brown-noise envelopes amplitude-modulate each band, while a
stochastic scheduler periodically injects "breaker crests" — short bursts shaped
by a decaying envelope. Rotary-encoder quadrature interrupts adjust volume, and
a push-button state machine handles sea-state cycling and power management.
Everything runs on the device; there is no audio file or host.

## Build and flash

Requires the `arduino-cli` core `esp32:esp32:esp32c3`. No external libraries.

```sh
./compile.sh    # builds the firmware
./flash.sh      # uploads to /dev/ttyACM0 (CDC USB serial)
```

## Using it

- **Power on:** Boots into deep sleep. Hold the push switch ≥ 3 s to wake and
  start playback.
- **Power off:** While running, hold the push switch ≥ 3 s to re-enter deep
  sleep.
- **Volume:** Rotate the encoder; each detent adjusts volume by 5%.
- **Sea state:** Short press (< 1 s) cycles calm → normal → rough → calm,
  retuning crest timing and band gains.

## Documentation

- [`ALGORITHM.md`](ALGORITHM.md) — authoritative synthesis reference (signal
  flow, filter coefficients, crest scheduler, `seaStates` tuning table). **Must
  be kept in sync with any change to synthesis logic or tuning.**
- [`blueprint.md`](blueprint.md) — bill of materials, pin assignments, and
  wiring guide.
- [`AGENTS.md`](AGENTS.md) — conventions for AI agents working on the firmware.