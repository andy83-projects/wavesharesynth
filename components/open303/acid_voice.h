#pragma once

#include <cstdint>
#include <memory>

namespace rosic { class Open303; }

class Open303Voice {
public:
    explicit Open303Voice(float sample_rate = 32000.0f);
    ~Open303Voice();
    void reset();
    void set_controls(uint16_t cutoff_hz, uint8_t resonance);
    void set_waveform(bool square);
    void set_dark(bool dark);
    void set_envelope(float note_decay_ms, float filter_decay_ms, float release_ms);
    void set_sustain(bool enabled);
    void note_on(uint8_t midi_note, bool accent, bool slide, uint16_t volume);
    void note_off();
    void set_volume(uint16_t volume) { target_gain = volume > 255 ? 1.0f : volume / 255.0f; }
    void render(int16_t *output, unsigned frames);
    unsigned fault_count() const { return faults; }
private:
    std::unique_ptr<rosic::Open303> engine;
    bool gate = false;
    bool sounding = false;
    bool sustain = false;
    unsigned quiet_frames = 0;
    unsigned faults = 0;
    float target_cutoff = 350, cutoff = 350;
    float target_resonance = 75, resonance = 75;
    float cutoff_scale = 1.0f;
    float gain = 0.75f, target_gain = 0.75f;
    unsigned control_count = 0;
};
