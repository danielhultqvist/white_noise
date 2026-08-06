#include <Arduino.h>
#include "driver/i2s.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

struct Note;

// --- Pin Definitions---
#define PIN_ROT_CLK   2
#define PIN_ROT_DT    7
#define PIN_ROT_SW    3   // RTC GPIO 3 for deep sleep wakeup

#define PIN_I2S_BCLK  4
#define PIN_I2S_LRC   5
#define PIN_I2S_DOUT  6

// --- Audio Configurations ---
#define SAMPLE_RATE   32000
#define I2S_PORT      I2S_NUM_0
#define DMA_BUF_LEN   256

// --- Ocean Wave Synthesizer Tuning ---
#define RUMBLE_LP_COEF   0.0347f
#define RUMBLE_GAIN      4.0f
#define SURF_HP_COEF     0.0479f
#define SURF_LP_COEF     0.2097f
#define SURF_GAIN        2.0f
#define HISS_HP_COEF     0.3875f
#define HISS_LP_COEF     0.20f
#define HISS_GAIN        1.1f
#define DC_BLOCK_COEF    0.995f

enum SeaState {
    SEA_CALM = 0,
    SEA_NORMAL,
    SEA_ROUGH,
    SEA_COUNT
};

struct SeaParams {
    float period1;
    float period2;
    float rumble;
    float surf;
    float hiss;
};

const SeaParams seaStates[SEA_COUNT] = {
    {  9.0f, 14.5f, 0.55f, 0.55f, 0.18f },
    {  7.0f, 11.5f, 0.70f, 0.85f, 0.40f },
    {  5.5f,  8.7f, 0.90f, 1.10f, 0.85f }
};

volatile uint8_t currentSeaState = SEA_CALM;
volatile float currentVolume = 0.05f; // Start at 5% volume
bool i2sInitialized = false;

// --- Ocean Wave Synthesizer State ---
float rumbleLP  = 0.0f;
float surfHP_lp = 0.0f;   // low-pass state used to derive the surf high-pass
float surfLP    = 0.0f;   // second stage low-pass of the surf band
float hissLP    = 0.0f;   // low-pass state used to derive the hiss high-pass
float hissLP2   = 0.0f;   // softens the hiss top end (water, not sizzle)
float dcPrevIn  = 0.0f;
float dcPrevOut = 0.0f;

// One-pole smoothers that crossfade the band gains so short-press sea-state
// changes don't step the real-time target (which would otherwise click).
float smRumble = 0.0f, smSurf = 0.0f, smHiss = 0.0f;

// --- Stochastic modulation state ---
// Slow low-passed-noise random walks: the continuous swell background. Unlike
// sine LFOs, brown-noise envelopes make wave arrival feel un-metronomic.
float envRumbleLP = 0.0f;
float envSurfLP   = 0.0f;
float envHissLP   = 0.0f;

// Discrete crest (wave-arrival) event scheduler: each crest is a short
// attack/decay burst that briefly boosts the surf body and hiss foam so
// breakers arrive one at a time, irregularly, like a real shoreline.
bool         crestActive   = false;
unsigned long crestStartMs = 0;
unsigned long crestDurMs   = 0;
float        crestAmp      = 0.0f;
unsigned long nextCrestMs  = 0;

// --- Quadrature State Machine for Rotary Encoder ---
volatile int encoderDelta = 0;
volatile int8_t encoderAcc = 0;
volatile uint8_t lastAB = 0x03;

void IRAM_ATTR encoderISR() {
    uint8_t clk = digitalRead(PIN_ROT_CLK);
    uint8_t dt  = digitalRead(PIN_ROT_DT);
    uint8_t currAB = (clk << 1) | dt;
    
    // Combine previous state and current state into a 4-bit index
    uint8_t index = (lastAB << 2) | currAB;
    lastAB = currAB;

    // 16-state Gray code lookup table (cancels out contact bounce)
    static const int8_t encoderStates[] = {
        0, -1,  1,  0,
        1,  0,  0, -1,
       -1,  0,  0,  1,
        0,  1, -1,  0
    };

    encoderAcc += encoderStates[index];

    // Standard EC11 detent click = 2 state transitions
    if (encoderAcc >= 2) {
        encoderDelta++;
        encoderAcc -= 2;
    } else if (encoderAcc <= -2) {
        encoderDelta--;
        encoderAcc += 2;
    }
}

// --- Button State ---
unsigned long buttonPressStartTime = 0;
bool buttonIsPressed = false;
unsigned long lastButtonCheckMs = 0;

void enterDeepSleep() {
    Serial.println(">>> Turning OFF (Entering Deep Sleep) <<<");
    Serial.flush();

    if (i2sInitialized) {
        i2s_zero_dma_buffer(I2S_PORT);
        i2s_driver_uninstall(I2S_PORT);
        i2sInitialized = false;
    }

    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_ROT_SW, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

void initI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = PIN_I2S_BCLK,
        .ws_io_num = PIN_I2S_LRC,
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2sInitialized = true;
}

// --- Ocean Wave Synthesizer ---
// Three band-limited noise layers modulated by slow, incommensurate LFOs so
// wave arrivals feel organic rather than metronomic:
//   rumble -> low-passed  (~180 Hz), gently breathing background wash
//   surf   -> band-passed (~250..1200 Hz), the body of each wave
//   hiss   -> high-passed (~2500 Hz), rises sharply only at wave crests
float generateOceanSample(float gainRumble, float gainSurf, float gainHiss) {
    float whiteNoise = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;

    rumbleLP += RUMBLE_LP_COEF * (whiteNoise - rumbleLP);
    float rumble = rumbleLP * RUMBLE_GAIN;

    float hpSurf = whiteNoise - surfHP_lp;
    surfHP_lp  += SURF_HP_COEF * (whiteNoise - surfHP_lp);
    surfLP     += SURF_LP_COEF * (hpSurf - surfLP);
    float surf = surfLP * SURF_GAIN;

    float hpHiss = whiteNoise - hissLP;
    hissLP  += HISS_HP_COEF * (whiteNoise - hissLP);
    hissLP2 += HISS_LP_COEF * (hpHiss - hissLP2);
    float hiss = hissLP2 * HISS_GAIN;

    float mixedBands = rumble * gainRumble + surf * gainSurf + hiss * gainHiss;

    float out = mixedBands - dcPrevIn + DC_BLOCK_COEF * dcPrevOut;
    dcPrevIn  = mixedBands;
    dcPrevOut = out;
    return out;
}

void processAudio() {
    int16_t buffer[DMA_BUF_LEN];
    float vol = currentVolume;

    const SeaParams &seaParams = seaStates[currentSeaState];
    unsigned long now = millis();

    // Slow stochastic "bed" envelopes: low-pass independent white-noise streams
    // into slow brownian random walks (~0.1 Hz). These become the gentle swell
    // that always sits under the discrete crests.
    float whiteRumble = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
    float whiteSurf   = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
    float whiteHiss   = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
    envRumbleLP += 0.0040f * (whiteRumble - envRumbleLP);
    envSurfLP   += 0.0070f * (whiteSurf - envSurfLP);
    envHissLP   += 0.0050f * (whiteHiss - envHissLP);

    // Map [-1,1] random walks to bed amounts with a non-zero floor so the wash
    // never drops out completely.
    float bedRumble = 0.30f + 0.15f * envRumbleLP;   // ~0.15..0.45
    float bedSurf   = 0.30f + 0.20f * envSurfLP;     // ~0.10..0.50
    float bedHiss   = 0.08f + 0.05f * envHissLP;     // ~0.03..0.13

    // --- Discrete crest (wave-arrival) scheduler ---
    // Triggered on an exponential interval around p.period2 so arrivals feel
    // irregular rather than metronomic.
    if (!crestActive && now >= nextCrestMs) {
        crestActive   = true;
        crestStartMs  = now;
        float durJit  = ((float)rand() / (float)RAND_MAX);
        crestDurMs    = (unsigned long)(seaParams.period1 * 1000.0f * (0.7f + 0.6f * durJit));
        crestAmp      = 0.6f + 0.4f * ((float)rand() / (float)RAND_MAX);
    }

    float crestEnv  = 0.0f;   // drives the surf crest body
    float crestFoam = 0.0f;   // drives the hiss fizz; slower decay so foam lingers
    if (crestActive) {
        float crestPhase = (float)(now - crestStartMs) / (float)crestDurMs;  // 0..1
        if (crestPhase >= 1.0f) {
            crestActive = false;
            float uniform = ((float)rand() / (float)RAND_MAX);
            if (uniform < 0.001f) uniform = 0.001f;
            float meanMs = seaParams.period2 * 1000.0f;
            float interval = -meanMs * logf(uniform);   // exponential wait -> Poisson arrivals
            if (interval > meanMs * 4.0f) interval = meanMs * 4.0f;
            nextCrestMs = now + (unsigned long)interval;
        } else {
            // Surf body: cosine attack over first 20%, exp decay after.
            if (crestPhase < 0.2f) {
                crestEnv = 0.5f * (1.0f - cosf(PI * crestPhase / 0.2f));
            } else {
                crestEnv = expf(-(crestPhase - 0.2f) * 5.0f);
            }
            // Foam fizz: fast rise to peak at breaker impact, long slow tail.
            if (crestPhase < 0.05f) {
                crestFoam = crestPhase / 0.05f;
            } else {
                crestFoam = expf(-(crestPhase - 0.05f) * 1.8f);
            }
        }
    }

    float tRumble = (bedRumble                          ) * seaParams.rumble;
    float tSurf   = (bedSurf   + crestAmp * crestEnv  * 0.8f) * seaParams.surf;
    float tHiss   = (bedHiss   + crestAmp * crestFoam       ) * seaParams.hiss;

    // Crossfade band gains toward the target (smooths sea-state switches).
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

    size_t bytes_written;
    i2s_write(I2S_PORT, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
}

// --- Startup Tunes ---
struct Note {
    float freq;
    float dur;
};

const Note tune1[] = {
    {261.63f, 0.25f}, {293.66f, 0.25f}, {329.63f, 0.25f}, {349.23f, 0.25f},
    {392.00f, 0.25f}, {440.00f, 0.25f}, {493.88f, 0.25f}, {523.25f, 0.25f}
};

const Note tune2[] = {
    {261.63f, 0.30f}, {329.63f, 0.30f}, {392.00f, 0.30f}, {523.25f, 0.30f},
    {392.00f, 0.30f}, {329.63f, 0.30f}, {261.63f, 0.20f}
};

const Note tune3[] = {
    {392.00f, 0.20f}, {523.25f, 0.20f}, {659.25f, 0.20f}, {783.99f, 0.20f},
    {659.25f, 0.20f}, {523.25f, 0.20f}, {392.00f, 0.20f}, {329.63f, 0.20f},
    {392.00f, 0.20f}, {523.25f, 0.20f}
};

void playTune(const Note *notes, int count) {
    const float tuneVol = 0.10f;
    const float attack = 0.010f;
    const float release = 0.030f;
    int16_t buffer[DMA_BUF_LEN];
    size_t bytes_written;

    for (int noteIndex = 0; noteIndex < count; noteIndex++) {
        float freq = notes[noteIndex].freq;
        unsigned long totalSamples = (unsigned long)(notes[noteIndex].dur * (float)SAMPLE_RATE);
        float phaseInc = 2.0f * PI * freq / (float)SAMPLE_RATE;
        float phase = 0.0f;
        unsigned long attackSamples = (unsigned long)(attack * SAMPLE_RATE);
        unsigned long relSamples = (unsigned long)(release * SAMPLE_RATE);
        unsigned long produced = 0;

        while (produced < totalSamples) {
            int chunk = (totalSamples - produced < DMA_BUF_LEN) ? (int)(totalSamples - produced) : DMA_BUF_LEN;
            for (int i = 0; i < chunk; i++) {
                unsigned long sampleIndex = produced + i;
                float env = 1.0f;
                if (sampleIndex < attackSamples) env = (float)sampleIndex / (float)attackSamples;
                unsigned long relStart = (relSamples < totalSamples) ? (totalSamples - relSamples) : 0;
                if (sampleIndex > relStart) {
                    float releaseEnv = (float)(totalSamples - sampleIndex) / (float)relSamples;
                    if (releaseEnv < 0.0f) releaseEnv = 0.0f;
                    env = (env < releaseEnv) ? env : releaseEnv;
                }
                float sample = sinf(phase) * env * tuneVol;
                buffer[i] = (int16_t)(sample * 32767.0f);
                phase += phaseInc;
                if (phase >= 2.0f * PI) phase -= 2.0f * PI;
            }
            i2s_write(I2S_PORT, buffer, chunk * 2, &bytes_written, portMAX_DELAY);
            produced += chunk;
        }
        delay(40);
    }
}

void playStartupTunes() {
    Serial.println(">>> Playing startup tunes <<<");
    playTune(tune1, sizeof(tune1) / sizeof(tune1[0]));
    delay(200);
    playTune(tune2, sizeof(tune2) / sizeof(tune2[0]));
    delay(200);
    playTune(tune3, sizeof(tune3) / sizeof(tune3[0]));
    Serial.println(">>> Starting ocean noise <<<");
}

void checkTurnOnCondition() {
    pinMode(PIN_ROT_SW, INPUT_PULLUP);
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        Serial.println("Woken up by button press. Checking 3s hold...");
        unsigned long start = millis();
        bool heldThreeSec = false;

        while (digitalRead(PIN_ROT_SW) == LOW) {
            if (millis() - start >= 3000) {
                heldThreeSec = true;
                break;
            }
            delay(10);
        }

        if (!heldThreeSec) {
            Serial.println("Button released early (< 3s). Returning to sleep.");
            enterDeepSleep();
        }
        Serial.println(">>> 3s Hold confirmed! Turning ON <<<");
    } else {
        Serial.println("Cold boot detected. Entering sleep until 3s button press.");
        enterDeepSleep();
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    // 1. Check power-on condition
    checkTurnOnCondition();

    // 2. Setup Pin Modes
    pinMode(PIN_ROT_CLK, INPUT_PULLUP);
    pinMode(PIN_ROT_DT, INPUT_PULLUP);
    pinMode(PIN_ROT_SW, INPUT_PULLUP);

    // Initial state read for encoder
    uint8_t clk = digitalRead(PIN_ROT_CLK);
    uint8_t dt  = digitalRead(PIN_ROT_DT);
    lastAB = (clk << 1) | dt;

    // 3. Attach interrupts to BOTH pins on CHANGE for state tracking
    attachInterrupt(digitalPinToInterrupt(PIN_ROT_CLK), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ROT_DT), encoderISR, CHANGE);

    initI2S();
    playStartupTunes();
    nextCrestMs = millis() + 1500;   // grace before the first crest
    Serial.println("Ocean Noise Machine Active! Starting Volume: 5%");
}

void loop() {
    // 1. Continuous Audio Stream
    processAudio();

    // 2. Process Encoder Volume Step Changes
    if (encoderDelta != 0) {
        int delta;
        noInterrupts();
        delta = encoderDelta;
        encoderDelta = 0;
        interrupts();

        currentVolume += delta * 0.05f; // Adjust volume by 5% per detent
        if (currentVolume > 1.0f) currentVolume = 1.0f;
        if (currentVolume < 0.0f) currentVolume = 0.0f;

        Serial.print("Volume: ");
        Serial.print((int)(currentVolume * 100));
        Serial.println("%");
    }

    // 3. Push Button Logic
    // Non-blocking debounce: sample the switch at most once per 20 ms so the
    // audio pump (which self-throttles on i2s_write) is never starved. A
    // blocking delay() here causes DMA underruns that sound like a choppy
    // "helicopter" amplitude modulation.
    unsigned long now = millis();
    if (now - lastButtonCheckMs >= 20) {
        lastButtonCheckMs = now;
        int swState = digitalRead(PIN_ROT_SW);

        if (swState == LOW && !buttonIsPressed) {
            buttonIsPressed = true;
            buttonPressStartTime = millis();
        } 
        else if (swState == LOW && buttonIsPressed) {
            if (millis() - buttonPressStartTime >= 3000) {
                enterDeepSleep();
            }
        } 
        else if (swState == HIGH && buttonIsPressed) {
            unsigned long pressDuration = millis() - buttonPressStartTime;
            buttonIsPressed = false;

            if (pressDuration > 50 && pressDuration < 1000) {
                currentSeaState = (currentSeaState + 1) % SEA_COUNT;
                Serial.print("Switched Sea State to: ");
                if (currentSeaState == SEA_CALM)   Serial.println("CALM");
                else if (currentSeaState == SEA_NORMAL) Serial.println("NORMAL");
                else if (currentSeaState == SEA_ROUGH) Serial.println("ROUGH");
            }
        }
    }
}