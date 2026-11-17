#pragma once

#include <stdint.h>
#include "lvgl.h"

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
} synth_ui_audio_callbacks_t;

void synth_ui_init(lv_obj_t *screen, const synth_ui_audio_callbacks_t *audio);
