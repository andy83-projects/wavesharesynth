# UI style

Visual and interaction spec for the two screens on the 720×720 round panel. Constants
live in `main/ui/ui_theme.h`; interactive references are in `docs/mockups/` (open in a
browser). When this doc and the header disagree, the header wins.

## Look

- Dark screen `#161615`, one amber accent `#EF9F27` (light `#FAC775`, text-on-amber
  `#412402`). Everything else is white at reduced opacity.
- Flat: no gradients, shadows or glow. The halo around the touch point is a translucent
  solid circle.
- Montserrat (LVGL built-in), sentence case everywhere.
- Geometry: angles measured from 12 o'clock, clockwise; centre (360, 360).

## Screen 1 — Pad (default)

Mockup: `docs/mockups/pad.html`

| Element | Spec |
|---|---|
| Grid | Square grid every 60 px, 2 px lines, white ~7%, clipped to the circle. Pre-render as a static background image. |
| Pad edge | Hairline ring r=302, 2 px, white ~12%. Touches clamp to r=300. |
| Program ring | 12 arc segments, r=338, 18 px wide, 30° each with ~2° gaps. Active = amber, others white ~13%. |
| Title | Program name, centred at y=100, 30 px, white ~88%. |
| Subtitle | `P04 · CPU 28%` at y=134, 22 px, white ~50%. CPU from `getCPULoad()`. |
| Y-axis label | Left side, x=92, rotated −90°, e.g. `Resonance ↑`, 16 px, white ~35%. |
| X-axis label | y=572, e.g. `Cutoff →`, 16 px, white ~35%. |
| Readout | y=612, `Cutoff 1.2 kHz · resonance Q 4.1`, 20 px, white ~50%. Shows `Touch to play` when idle. |
| Hold pill | Centred (360, 520), 92×32, radius 16. Off: white ~10% fill, text ~60%. On: amber fill, `#412402` text. |
| Touch point | Halo r=40 light-amber ~25%, core r=24 light-amber, dot r=10 white. Crosshair lines through it, 2 px amber ~35%, full width/height. |
| Light trail | Pool of 20 reused circles. Each lives 700 ms, radius 34 → 4 px, opacity ~45% → 0, amber. Use `lv_anim`, not a full-canvas redraw. |
| Scale lanes | Note programs only: 11 vertical lanes across x 60–660 (C minor pentatonic C3–C5), note names at y=470 (16 px, ~30%). Active lane gets an amber ~12% fill; X snaps to lane centre. Horizontal grid lines stay. |

Behaviour:
- Lifting the finger releases the sound unless Hold is on (Hold latches the last position).
- X/Y are normalised over 60–660 px; Y is inverted (top = max).
- Long-press the centre for 600 ms → instrument picker.
- Touch is sampled at ≥ 100 Hz on Core 0. Values reach the synth only through the
  seqlock → `applyUiState()` path on Core 1. Smooth parameters over ~5 ms on the audio side.

### Programs

| # | Program | X | Y | Lanes |
|---|---|---|---|---|
| 1 | Filter sweep | Cutoff (log 20 Hz–20 kHz) | Resonance (Q 0.5–12) | |
| 2 | Band sweep | Center | Width | |
| 3 | Acid | Cutoff | Resonance | |
| 4 | Touch scale | Note | Filter | ✓ |
| 5 | FM bell | Ratio (1:1–8:1) | Mod index | |
| 6 | Arp | Rate (400–40 ms/step) | Span (1–3 oct) | |
| 7 | Vibrato | Rate (0.5–16 Hz) | Depth (cents) | |
| 8 | Tremolo gate | Rate | Depth | |
| 9 | PWM lead | Note | Pulse width | ✓ |
| 10 | Bitcrush | Bits (16 → 3) | Cutoff | |
| 11 | Pluck | Note | Damping | ✓ |
| 12 | Organ | Note | Reverb | ✓ |

## Screen 2 — Instrument picker (iPod-style click wheel)

Mockup: `docs/mockups/picker.html`

**Silent.** No click on scroll, no auto-audition on hover. The only sound the picker makes
is when you tap Audition.

| Element | Spec |
|---|---|
| Wheel band | r 235–335 (draw one arc r=285, 100 px). White ~5% idle, ~9% while a wheel gesture is live. |
| Detent ticks | 15 ticks every 24°, r 326–336, 3 px, white ~15%. On each step the tick nearest the finger flashes amber at 5 px for 180 ms. |
| Scroll arc | Outer edge r=350, 6 px, track spans ±57° around 12 o'clock (white ~10%); amber thumb ±7° shows list position. |
| Edge labels | At r=287: `Menu` (top), `Audition` (bottom), `LV_SYMBOL_PREV` (left), `LV_SYMBOL_NEXT` (right). 22 px, white ~60%. |
| Breadcrumb | y=182, `Instruments › Keys`, 18 px, white ~45%. |
| List | Clipped to y 210–510. Rows 58 px apart. Selected row: amber pill 320×52, radius 26, text 26 px medium `#412402`. Other rows 26 px white, fading from ~75% at centre to a ~15% floor over 220 px. Categories end with `›`. |
| Footer | y=542, `Tap center to select`, 16 px, white ~35%. Replaced for 1.6 s by a light-amber toast (`Loaded Organ B3`). |
| List motion | Animate the list y-offset toward the selected row (~150 ms ease-out); never jump. |

Gestures:
- A touch that starts at r ≥ 235 is a wheel gesture and stays one until release. Track
  the finger angle with `atan2` around the centre, unwrap deltas, and step one item per 24°.
- A wheel touch that moves < 8° in total is a tap on an edge button (±30° zones):
  top = Menu/back, bottom = Audition, left/right = previous/next program.
- A tap inside the band selects: opens a category, or loads the instrument and returns to the pad.

Menu tree:

- Synth: Saw lead, Pulse lead, Filter synth, Acid bass, Sub bass
- Keys: FM e-piano, FM bell, Organ B3, Organ + Leslie
- Plucked: Nylon guitar, Harp, Pizzicato
- Drums: Noise hat, Noise snare, Sine kick
- Wavetables: Glass, Formant, Digital
- Samples: placeholders until real samples are added

## LVGL notes

- Enable fonts in menuconfig: `CONFIG_LV_FONT_MONTSERRAT_{16,18,20,22,26,30}=y`.
- Draw static layers (grid, ring track, wheel band) once into an image; animate only small
  objects (trail dots, ticks, list) so LVGL redraws small areas and DSI/PSRAM traffic stays
  low for the audio core.
- `→ ↑ ›` aren't in the built-in Montserrat subset. Add them with a custom font
  (`lv_font_conv`) or swap for `LV_SYMBOL_RIGHT` / `LV_SYMBOL_UP`.
