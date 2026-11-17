## Goal

Add physical controls (rotary encoders + a crossfader) and proper audio I/O (1/4" line in, 1/4" line out, 1/8" headphones) to the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C synth. Plugging into the 1/4" out or the headphones must disable the onboard speaker.

## Decisions already made

- **MIDI is BLE only.** No USB MIDI. The P4's USB OTG port is reserved for audio (option B below). BLE MIDI runs on the ESP32-C6 through ESP-Hosted, as already planned.
- **Encoders and crossfader go on I2C** (the board's 4-pin SH1.0 "ESP32-P4 I2C header").
- **The speaker is muted in firmware** from jack-detect signals (NS4150B amp enable, GPIO53, active-high), not only by a jack's mechanical contact.

## Step 0 — GPIO survey (gate: do this first and report back before buying or wiring anything)

- [ ] List every GPIO the 4C breaks out besides the I2C header, and which ones the BSP already uses. Source: Waveshare schematic/BSP, not guesses.
- [ ] Measure and record the I2C header's pin order. Qwiic/STEMMA QT is GND, 3V3, SDA, SCL; Waveshare's may differ. **Don't plug a Qwiic cable in until this is confirmed.**
- [ ] Confirm which devices already share that I2C bus (expected: ES8311 at 0x18, GT9271 touch, possibly others) and their addresses, so new parts don't collide.
- [ ] Decide on audio option A or B from the pin count: A needs about 4–6 free GPIOs (I2S BCK, WS, DOUT, DIN, maybe MCLK, plus jack detect); otherwise go with B.

## Part 1 — Controls over I2C

- Encoders: I2C boards that count steps on their own chip (Adafruit I2C QT Rotary Encoder / seesaw, or M5Stack 8-Encoder) so nothing is lost at a 100–200 Hz poll. Use their INT line if a GPIO is free.
- Crossfader: slide pot (or a contactless optical/hall fader for durability) read by an I2C ADC (ADS1115/ADS1015 Qwiic). Add a small dead zone at each end plus smoothing.
- Firmware: poll from the same Core 0 task as touch. Changes go through the seqlock `PadState` → `applyUiState()` on Core 1, never directly to ESP32Synth.
- Default mappings (make them configurable): encoders = program, instrument, tempo/arp rate, master volume. Crossfader = crossfade bus 1 ↔ bus 2 via `setBusMix`, or synth ↔ line input once Part 2 exists.
- Watch I2C bus load against touch latency. If touch gets sluggish, move the controls to a second I2C controller on free pins.

## Part 2 — Audio I/O

### Option A (preferred if pins allow): WM8960 stereo codec on a second I2S port
- One chip: stereo line in, stereo line out, a headphone amp that drives real headphones, and jack detect. Control over the same I2C bus.
- 1/4" out: line out to a **switched** 1/4" TRS jack (unbalanced). A balanced driver (DRV134 / THAT1646) is out of scope unless asked.
- 1/8" headphones: WM8960 headphone outputs to a switched 3.5 mm jack.
- 1/4" in: line level direct to the WM8960 line input. For guitar/bass, add a high-impedance (~1 MΩ) op-amp buffer. Note it as optional, don't build it by default.
- Keep the onboard ES8311 + speaker for the built-in speaker path, or route everything through the WM8960. Pick one and document why.

### Option B (if not enough GPIOs): class-compliant USB audio interface
- ESP-IDF USB audio host (`usb_host_uac` component) on the P4's USB OTG HS port. Gives 1/4" I/O and headphones with no soldering.
- Interface choice should have line in, line out and headphones, and work without drivers.
- Speaker mute: when the interface is attached and streaming, drive GPIO53 low.

### Both options
- **Jack detect → speaker mute:** when 1/4" out or headphones are plugged in, ramp the output down, pull GPIO53 low, then ramp back up on the new path (no pop). Show an output icon on screen. Put the detect inputs on an I2C GPIO expander (PCF8574) if GPIOs are short.
- **Line input into the synth:** ESP32Synth has no input path. Mix I2S/USB input into the master buffer with `setCustomDSP()` so the effects process it. That gives the real Kaoss Pad behavior, effects over incoming audio.
- **Latency:** ESP32Synth defaults to `SYNTH_DMA_BUF_COUNT 6` × `SYNTH_DMA_BUF_LEN 512`, about 64 ms at 48 kHz, which is too slow for monitoring live input. Target ~3 × 128 (~8 ms), then re-run the display stress test for underruns.
- Run the codec and synth at the same sample rate (48 kHz), with no resampling in the hot path.

## Acceptance criteria

- [ ] Step 0 survey committed as `docs/HARDWARE_PINS.md`, with the option A/B decision.
- [ ] Encoders step reliably (no missed or doubled detents in fast spins) while touch and audio run.
- [ ] Crossfader sweeps smoothly with no zipper noise and stable endpoints.
- [ ] Plugging into the 1/4" out or the headphones mutes the speaker within ~50 ms, with no pop; unplugging restores it.
- [ ] Line in passes through the pad effects; round-trip latency measured and documented (target ≤ 10 ms).
- [ ] No audio glitches during simultaneous touch + encoders + line in + full display redraw.
- [ ] Parts list with links committed to `docs/HARDWARE_BOM.md`; wiring notes in `docs/HARDWARE_WIRING.md`.

## Depends on

Milestones 1–2 (audio out + display/touch running together) and the UI milestone (`docs/issues/milestone-ui-design.md`), so the controls have screens to drive. Independent of BLE MIDI.
