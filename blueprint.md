# Hardware Wiring Guide: ESP32-C3 White Noise Box

## Component List

* **Microcontroller:** ESP32-C3 Development Board (with LiPo charger)
* **Audio Amplifier:** MAX98357A I2S Class-D 3W Mono Amplifier
* **Speaker:** 4 Ohm 3W Speaker (JST-PH connector)
* **Control:** EC11 Bare Rotary Encoder with Push Switch (5-pin)

---

## Master Connection Table

| Origin Component | Component Pin | Target Board | Target Pin | Signal / Function | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Rotary Encoder** | **CLK** (3-pin side, Pin 1) | **ESP32-C3** | **GPIO 0** | Quadrature Signal A | Software `INPUT_PULLUP` enabled |
| **Rotary Encoder** | **GND** (3-pin side, Pin 2) | **ESP32-C3** | **GND** | Common Ground | Center pin on 3-pin side |
| **Rotary Encoder** | **DT** (3-pin side, Pin 3) | **ESP32-C3** | **GPIO 1** | Quadrature Signal B | Software `INPUT_PULLUP` enabled |
| **Rotary Encoder** | **SW** (2-pin side, Pin 1) | **ESP32-C3** | **GPIO 3** | Push Switch Signal | RTC GPIO pin used for deep sleep wakeup |
| **Rotary Encoder** | **SW GND** (2-pin side, Pin 2)| **Rotary Encoder**| **GND** | Common Ground | Solder bridge to 3-pin center GND |
| **MAX98357A Amp** | **VIN** | **ESP32-C3** | **5V / VBUS** | Amplifier Power | Connect to 5V rail for full 3W output |
| **MAX98357A Amp** | **GND** | **ESP32-C3** | **GND** | Power & Signal Ground | Common ground |
| **MAX98357A Amp** | **BCLK** | **ESP32-C3** | **GPIO 4** | Bit Clock | I2S serial clock |
| **MAX98357A Amp** | **LRC / WS** | **ESP32-C3** | **GPIO 5** | Word Select | Left/Right channel clock |
| **MAX98357A Amp** | **DIN** | **ESP32-C3** | **GPIO 6** | Data Output | Serial audio stream |
| **MAX98357A Amp** | **GAIN** | **MAX98357A Amp** | **GND** | Gain Setting | Sets gain to default 12 dB |
| **MAX98357A Amp** | **SD** | *Unconnected* | *N/A* | Shutdown / Mute | Leave floating (internal pull-up enables amp) |
| **MAX98357A Amp** | **Speaker Terminal (+)**| **Speaker** | **Positive Wire**| Audio Output (+) | Channel output |
| **MAX98357A Amp** | **Speaker Terminal (-)**| **Speaker** | **Negative Wire**| Audio Output (-) | Channel output |

---

## Detail Breakdowns

### 1. Rotary Encoder Pinout (EC11 Style)

Looking at the rotary encoder with the shaft pointing upward:

```
3-PIN SIDE (Rotation)           2-PIN SIDE (Push Switch)
+---------------------+         +---------------------+
|  [CLK]  [GND]  [DT] |         |    [SW]    [GND]    |
+---|------|------|---+         +-----|--------|------+
|      |      |                   |        |
GPIO 0   GND   GPIO 1             GPIO 3   (Bridge to GND)
```

* **Ground Optimization (4-Wire Harness):** Because both the 3-pin side (middle pin) and the 2-pin side (either pin) connect to `GND`, bridge these two pins together on the encoder with a small solder blob or jumper wire. This reduces the cable run to the ESP32-C3 board to 4 total wires (`CLK`, `DT`, `SW`, `GND`).

### 2. MAX98357A I2S Amplifier Wiring

```
ESP32-C3 Pin                       MAX98357A Module Pin
+------------+                     +--------------------+
|  5V / VBUS | ------------------> | VIN                |
|        GND | ------------------> | GND                |
|     GPIO 4 | ------------------> | BCLK               |
|     GPIO 5 | ------------------> | LRC (WS)           |
|     GPIO 6 | ------------------> | DIN                |
+------------+                     | GAIN ------+       |
| SD (Float) | (GND) |
+-----|------|-------+
|      |
[ Speaker +/- ]
```

* **Power Rail:** Connect `VIN` to `5V` (or `VBUS` USB power). Running the amplifier off 3.3V will limit dynamic power and cause clipping at higher volume levels.
* **Shutdown Pin (`SD`):** Must remain unconnected. An internal resistor pulls this line high to keep the output active. Pulling it to ground will mute the device.

---

## Crucial Assembly Notes

1. **Wakeup Pin Requirement:** The push switch **must** be connected to **GPIO 3**. GPIO 3 is an RTC-capable pin on the ESP32-C3 architecture, which allows the CPU to enter low-power deep sleep and wake up when pulled LOW by the switch button.
2. **Internal Pull-Ups:** No external 10 kΩ pull-up resistors are required on `CLK`, `DT`, or `SW`. The ESP32 code configures internal `INPUT_PULLUP` resistors automatically on `GPIO 0`, `GPIO 1`, and `GPIO 3`.
3. **Common Ground:** Ensure that both the amplifier `GND` and encoder `GND` tie back to a single shared `GND` pin on the ESP32-C3 development board to prevent audio ground loop noise.
