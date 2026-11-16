# Milestone 1 Setup & Next Steps

## Current Status
Project structure initialized with skeleton code for:
- I2C initialization (codec control)
- I2S initialization (audio out)
- GPIO control (speaker amp enable)
- ESP32Synth integration stubs

## Before You Build: Critical Information Needed

### 1. **Pin Assignments**
The pins in `main/main.c` are **placeholders**. You need to verify them from:
- Waveshare official ESP32-P4-WIFI6-Touch-LCD-4C BSP/demos
- Check: `I2S_BCK_PIN`, `I2S_WS_PIN`, `I2S_DO_PIN`, `I2S_DI_PIN`, `I2S_MCLK_PIN`
- Check: `I2C_SDA_PIN`, `I2C_SCL_PIN` (codec I2C)
- Check: `SPEAKER_AMP_EN_PIN`
- Do NOT guess or proceed without confirming

### 2. **ESP32Synth Component**
Need to add the library as an ESP-IDF component:

```bash
cd /Users/andydrag/Documents/GitRepos/wavesharesynth/components
git clone https://github.com/danilogcrf2-oss/ESP32Synth.git esp32synth
```

This should provide:
- `esp32synth.h` (main API)
- `esp32synth_patches.hpp` (voice patches)
- Proper CMakeLists.txt for ESP-IDF integration

### 3. **ES8311 Codec Driver**
Options:
- Use Waveshare's BSP (already has ES8311 driver): add to `idf_component.yml`
- Use `esp_codec_dev` component (generic codec driver)
- Minimal register init if neither is available

### 4. **I2S Audio Setup**
Two approaches:
- **ESP32Synth's `begin()`**: handles I2S internally (simpler, if no conflicts)
- **`beginCustom()` + manual I2S**: more control, what `main.c` currently uses

The final choice depends on codec driver availability.

## Steps to Get Milestone 1 Working

1. **Verify pins** from Waveshare demos
   - Find the official BSP at: https://github.com/waveshare/ESP32-P4
   - Look for pin definitions in demo code
   - Update `main/main.c` with correct pins

2. **Add ESP32Synth component**
   ```bash
   cd components
   git clone https://github.com/danilogcrf2-oss/ESP32Synth.git esp32synth
   ```

3. **Choose codec initialization strategy**
   - If using Waveshare BSP: get `es8311_codec` driver setup
   - Otherwise: minimal I2C register writes to wake the codec

4. **Install ESP-IDF** (if not already done)
   ```bash
   curl https://dl.espressif.com/github_assets/espressif/esp-idf/releases/download/v5.3/esp-idf-tools-setup-online-5.3.exe
   # or on macOS:
   git clone https://github.com/espressif/esp-idf.git ~/.esp-idf
   ~/.esp-idf/install.sh esp32p4
   source ~/.esp-idf/export.sh
   ```

5. **Build & test**
   ```bash
   cd /Users/andydrag/Documents/GitRepos/wavesharesynth
   idf.py set-target esp32p4
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

6. **What to listen for**
   - Serial output should show boot sequence
   - After ~2 seconds, speaker should play a C major chord (C4, E4, G4)
   - Then silence for 2 seconds, then loop

## Key References

- **Waveshare BSP**: https://github.com/waveshare/ESP32-P4
- **ESP32Synth**: https://github.com/danilogcrf2-oss/ESP32Synth
- **ESP-IDF Docs**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/
- **ES8311 Datasheet**: Available in Waveshare resources or online

## Once Milestone 1 is Done

After confirming audio playback:
- Move to Milestone 2: Display + touch initialization
- Then: LVGL setup and pad screen UI
- Then: Synth control handoff via seqlock
