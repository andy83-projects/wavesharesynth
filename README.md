# Waveshare ESP32-P4 Synth

A Kaoss-pad-style synth running on the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C board.

## Hardware
- **Board**: Waveshare ESP32-P4-WIFI6-Touch-LCD-4C
- **Display**: 4" round 720×720 IPS touch (GT9271 capacitive)
- **Audio**: ES8311 codec + onboard speaker amp
- **MCU**: ESP32-P4NRW32 (dual-core Xtensa, 32MB PSRAM, 32MB flash)

## Setup

### 1. Install ESP-IDF
```bash
# Clone ESP-IDF (if not already installed)
git clone https://github.com/espressif/esp-idf.git ~/.esp-idf
cd ~/.esp-idf
git checkout v5.3  # or latest stable
./install.sh esp32p4
source export.sh
```

### 2. Build the project
```bash
idf.py build
```

### 3. Flash
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## Milestones

### Milestone 4: Touch-controlled synth interaction (Complete)
- [x] ESP32Synth integrated through the Waveshare BSP ES8311/I2S path
- [x] Touch X controls pitch from 130–1,000 Hz
- [x] Touch Y controls volume with vertical inversion
- [x] Touch controls pass through a FreeRTOS queue to the audio task
- [x] Touch release sends note-off; startup chord remains disabled
- [x] Firmware flashed and touch-generated audio verified on hardware

See [TOUCH_LAYOUT.md](TOUCH_LAYOUT.md) for the control layout and mapping.

## Project Structure

```
.
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   └── main.cpp         (app entry, LVGL UI and synth controls)
├── components/
│   ├── esp32synth/      (ESP32Synth library)
│   └── ...
└── README.md
```

## Pin Assignments

**TODO**: Confirm all pins from Waveshare official BSP/demos before flashing.

```
I2S:
  BCK     = GPIO 12
  WS      = GPIO 10
  DO      = GPIO 9   (playback to ES8311)
  DI      = GPIO 11  (capture from ES7210)
  MCLK    = GPIO 13

I2C (Codec):
  SDA     = GPIO 7
  SCL     = GPIO 8

Audio:
  Amp EN  = GPIO 53
```
