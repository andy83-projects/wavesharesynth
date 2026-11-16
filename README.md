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

### Milestone 1: Audio Test (In Progress)
- [ ] Project builds
- [ ] ES8311 codec initialized over I2C
- [ ] Speaker amp enabled
- [ ] ESP32Synth plays a test chord through speaker
- [ ] Verify audio output with listen test

## Project Structure

```
.
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   └── main.c           (app entry, audio test)
├── components/
│   ├── esp32synth/      (ESP32Synth library)
│   └── ...
└── README.md
```

## Pin Assignments

**TODO**: Confirm all pins from Waveshare official BSP/demos before flashing.

```
I2S:
  BCK     = GPIO 8
  WS      = GPIO 9
  DO      = GPIO 10
  DI      = GPIO 11
  MCLK    = GPIO 12

I2C (Codec):
  SDA     = GPIO 17
  SCL     = GPIO 18

Audio:
  Amp EN  = GPIO 41
```
