# Touch-controlled synth layout

The current firmware uses a centered LVGL touch surface on the 720×720 round
display.

## Surface

- Size: 560×430 pixels
- Alignment: centered horizontally and vertically, with a +30 pixel vertical offset
- Input events: press, drag/pressing, and release

## Sound mapping

Touch coordinates are clamped to the display bounds before conversion:

- X = 0 → 130 Hz
- X = 719 → 1,000 Hz
- Y = 0 → maximum volume
- Y = 719 → minimum volume

Pitch is interpolated linearly across X. Volume is interpolated linearly and
inverted across Y, using the ESP32Synth range 0–255.

## Audio-task handoff

LVGL callbacks do not call the synth directly. They overwrite a one-entry
FreeRTOS command queue with the latest frequency, volume, and pressed state.
The ESP32Synth output callback drains that queue from the audio task, applies
`noteOn`/`noteOff`, renders the current mono block, duplicates it to stereo,
and writes it to the Waveshare BSP ES8311 codec path.

Releasing the surface queues `noteOff`, leaving the synth silent until the next
touch. The repeating startup chord used during the early hardware test is
disabled.

## Hardware verification

The successful firmware was flashed to the ESP32-P4 board and touch-generated
audio was confirmed through the connected speaker.
