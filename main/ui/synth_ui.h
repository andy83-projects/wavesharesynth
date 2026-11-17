#pragma once

#include <stdint.h>
#include "lvgl.h"
#include "preset_env.h"

#define SYNTH_UI_MAX_CONTACTS 5
/* Voice slots sent to audio: 0..4 follow touch contacts, the rest carry notes
 * held on other wedges, each with its own instrument. */
#define SYNTH_UI_MAX_VOICES 16

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
    synth_ui_note_t note;
    uint16_t preset;
    synth_envelope_t envelope;
    /* Non-zero: this held note belongs to an arp group the audio side steps
     * through, starting at arp_start_ms (esp_timer clock). */
    uint8_t arp_group;
    uint32_t arp_start_ms;
    uint32_t arp_base_trigger;
} synth_ui_voice_t;

typedef struct {
    void (*note_on)(uint32_t frequency_centi_hz, uint16_t volume);
    void (*note_off)(void);
    void (*select_preset)(uint16_t preset);
    void (*set_voices)(const synth_ui_voice_t *voices);
    void (*set_acid_filter)(uint16_t cutoff_hz, uint8_t resonance);
    void (*set_envelope)(const synth_envelope_t *envelope);
    void (*set_delay)(uint16_t time_ms, uint8_t feedback_pct, uint8_t mix_pct);
    void (*set_tempo)(uint16_t bpm);
} synth_ui_audio_callbacks_t;

void synth_ui_init(lv_obj_t *screen, const synth_ui_audio_callbacks_t *audio);
