# white-noise

A battery-powered ocean-wave sound box built around an ESP32-C3, a MAX98357A
I2S amplifier, and an EC11 rotary encoder with push switch. The device
synthesizes ocean-wave sound entirely in real time — filtered white noise shaped
by slow brown-noise envelopes plus stochastically-triggered breaker crests — and
plays it continuously through an attached speaker.

See [`ALGORITHM.md`](ALGORITHM.md) for the authoritative description of the synthesis
algorithm (signal flow, filter coefficients, crest scheduler, and the `seaStates`
tuning table). **That file must be kept in sync with any change to the synthesis
logic, coefficients, modulation model, or tuning table** — see the "Maintenance
contract" section at the end of `ALGORITHM.md`.

See [`blueprint.md`](blueprint.md) for hardware/wiring, and [`AGENTS.md`](AGENTS.md)
for build/flash and runtime behavior.
