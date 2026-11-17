#pragma once

#include <stdint.h>
#include "lvgl.h"
#include "preset_env.h"

#define SYNTH_UI_MAX_CONTACTS 5

typedef struct {
    uint32_t frequency_centi_hz;
    uint16_t volume;
    bool pressed;
    uint32_t trigger;
    bool accent;
    bool slide;
    bool sustain;
} synth_ui_note_t;

typedef struct {
    void (*note_on)(uint32_t frequency_centi_hz, uint16_t volume);
    void (*note_off)(void);
    void (*select_preset)(uint16_t preset);
    void (*set_notes)(const synth_ui_note_t *notes);
    void (*set_acid_filter)(uint16_t cutoff_hz, uint8_t resonance);
    void (*set_envelope)(const synth_envelope_t *envelope);
    void (*set_delay)(uint16_t time_ms, uint8_t feedback_pct, uint8_t mix_pct);
} synth_ui_audio_callbacks_t;

void synth_ui_init(lv_obj_t *screen, const synth_ui_audio_callbacks_t *audio);
