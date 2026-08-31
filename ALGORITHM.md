# Ocean Wave Synthesis Algorithm

This document is the authoritative description of the audio algorithm in
`white_noise.ino`. **Any change to the synthesis logic, filter coefficients,
modulation model, or `seaStates` tuning table must be reflected here.**

The synthesizer runs entirely on the ESP32-C3 in real time at 32 kHz mono. It
does **not** play back samples; every sample is computed from filtered white
noise. The design follows the well-known *band-split noise + stochastic
envelope* recipe used by Pure Data / Csound ocean patches: the audio itself is
filtered noise split into three perceptual bands, and the loudness of each band
is shaped by slow random envelopes plus randomly-triggered discrete "breaker"
events. Earlier versions drove the envelopes with sine LFOs, which sounded
metronomic; the current stochastic model makes wave arrival feel organic.

## Signal flow (per output sample)

```
                          white noise (rand)
                                 |
        +------------------------+------------------------+
        |                        |                        |
   rumble LP                surf HP->LP               hiss HP->LP
   (~180 Hz)               (~250..1200 Hz)             (~2500 Hz)
        |                        |                        |
   * RUMBLE_GAIN          * SURF_GAIN                * HISS_GAIN
        |                        |                        |
        * gainRumble             * gainSurf               * gainHiss
        |                        |                        |
        +------------------------+------------------------+
                                 |  (sum = mixedBands)
                          DC-blocking filter
                                 |
                          sample -> volume -> clamp -> int16 -> I2S
```

## 1. Three-band noise source — `generateOceanSample()`

A single `rand()` call produces white noise in [-1, 1] each sample. It is split
into three bands by cascaded one-pole filters. All filter state is global and
runs continuously (`rumbleLP`, `surfHP_lp`, `surfLP`, `hisLLP`, `hissLP2`).

### Rumble (background wash, ~180 Hz)
```
rumbleLP += RUMBLE_LP_COEF  * (whiteNoise - rumbleLP)
rumble    = rumbleLP * RUMBLE_GAIN        // RUMBLE_LP_COEF=0.02, RUMBLE_GAIN=6.0
```
A single low-pass. Gentle, always-present low swell.

### Surf (body of each wave, ~250..1200 Hz band-pass)
```
hpSurf    = whiteNoise - surfHP_lp
surfHP_lp += SURF_HP_COEF * (whiteNoise - surfHP_lp)   // high-pass stage, SURF_HP_COEF=0.0479
surfLP    += SURF_LP_COEF * (hpSurf   - surfLP)        // low-pass stage,  SURF_LP_COEF=0.14
surf      = surfLP * SURF_GAIN                         // SURF_GAIN=0.6
```
A high-pass followed by a low-pass = band-pass. This is the audible "wave" body.

### Hiss (foam / fizz, ~2500 Hz high-pass)
```
hpHiss   = whiteNoise - hislLLP
hissLP  += HISS_HP_COEF * (whiteNoise - hisLLP)        // high-pass, HISS_HP_COEF=0.3875
hissLP2 += HISS_LP_COEF * (hpHiss   - hissLP2)        // soften top edge, HISS_LP_COEF=0.12
hiss     = hisLLP2 * HISS_GAIN                        // HISS_GAIN=0.1
```
High-pass then a second low-pass so the top end is rounded ("water, not sizzle").

### Band sum + DC blocking
```
mixedBands = rumble*gainRumble + surf*gainSurf + hiss*gainHiss
out        = mixedBands - dcPrevIn + DC_BLOCK_COEF * dcPrevOut   // DC_BLOCK_COEF=0.995
dcPrevIn   = mixedBands
dcPrevOut  = out
```
The one-pole DC blocker removes the DC offset introduced by the asymmetric
low-pass chains so the speaker cone sits centered.

`gainRumble` / `gainSurf` / `gainHiss` are the time-varying modulation targets
described in section 2.

## 2. Stochastic modulation — `processAudio()` (buffer-level, once per DMA block)

The three band gains are recomputed once per 256-sample buffer (cheap and avoids
per-sample `millis()` calls). Two independent mechanisms contribute:

### 2a. Continuous "bed" envelopes (slow brown-noise random walks)

Three separate white-noise streams are low-passed with very small coefficients so
they wander slowly (~0.1 Hz) and independently, then mapped to a non-zero floor:

```
envRumbleLP += 0.0040 * (whiteRumble - envRumbleLP)
envSurfLP   += 0.0070 * (whiteSurf   - envSurfLP)
envHissLP   += 0.0050 * (whiteHiss   - envHissLP)

bedRumble = 0.15 + 0.10 * envRumbleLP      // ~0.05..0.25
bedSurf   = 0.12 + 0.10 * envSurfLP        // ~0.02..0.22
bedHiss   = 0.03 + 0.03 * envHissLP        // ~0.00..0.06
```

The non-zero floor guarantees the wash never drops out to silence. Low-passed
white noise is brown noise in expectation; using it as an envelope is what makes
the swell feel un-metronomic versus a sinusoidal LFO.

### 2b. Discrete crest (wave-arrival) events — Poisson schedule

A single crest scheduler runs alongside the bed envelopes. Each crest is a
short attack/decay burst that briefly boosts the surf body and hiss foam so
individual breakers are heard arriving at irregular intervals.

**Trigger timing:** when no crest is active and `now >= nextCrestMs`, start a
new crest. The wait until the next crest is drawn from an **exponential
distribution** with mean `seaParams.period1` (seconds):

```
crestDurMs = period1 * 1000 * (0.7 + 0.6 * rand())   // duration jitter 0.7x..1.3x
crestAmp   = 0.6 + 0.4 * rand()                      // amplitude 0.6..1.0

interval   = -meanMs * log(uniform)                    // exponential -> Poisson arrivals
nextCrestMs = now + clamp(interval, 0, 4*meanMs)
```

**Crest shape** over normalized phase `crestPhase = (now - crestStartMs) / crestDurMs`,
in [0, 1]:

- **`crestEnv`** (drives the surf body):
  - `crestPhase < 0.2`: cosine attack `0.5 * (1 - cos(PI * phase / 0.2))` (smooth rise)
  - `crestPhase >= 0.2`: exponential decay `exp(-(phase - 0.2) * 5.0)` (receding breaker)
- **`crestFoam`** (drives the hiss fizz): faster rise, much slower decay so foam lingers after the crest:
  - `crestPhase < 0.05`: linear rise `phase / 0.05`
  - `crestPhase >= 0.05`: slow decay `exp(-(phase - 0.05) * 1.8)`

### 2c. Combining into band-gain targets

```
tRumble =  bedRumble                              * seaParams.rumble
tSurf   = (bedSurf   + crestAmp * crestEnv  * 0.8) * seaParams.surf
tHiss   = (bedHiss   + crestAmp * crestFoam * 0.3)  * seaParams.hiss
```

### 2d. Crossfade smoothers

To avoid clicks when switching sea state (which steps the targets), each target
is chased by a one-pole smoother before being handed to the per-sample synth:

```
smRumble += 0.04 * (tRumble - smRumble)
smSurf   += 0.04 * (tSurf   - smSurf)
smHiss   += 0.04 * (tHiss   - smHiss)
```

`smRumble` / `smSurf` / `smHiss` are the `gainRumble` / `gainSurf` / `gainHiss`
arguments passed into `generateOceanSample()` for every sample in the buffer.

## 3. Sea-state tuning — `seaStates[]`

Three presets selected by the short-press button cycle. `period1` is the mean
crest *duration*; `period2` is the mean crest *inter-arrival* (Poisson) interval;
`rumble` / `surf` / `hiss` are per-band gain scales.

| State        | period1 (s) | period2 (s) | rumble | surf | hiss |
|--------------|-------------|-------------|--------|------|------|
| `SEA_CALM`   | 9.0         | 14.5        | 0.70   | 0.40 | 0.08 |
| `SEA_NORMAL` | 7.0         | 11.5        | 0.80   | 0.60 | 0.18 |
| `SEA_ROUGH`  | 5.5         | 8.7         | 1.00   | 0.80 | 0.35 |

Rougher seas => shorter, more frequent, louder crests and slightly more hiss,
though all sea states are tuned mellow (rumble-dominant) for bedside listening.

## 4. Output stage

```
sample *= currentVolume
clamp(sample, -1, 1)
buffer[i] = (int16_t)(sample * 32767)
i2s_write(...)
```

`currentVolume` is set by the rotary encoder (0.05 per detent, range [0, 1]).

## Coefficient reference (all defined near top of `white_noise.ino`)

| Constant         | Value  | Used by                     |
|------------------|--------|------------------------------|
| `RUMBLE_LP_COEF` | 0.02   | rumble low-pass              |
| `RUMBLE_GAIN`    | 6.0    | rumble output gain           |
| `SURF_HP_COEF`   | 0.0479 | surf high-pass stage         |
| `SURF_LP_COEF`   | 0.14   | surf low-pass stage          |
| `SURF_GAIN`      | 0.6    | surf output gain             |
| `HISS_HP_COEF`   | 0.3875 | hiss high-pass stage         |
| `HISS_LP_COEF`   | 0.12   | hiss top-edge softening LP   |
| `HISS_GAIN`      | 0.1    | hiss output gain             |
| `DC_BLOCK_COEF`  | 0.995  | DC-blocking filter pole      |

The three bed-envelope LP coefficients (0.0040 / 0.0070 / 0.0050) and the four
crest shape constants (attack split 0.2, decay 5.0; foam rise 0.05, decay 1.8)
are currently inline literals in `processAudio()`. If you retune them, update
both this document and the inline comments.

## References

The algorithm is an embedded adaptation of the well-known *band-split noise +
stochastic envelope* recipe used by real-time ocean-sound synthesizers. The
following sources were researched while developing this implementation:

### Primary practical reference (directly informed the design)

- **`arrudasoueu/synthWave`** — Pure Data patch (MIT-licensed).
  https://github.com/arrudasoueu/synthWave
  A study project in procedural ocean-sound textures using filtered noise and
  stochastic processes. Its architecture confirmed the two key ideas that the
  earlier sine-LFO version of this firmware was missing:
  1. A noise source low-passed at a few Hz (a slow brownian random walk) used as
     an *amplitude envelope* of another noise source → un-metronomic swell.
  2. Discrete triggered wave events with randomized attack/decay and a non-zero
     floor (~0.2, so the wash never drops to silence), plus a long-tail
     high-gain noise layer that gives the "fizz" which lingers after each crest.
  Source read: https://raw.githubusercontent.com/arrudasoueu/synthWave/main/synthWaves.pd

### Background / textbook techniques (informs the band-split + envelope model)

- **Csound `ocean` opcode** (Gabriel Maldonado), canonical example of
  band-split-noise + low-passed-noise-envelope ocean synthesis.
  https://csounds.com/manual/ocean.html
- **Dobson, "Sound of the Sea"** ( BYTE magazine, 1980s) — the original
  popularization of using a separate low-passed noise channel as a stochastic
  amplitude envelope for a second (filtered) noise channel. No canonical online
  URL; widely cited in the procedural-audio community.

### Academic work explored but NOT used (visualization/spatial, not embedded-mono)

These were reviewed for ideas and explicitly judged out of scope for a single
ESP32-C3 mono synth (they require particle/foam simulations or FFT-based
additive models). Listed here so future agents don't re-research them:

- Robine & Frechot, "Fast additive sound synthesis for real-time simulation of
  ocean surface" — additive/FFT surface model.
  https://hal.science/hal-00307929v1/file/robine_frechot_fast_additive_sound_synthesis_for_real_time_simulation_of_.pdf
- Nature Scientific Reports s41598-026-59852-6, "Data-driven and physics-inspired
  sound synthesis of ocean waves" — foam-particle-driven, spatial.
  https://www.nature.com/articles/s41598-026-59852-6.pdf
- IEEE 11457624, "Screen-Space Vortex-Aware Sound Synthesis for Ocean Waves via
  Particle" — screen-space particle clustering.
  https://ieeexplore.ieee.org/document/11457624

## Maintenance contract

When making any change to the algorithm, you **must**:

1. Keep this file in sync with the code (coefficients, shapes, scheduling,
   `seaStates` table, signal flow).
2. Verify with `./compile.sh` before considering the work done.