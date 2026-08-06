// noise_sim.cpp
// Host-side simulator for the white-noise box ocean synthesizer.
// Reproduces the exact same noise-generation algorithm as white_noise.ino
// (same filter states, same sea-state table, same crest scheduler, same
// smoothing) so the audio character can be developed locally.
//
// Build (g++, standard libraries only):
//   g++ -O2 -std=c++17 -o noise_sim noise_sim.cpp
//
// Listen (requires aplay or any raw PCM player):
//   ./noise_sim 30        | aplay -r 32000 -f S16_LE -c 1
//   ./noise_sim 60 2      | aplay -r 32000 -f S16_LE -c 1   # 60s, sea state 2
//   ./noise_sim 10 0 0.2  | aplay -r 32000 -f S16_LE -c 1   # 10s, state 0, 20%
//
// Or dump to a file:
//   ./noise_sim 5 > ocean.raw

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Audio Configuration (mirrors white_noise.ino) ---
static const int      SAMPLE_RATE   = 32000;
static const int      DMA_BUF_LEN   = 256;
static const float    PI_F          = (float)M_PI;

enum SeaState { SEA_CALM = 0, SEA_NORMAL, SEA_ROUGH, SEA_COUNT };

struct SeaParams { float period1; float period2; float rumble; float surf; float hiss; };

static const SeaParams seaStates[SEA_COUNT] = {
    {  9.0f, 14.5f, 0.55f, 0.55f, 0.18f },
    {  7.0f, 11.5f, 0.70f, 0.85f, 0.40f },
    {  5.5f,  8.7f, 0.90f, 1.10f, 0.85f }
};

// --- Runtime state (mirrors white_noise.ino globals) ---
static uint8_t currentSeaState = SEA_CALM;
static float   currentVolume   = 0.05f;

static float rumbleLP   = 0.0f;
static float surfHP_lp  = 0.0f;
static float surfLP     = 0.0f;
static float hissLP     = 0.0f;
static float hissLP2    = 0.0f;
static float dcPrevIn   = 0.0f;
static float dcPrevOut  = 0.0f;

static float smRumble = 0.0f, smSurf = 0.0f, smHiss = 0.0f;

static float envRumbleLP = 0.0f;
static float envSurfLP   = 0.0f;
static float envHissLP   = 0.0f;

static bool   crestActive   = false;
static unsigned long crestStartMs = 0;
static unsigned long crestDurMs   = 0;
static float        crestAmp      = 0.0f;
static unsigned long nextCrestMs  = 0;

// --- Deterministic LCG so output is reproducible for development.
// The firmware uses rand(); the algorithm is identical here, only the
// RNG source differs. Seed can be overridden via the --seed flag.
static uint32_t rngState = 0xdeadbeefu;
static void rngSeed(uint32_t s) { rngState = s ? s : 0xdeadbeefu; }
static uint32_t rngNext() {
    // xorshift32, full 32-bit range
    uint32_t x = rngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rngState = x;
    return x;
}
// Returns uniform float in [-1.0, 1.0] analogous to ((rand()/RAND_MAX)*2-1).
static float rngSigned() {
    return ((float)rngNext() / 4294967295.0f) * 2.0f - 1.0f;
}
static float rngUnit() {   // [0,1)
    return (float)(rngNext()) / 4294967296.0f;
}

// --- Ocean Wave Synthesizer (exact mirror of generateOceanSample) ---
static float generateOceanSample(float gRumble, float gSurf, float gHiss) {
    float w = rngSigned();

    rumbleLP += 0.0347f * (w - rumbleLP);
    float rumble = rumbleLP * 4.0f;

    float hpSurf = w - surfHP_lp;
    surfHP_lp  += 0.0479f * (w - surfHP_lp);
    surfLP     += 0.2097f * (hpSurf - surfLP);
    float surf = surfLP * 2.0f;

    float hpHiss = w - hissLP;
    hissLP  += 0.3875f * (w - hissLP);
    hissLP2 += 0.20f   * (hpHiss - hissLP2);
    float hiss = hissLP2 * 1.1f;

    float s = rumble * gRumble + surf * gSurf + hiss * gHiss;

    float out = s - dcPrevIn + 0.995f * dcPrevOut;
    dcPrevIn  = s;
    dcPrevOut = out;
    return out;
}

// --- Audio pump (exact mirror of processAudio) ---
// `nowMs` plays the role of millis() on the device. The block length in
// real time is DMA_BUF_LEN / SAMPLE_RATE seconds (= 8 ms), which governs
// the crest scheduler and smoothing rates exactly as on the firmware.
static void processAudio(int16_t *buffer, unsigned long nowMs) {
    float vol = currentVolume;
    const SeaParams &p = seaStates[currentSeaState];

    float w1 = rngSigned();
    float w2 = rngSigned();
    float w3 = rngSigned();
    envRumbleLP += 0.0040f * (w1 - envRumbleLP);
    envSurfLP   += 0.0070f * (w2 - envSurfLP);
    envHissLP   += 0.0050f * (w3 - envHissLP);

    float bedRumble = 0.30f + 0.15f * envRumbleLP;
    float bedSurf   = 0.30f + 0.20f * envSurfLP;
    float bedHiss   = 0.08f + 0.05f * envHissLP;

    if (!crestActive && nowMs >= nextCrestMs) {
        crestActive  = true;
        crestStartMs = nowMs;
        float durJit = rngUnit();
        crestDurMs   = (unsigned long)(p.period1 * 1000.0f * (0.7f + 0.6f * durJit));
        crestAmp     = 0.6f + 0.4f * rngUnit();
    }

    float crestEnv  = 0.0f;
    float crestFoam = 0.0f;
    if (crestActive) {
        float t = (float)(nowMs - crestStartMs) / (float)crestDurMs;
        if (t >= 1.0f) {
            crestActive = false;
            float u = rngUnit();
            if (u < 0.001f) u = 0.001f;
            float meanMs = p.period2 * 1000.0f;
            float interval = -meanMs * logf(u);
            if (interval > meanMs * 4.0f) interval = meanMs * 4.0f;
            nextCrestMs = nowMs + (unsigned long)interval;
        } else {
            if (t < 0.2f) {
                crestEnv = 0.5f * (1.0f - cosf(PI_F * t / 0.2f));
            } else {
                crestEnv = expf(-(t - 0.2f) * 5.0f);
            }
            if (t < 0.05f) {
                crestFoam = t / 0.05f;
            } else {
                crestFoam = expf(-(t - 0.05f) * 1.8f);
            }
        }
    }

    float tRumble = (bedRumble                              ) * p.rumble;
    float tSurf   = (bedSurf   + crestAmp * crestEnv  * 0.8f) * p.surf;
    float tHiss   = (bedHiss   + crestAmp * crestFoam       ) * p.hiss;

    smRumble += 0.04f * (tRumble - smRumble);
    smSurf   += 0.04f * (tSurf   - smSurf);
    smHiss   += 0.04f * (tHiss   - smHiss);

    for (int i = 0; i < DMA_BUF_LEN; i++) {
        float sample = generateOceanSample(smRumble, smSurf, smHiss);
        sample *= vol;
        if (sample > 1.0f)  sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        buffer[i] = (int16_t)(sample * 32767.0f);
    }
}

static void usage(const char *prog) {
    std::fprintf(stderr,
        "usage: %s [durationSec] [seaState] [volume] [--seed N] [--calm|--normal|--rough]\n"
        "  durationSec  total output length in seconds (default 30)\n"
        "  seaState     0=calm 1=normal 2=rough (default 0)\n"
        "  volume       0.0..1.0 (default 0.05)\n"
        "  --seed N     deterministic RNG seed for reproducible output\n"
        "\n"
        "Listen:  %s 30 | aplay -r 32000 -f S16_LE -c 1\n",
        prog, prog);
}

int main(int argc, char **argv) {
    float    durationSec = 30.0f;
    int      sea         = 0;
    float    vol         = 0.05f;
    uint32_t seed        = 0xdeadbeefu;

    // Lightweight positional + flag parsing.
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!std::strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]); return 0;
        } else if (!std::strcmp(a, "--seed")) {
            if (++i >= argc) { usage(argv[0]); return 1; }
            seed = (uint32_t)std::strtoul(argv[i], nullptr, 0);
        } else if (!std::strcmp(a, "--calm"))   { sea = 0; }
        else if (!std::strcmp(a, "--normal")) { sea = 1; }
        else if (!std::strcmp(a, "--rough"))  { sea = 2; }
        else if (a[0] == '-' && a[1] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", a);
            usage(argv[0]); return 1;
        } else {
            switch (positional++) {
                case 0: durationSec = (float)std::atof(a); break;
                case 1: sea = std::atoi(a); break;
                case 2: vol = (float)std::atof(a); break;
                default:
                    std::fprintf(stderr, "too many positional args\n");
                    usage(argv[0]); return 1;
            }
        }
    }

    if (durationSec <= 0.0f) {
        std::fprintf(stderr, "duration must be > 0\n"); return 1;
    }
    if (sea < 0 || sea >= SEA_COUNT) {
        std::fprintf(stderr, "seaState must be 0..%d\n", SEA_COUNT - 1); return 1;
    }
    if (vol < 0.0f || vol > 1.0f) {
        std::fprintf(stderr, "volume must be 0.0..1.0\n"); return 1;
    }

    currentSeaState = (uint8_t)sea;
    currentVolume   = vol;
    rngSeed(seed);

    // Mimic the firmware's "grace before the first crest".
    unsigned long nowMs         = 0;
    nextCrestMs                 = 1500;
    const unsigned long blockMs = (unsigned long)((uint64_t)DMA_BUF_LEN * 1000 / SAMPLE_RATE);

    const unsigned long totalSamples = (unsigned long)(durationSec * (float)SAMPLE_RATE);
    const unsigned long totalBlocks  = (totalSamples + DMA_BUF_LEN - 1) / DMA_BUF_LEN;

    // Write raw signed 16-bit little-endian mono PCM to stdout in binary.
#ifdef _WIN32
    _setmode(_fileno(stdout), 0x8000);  // _O_BINARY
#endif
    FILE *out = stdout;

    int16_t buffer[DMA_BUF_LEN];
    for (unsigned long b = 0; b < totalBlocks; b++) {
        processAudio(buffer, nowMs);
        size_t n = (b == totalBlocks - 1 && totalSamples % DMA_BUF_LEN)
                   ? (totalSamples % DMA_BUF_LEN) : (size_t)DMA_BUF_LEN;
        // fwrite is binary-safe; host byte order on x86/ARM-LE is already LE.
        if (std::fwrite(buffer, sizeof(int16_t), n, out) != n) {
            std::fprintf(stderr, "write error (broken pipe?)\n");
            return 1;
        }
        nowMs += blockMs;
    }
    return 0;
}