# Open303 for ESP32-P4

Original DSP: Robin Schmidt, MIT license (see LICENSE).
Source: https://github.com/RobinSchmidt/Open303
Pinned revision: 313bf0d9ade7c1dcb6b3a74f5ea1780a29d70074

Only Source/DSPCode is vendored; plugin SDK and GUI code are excluded.

Port changes:
- Floating-point storage and arithmetic converted to single precision, including
  literal constants and FFT buffers. P4 has hardware single-precision floating point.
- Wavetable prototype guard allocation corrected from 2048 to 2052 samples.
- Portable integer types and float exponent extraction replace double bit-layout assumptions.
- Fixed-capacity note list replaces dynamic list allocation in note handling.
- Original 4x oversampling and modeled TB_303 filter mode retained.
- Original anti-alias response factored into six second-order sections: the original
  12th-order direct form is unstable in float. Host tests compare impulse responses.
- One-pole initialization and sample-rate bookkeeping corrected.
- Wrapper constrains controls, runs monophonically, and exposes rendering for host tests
  and real-device deadline measurements.
- Acid-techno voicing: bass register, stronger resonance taper, 250 ms filter decay,
  accents, 60 ms slides, and 9 dB of post-filter drive before 4x decimation.
- No note-list allocation in the audio callback. Wavetables/FFT storage allocate
  once during startup, before audio playback starts.

## Verification

Compile `dsp/*.cpp`, `acid_voice.cpp`, and `tests/test_open303.cpp` with C++17,
`-fno-strict-aliasing`, and this component's include directories. Address/undefined
sanitizers are supported for host tests. `tests/render_open303.cpp` writes an
8-second WAV reference for listening and clipping checks.

Firmware performs a 128-block benchmark on P4 core 1 before starting audio.
It requires zero numerical faults and worst render below 3200 us per 128 samples
(4000 us deadline at 32 kHz). Runtime logs measure rendering alongside live UI input.

The wrapper is specific to this firmware. This is an Open303 port, not Roland code
or a claim of bit-identical TB-303 emulation.
