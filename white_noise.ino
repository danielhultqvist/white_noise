#include <Arduino.h>
#include "driver/i2s.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

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

enum NoiseType {
    NOISE_WHITE = 0,
    NOISE_PINK,
    NOISE_BROWN,
    NOISE_COUNT
};

volatile NoiseType currentNoise = NOISE_WHITE;
volatile float currentVolume = 0.20f; // Start at 20% volume
bool i2sInitialized = false;

// --- Pink Noise Generator State ---
float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

// --- Brown Noise Generator State ---
float lastBrown = 0.0f;

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

// --- Noise Algorithms ---
float generateWhiteSample() {
    return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
}

float generatePinkSample() {
    float white = generateWhiteSample();
    b0 = 0.99886f * b0 + white * 0.0555179f;
    b1 = 0.99332f * b1 + white * 0.0750759f;
    b2 = 0.96900f * b2 + white * 0.1538520f;
    b3 = 0.86650f * b3 + white * 0.3104856f;
    b4 = 0.55000f * b4 + white * 0.5329522f;
    b5 = -0.7616f * b5 - white * 0.0168980f;
    float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
    b6 = white * 0.115926f;
    return pink * 0.11f;
}

float generateBrownSample() {
    float white = generateWhiteSample();
    lastBrown = (lastBrown + (0.02f * white)) / 1.02f;
    return lastBrown * 3.5f;
}

void processAudio() {
    int16_t buffer[DMA_BUF_LEN];
    float vol = currentVolume;

    for (int i = 0; i < DMA_BUF_LEN; i++) {
        float sample = 0.0f;
        switch (currentNoise) {
            case NOISE_WHITE: sample = generateWhiteSample(); break;
            case NOISE_PINK:  sample = generatePinkSample(); break;
            case NOISE_BROWN: sample = generateBrownSample(); break;
            default: break;
        }

        sample *= vol;
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;

        buffer[i] = (int16_t)(sample * 32767.0f);
    }

    size_t bytes_written;
    i2s_write(I2S_PORT, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
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
    Serial.println("White Noise Machine Active! Starting Volume: 20%");
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
    delay(20); // Debounce the push switch reads
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
            currentNoise = (NoiseType)((currentNoise + 1) % NOISE_COUNT);
            Serial.print("Switched Noise Type to: ");
            if (currentNoise == NOISE_WHITE) Serial.println("WHITE");
            else if (currentNoise == NOISE_PINK) Serial.println("PINK");
            else if (currentNoise == NOISE_BROWN) Serial.println("BROWN");
        }
    }
}
