## Goal

Build the two UI screens (pad + instrument picker) on the device so they match the agreed design. The design is already locked in. This milestone implements it in LVGL, without redesigning anything.

## Design source files (in this repo)

| File | What it is |
|---|---|
| `docs/UI_STYLE.md` | Full visual + interaction spec for both screens, the 12-program table, and the menu tree |
| `main/ui/ui_theme.h` | LVGL 9 constants: palette, opacities, fonts, geometry, timings, gesture thresholds. **If it disagrees with the doc, the header wins.** |
| `docs/mockups/pad.html` | Interactive reference for the pad screen (open in a browser) |
| `docs/mockups/picker.html` | Interactive reference for the click-wheel instrument picker |

> These were added locally on 2026-11-17 and need to be committed/pushed before this issue is picked up. If they're missing from `master`, stop and ask.

## Scope

### 1. Pad screen (default)
- Static background image: square grid clipped to the circle, 12-segment program ring, pad edge hairline.
- Title / `Pxx · CPU nn%` subtitle (CPU from `getCPULoad()`), rotated Y-axis label, X-axis label, value readout, Hold pill.
- Touch point (halo + core + dot) with crosshair lines.
- Light trail as a **pool of 20 reused circle objects with `lv_anim` fades**. Don't redraw the full canvas.
- Scale lanes with X snapping for note programs (4, 9, 11, 12).
- Lift releases the sound unless Hold is on. Long-press the centre (600 ms) opens the picker.

### 2. Instrument picker (iPod-style click wheel)
- **Silent**: no scroll click, no auto-audition. The only sound is an explicit Audition tap.
- Wheel band r 235–335; one item per 24° of rotation; a wheel gesture stays locked until release.
- Detent ticks flash amber per step; scroll-position arc; breadcrumb; amber selected-row pill; list eases toward the selection (~150 ms).
- Edge taps (< 8° movement, ±30° zones): top = Menu/back, bottom = Audition, left/right = prev/next program.
- Tap inside the band selects (opens a category, or loads the instrument and returns to the pad).
- Menu tree and program table come from `docs/UI_STYLE.md`. Keep both data-driven (tables, not hard-coded branches).

## Constraints (from the architecture, don't break these)
- LVGL and touch run on **Core 0**. ESP32Synth audio runs on **Core 1**.
- **No ESP32Synth calls from Core 0.** UI state goes through the seqlock `PadState` and is applied in `applyUiState()` (registered with `setCustomControl`, control rate 500 Hz). ESP32Synth has no locks, and `noteOn`/`slideFreq`/`setArpeggio` race with the audio core otherwise.
- Keep audio buffers in internal SRAM and LVGL buffers in PSRAM. Only redraw small regions so DSI/PSRAM traffic doesn't glitch audio.
- Enable fonts: `CONFIG_LV_FONT_MONTSERRAT_{16,18,20,22,26,30}=y`. Use `LV_SYMBOL_PREV/NEXT/RIGHT/UP` or a custom font for the `◀◀ ▶▶ → ↑ ›` glyphs.
- Use the Waveshare BSP (`waveshare/esp32_p4_wifi6_touch_lcd_xc`) for display/touch/backlight. Don't guess pins.

## Suggested layout
`main/ui/ui_theme.h` (exists), `main/ui/pad_screen.{h,cpp}`, `main/ui/picker_screen.{h,cpp}`, `main/programs/`, `main/presets/`, `main/control/` (seqlock + `applyUiState`).

## Acceptance criteria
- [ ] Side-by-side with the HTML mockups, both screens match colours, geometry, text placement and motion.
- [ ] Dragging continuously on the pad while a 16-voice chord plays causes no audible glitches or I2S underruns (log `getCPULoad()`).
- [ ] Picker makes no sound except on an Audition tap.
- [ ] One full wheel rotation = 15 steps, and tap vs scroll is never misdetected in normal use.
- [ ] All synth calls happen inside `applyUiState()` on Core 1 (verify by code review / an assert on core ID).
- [ ] README milestone list updated, plus a short "what to flash and what to look/listen for" note.

## Depends on
Milestone 1 (audio out through ES8311) and Milestone 2 (display + touch + LVGL alongside audio, with the stress test passing).
