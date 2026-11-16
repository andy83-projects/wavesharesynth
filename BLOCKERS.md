# Known Blockers for Milestone 1

Before the project can build and audio can be tested, these blockers must be resolved:

## 1. ⚠️ Pin Assignments (CRITICAL)
**Status**: Unknown  
**Issue**: `main/main.c` has placeholder pin values. Must verify from Waveshare BSP.

**Pins needed**:
- I2S: BCK, WS, DO, DI, MCLK
- I2C codec: SDA, SCL (address typically 0x18 or 0x1A)
- Speaker amp enable GPIO (and confirm polarity: active-high or active-low?)

**Action**: 
1. Download/clone Waveshare ESP32-P4 BSP: https://github.com/waveshare/ESP32-P4
2. Find pin definitions in demo code (search for `I2S_`, `I2C_`, `GPIO_AMP`, etc.)
3. Update all #define statements in `main/main.c`
4. DO NOT proceed without this

**Ref**: https://github.com/waveshare/ESP32-P4

---

## 2. ⚠️ Codec Initialization (CRITICAL)
**Status**: Stubbed  
**Issue**: `init_codec()` function in `main/main.c` is a placeholder.

**Needs to**:
- Bring up ES8311 over I2C
- Configure sample rate to 32 kHz
- Set line-out level
- Possibly configure ADC (unused for now, but may need to be muted)

**Options**:
- Use Waveshare's BSP codec driver (if available as component)
- Use `esp_codec_dev` generic driver (more portable)
- Minimal register writes if neither available

**Action**: 
1. Check Waveshare BSP for ES8311 driver example
2. Adapt or integrate into `init_codec()`
3. Test I2C communication before proceeding

---

## 3. ⚠️ ESP32Synth Component (BLOCKING)
**Status**: Not added  
**Issue**: Component doesn't exist yet, build will fail.

**Action**:
```bash
cd /Users/andydrag/Documents/GitRepos/wavesharesynth/components
git clone https://github.com/danilogcrf2-oss/ESP32Synth.git esp32synth
```

**Verify**:
- `components/esp32synth/CMakeLists.txt` exists and is ESP-IDF compatible
- `components/esp32synth/esp32synth.h` available
- `components/esp32synth/esp32synth_patches.hpp` available

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
1. ✅ Install ESP-IDF (or confirm it's installed)
2. ✅ Get Waveshare BSP and extract pin definitions
3. ✅ Update `main/main.c` with correct pins
4. ✅ Integrate ESP32Synth component (git clone)
5. ✅ Choose codec initialization strategy (BSP driver vs minimal I2C)
6. ✅ Build and test for I2C communication
7. ✅ Flash and listen for audio
8. ✅ Debug if no sound (see troubleshooting in MILESTONE1.md)

**Once all 8 steps pass → Milestone 1 complete**

---

**Last Updated**: 2026-11-16  
**Status**: 0/8 steps complete
