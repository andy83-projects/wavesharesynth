#pragma once

#include <stdint.h>

/* Amplitude envelope in ms (sustain 0-255). The Open303 presets (3, 5) have no
 * attack/sustain stage: decay is the note decay and filter_decay drives the filter. */
typedef struct {
    uint16_t attack;
    uint16_t decay;
    uint8_t sustain;
    uint16_t release;
    uint16_t filter_decay;
} synth_envelope_t;

static inline bool synth_envelope_equal(const synth_envelope_t &a, const synth_envelope_t &b)
{
    return a.attack == b.attack && a.decay == b.decay && a.sustain == b.sustain &&
           a.release == b.release && a.filter_decay == b.filter_decay;
}

/* Factory envelope for each preset; also each wedge's default when assigned. */
static inline synth_envelope_t preset_envelope(uint16_t preset)
{
    switch(preset) {
    case 2: return {15, 250, 140, 200, 0};
    case 3: case 5: return {0, 1230, 0, 0, 250};
    case 6: return {3, 500, 0, 200, 0};
    case 7: return {3, 1000, 0, 200, 0};
    case 12: return {2, 450, 0, 120, 0};
    case 13: return {2, 750, 0, 120, 0};
    case 14: return {2, 130, 0, 120, 0};
    case 18: return {1, 35, 0, 30, 0};
    case 19: return {1, 140, 0, 30, 0};
    case 20: return {1, 220, 0, 30, 0};
    default: return {5, 60, 200, 120, 0};
    }
}
