# Milestone 1: Audio Test

**Goal**: Project builds; ES8311 + amp initialized; ESP32Synth plays a test chord through the speaker.

## Checklist

### Phase 1: Verification & Setup
- [ ] Obtain Waveshare ESP32-P4-WIFI6-Touch-LCD-4C BSP
  - Source: https://github.com/waveshare/ESP32-P4
  - Extract pin definitions for I2S, I2C, speaker amp GPIO
  - Update `main/main.c` with correct pins

- [ ] Install ESP-IDF v5.3+ with esp32p4 target
  ```bash
  git clone https://github.com/espressif/esp-idf.git ~/.esp-idf
  cd ~/.esp-idf && git checkout v5.3
  ./install.sh esp32p4
  source export.sh
  ```

- [ ] Add ESP32Synth as component
  ```bash
  cd components
  git clone https://github.com/danilogcrf2-oss/ESP32Synth.git esp32synth
  cd esp32synth && git checkout <latest stable>
  ```

### Phase 2: Build & Integration
- [ ] Update `main/main.c`
  - Confirm I2S pins from BSP
  - Confirm I2C pins and codec address
  - Confirm speaker amp enable pin and polarity
  - Replace TODO codec init with actual ES8311 setup (from BSP or minimal I2C)

- [ ] Update `CMakeLists.txt`
  - Remove generic component list, add only what's needed:
    - `esp_timer`, `driver`, `freertos`
    - Waveshare BSP component (if available) or manual I2C/I2S
    - `esp32_synth` (from components/)
  - Verify no build conflicts between I2S setup and codec driver

- [ ] Configure ESP-IDF
  ```bash
  cd /Users/andydrag/Documents/GitRepos/wavesharesynth
  idf.py set-target esp32p4
  idf.py menuconfig
  # Set: Component config → ESP32Synth → audio task core (Core 1)
  #      Component config → Freertos → enable static allocation if needed
  ```

- [ ] Build
  ```bash
  idf.py build
  ```

### Phase 3: Flash & Test
- [ ] Connect board via USB-UART and identify `/dev/ttyUSB*` port

- [ ] Flash and monitor
  ```bash
  idf.py -p /dev/ttyUSB0 flash monitor
  ```

- [ ] Verify boot sequence in serial output:
  ```
  I (xxx) WAVESHARESYNTH: === Waveshare Synth Boot ===
  I (xxx) WAVESHARESYNTH: I2C master initialized on SDA=17, SCL=18
  I (xxx) WAVESHARESYNTH: Speaker amp enabled on GPIO 41
  I (xxx) WAVESHARESYNTH: ES8311 codec initialized
  I (xxx) WAVESHARESYNTH: I2S initialized: BCK=8, WS=9, DO=10, DI=11, MCLK=12
  I (xxx) WAVESHARESYNTH: ESP32Synth initialized: 8 voices at 32000 Hz
  I (xxx) WAVESHARESYNTH: === Boot complete ===
  I (xxx) WAVESHARESYNTH: Test chord playing (C major: C4, E4, G4)
  ```

- [ ] **Listen test** (critical)
  - Hold ear close to speaker
  - Should hear a smooth C major chord (C4=262 Hz, E4=330 Hz, G4=392 Hz)
  - After ~2 seconds: silence
  - Loop repeats
  - No digital noise, stutters, or distortion

### Phase 4: Troubleshooting (if audio doesn't play)
- [ ] Check serial output for errors
  - Any ESP_LOGE messages → note them, fix pins/config
  - If I2S won't initialize: verify BCK, WS, DO, DI, MCLK pins match board

- [ ] Verify speaker amp
  - Measure GPIO 41 on oscilloscope: should be 3.3V high (or low, depending on polarity)
  - Measure speaker output with multimeter: should see DC bias (~1.5V) and AC ripple during audio

- [ ] Verify I2C codec communication
  - i2cdetect from host PC (if USB-UART supports I2C bridging)
  - Or use ESP-IDF's `esp_codec_dev_read()` to check codec is awake

- [ ] Verify I2S output
  - Oscilloscope on BCK, WS, DO pins
  - Should see bit clock, word select, and data signals at ~32 kHz sample rate

### Phase 5: Success Criteria
✅ Milestone 1 is DONE when:
1. Project builds without errors
2. Board boots and initializes all subsystems
3. Speaker plays clear C major chord (1-2 seconds)
4. No glitches, stutters, or noise
5. Serial log shows "Boot complete" and chord status

## Notes
- If using Waveshare BSP directly, may need to add their bsp_esp32_p4 component
- ES8311 power-up sequence may need I2C register writes; check BSP examples
- Speaker amp GPIO polarity: confirm active-high or active-low
- I2S MCLK: some codecs need it, some don't; adjust if codec won't clock

---

**Next Milestone**: Display + touch initialization (Milestone 2)
