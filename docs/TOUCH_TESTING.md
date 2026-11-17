# Local multi-touch and arpeggiator verification

The 4C firmware uses the GT911-compatible driver with a five-contact limit.
This is the supported software limit, not a claim that ten contacts have been
verified on this panel. The driver supplies tracking IDs; the LVGL adapter maps
them to five independent pointers. Synth voices follow those pointers.

Build locally with ESP-IDF (`idf.py build`). After flashing, always start serial
logging immediately. Current local ESP-IDF is 5.5.6; GitHub CI uses 5.5.5.

## Controls

- Tap or move on the pad: one note per finger, up to five configured voices.
- Hold any pad contact within a 12-pixel radius for 500 ms: start arpeggiation.
- Multiple held notes: ascending pitch order, one note per 125 ms at 120 BPM.
- One held note: root, minor third, fifth, octave.
- Lift all fingers: stop. Hold retains the released notes until switched off.
- Open Instrument: stop all pad notes and arpeggiation. Picker interaction uses
  one pointer at a time and does not start the arpeggiator.
- Acid: with three or more pad fingers down, the average finger position sweeps
  cutoff (left/right, 80–3000 Hz logarithmic) and resonance (bottom/top).
  Pitches and volumes stay fixed while that gesture is active. The readout shows
  cutoff and resonance. Other presets retain normal multi-touch pitch control.
  Acid is now a dedicated monophonic Open303 voice at 32 kHz with 4x oversampling.
  Its pitch range is C1–C3, and finger height does not reduce note volume.
  The arpeggiator accents every fourth step and uses legato slides on two steps
  out of four. The model has a 250 ms filter decay and 9 dB of oversampled drive.
  Other instruments remain polyphonic through ESP32Synth.

Observed on hardware: five simultaneous contacts have been logged. The revised
Open303 voice passed its P4 core-1 benchmark at 866 us average / 1030 us worst per
128 frames (4000 us deadline). Live UI testing reached 3309 us worst per 256
frames (8000 us deadline), with zero numerical faults and no captured panic.
Listening feedback for that first voicing was "improved" but lacking squelch and
bass body; the current voicing addresses that feedback and needs listening confirmation.

## Hardware acceptance checks

1. Add two, then three, four, and five fingers. Check each marker and the log
   `Touch contacts=N highest observed=N`. Only counts actually logged are verified.
2. Lift a middle finger while keeping the others down. Its note must release
   without swapping the pitches of the remaining fingers.
3. Move fingers continuously: notes should follow without starting the arp.
4. Hold still for 500 ms. Check `Arpeggiator started`, the ARP readout, and sound.
5. Change the held chord, release it, and repeat with Hold enabled and disabled.
6. Open the picker while notes are held. Verify silence and normal wheel input.
7. Repeat with Saw, FM bell, and Organ; watch for panics, stuck notes, or distortion.
8. On Acid, place three fingers and drag horizontally/vertically. Verify the
   cutoff/resonance readout, stable pitches, audible sweep, and release behavior.

Compilation and a successful flash do not by themselves verify these hardware
behaviors. Preserve the observed count and any failures in the UI issue.
