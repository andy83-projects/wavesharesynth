# Known Blockers for Milestone 1

Before the project can build and audio can be tested, these blockers must be resolved:

## 1. ✅ Pin Assignments (CONFIRMED)
**Status**: DONE  
**Source**: https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-XC/ESP-IDF

**Confirmed pins**:
```
I2S:
  MCLK  = GPIO 13
  BCK   = GPIO 12
  WS    = GPIO 10
  DO    = GPIO 11 (ASDOUT - data to codec)
  DI    = GPIO 9  (DSDIN - data from codec)

I2C (codec control):
  SDA   = GPIO 7
  SCL   = GPIO 8
  Address = 0x18

Speaker Amp:
  PA_Ctrl = GPIO 53 (active-high, NS4150B amplifier)
```

**Updated in `main/main.c`** ✓

---

## 2. ✅ Codec Initialization (DONE)
**Status**: Complete with Espressif ES8311 component  

**Implementation**:
- Uses official `espressif/es8311` component (high-level API)
- Proper I2C device creation and initialization
- Configured for 32 kHz sample rate with 12.288 MHz MCLK
- Volume set to -20 dB default
- Microphone disabled (playback only)

**Code**: `init_codec()` in `main/main.c` ✓

**Component dependency added**: `espressif/es8311: "^1.0.0"` in CMakeLists.txt

---

## 3. ⚠️ ESP32Synth Component (NEXT ACTION)
**Status**: Not yet cloned  
**Issue**: Component doesn't exist, build will fail without it.

**Action** (run before building):
```bash
cd /Users/andydrag/Documents/GitRepos/wavesharesynth/components
git clone https://github.com/danilogcrf2-oss/ESP32Synth.git esp32synth
```

**Verify after clone**:
- `components/esp32synth/CMakeLists.txt` exists
- `components/esp32synth/esp32synth.h` present
- `components/esp32synth/esp32synth_patches.hpp` present
- Try a quick build to check integration

**Note**: ESP32Synth source is already referenced in `main/CMakeLists.txt`

---

## 4. ⚠️ ESP-IDF Installation (BLOCKING)
**Status**: Not confirmed on this system  
**Issue**: `idf.py` not in PATH.

**Action**:
```bash
git clone https://github.com/espressif/esp-idf.git ~/.esp-idf
cd ~/.esp-idf && git checkout v5.3
./install.sh esp32p4
source export.sh
```

**Verify**: `idf.py --version` should return 5.3.x+

---

## 5. I2S Audio Path Conflict
**Status**: Potential issue  
**Issue**: Current `main.c` uses manual I2S setup via `i2s_new_std_tx_channel()`.  
If codec driver also tries to set up I2S, there will be a conflict.

**Resolution**:
- If Waveshare BSP has ES8311 driver that handles I2S → use `esp32synth_begin()` (not `beginCustom()`)
- If manual codec init → keep `beginCustom()` with our I2S channel
- Coordinate with actual driver choice before building

---

## 6. Speaker Amp GPIO Polarity
**Status**: Unknown  
**Issue**: GPIO 41 polarity (active-high vs active-low) not confirmed.

**Symptom**: Audio plays silently or speaker is always on.

**Action**:
1. Measure GPIO 41 with multimeter after boot
2. Listen for audio with both polarities
3. Update `init_speaker_amp()` if needed

---

## Recommended Resolution Order

✅ DONE:
1. Get Waveshare BSP and extract pin definitions
2. Update `main/main.c` with correct pins (all 10 pins confirmed)
3. Choose codec initialization strategy (Espressif ES8311 component)
4. Codec full implementation with Espressif driver

🚨 TODO (before build):
5. Install ESP-IDF (or confirm it's installed) with esp32p4 target
6. Clone and integrate ESP32Synth component
7. Build: `idf.py set-target esp32p4 && idf.py build`
8. Flash and listen for audio on GPIO 53 speaker
9. Debug if no sound (see troubleshooting in MILESTONE1.md)

**After steps 7-9 pass → Milestone 1 complete**

---

**Last Updated**: 2026-11-16  
**Status**: 4/9 steps complete (blockers resolved: pins, codec init, I2C setup)
