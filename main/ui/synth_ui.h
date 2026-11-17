#pragma once

#include <stdint.h>
#include "lvgl.h"

typedef struct {
    void (*note_on)(uint32_t frequency_centi_hz, uint16_t volume);
    void (*note_off)(void);
} synth_ui_audio_callbacks_t;

void synth_ui_init(lv_obj_t *screen, const synth_ui_audio_callbacks_t *audio);
