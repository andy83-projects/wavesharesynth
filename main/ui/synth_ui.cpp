#include "synth_ui.h"
#include "ui_theme.h"

#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

namespace {

struct Program {
    const char *name;
    const char *ring_name;
    const char *x_label;
    const char *y_label;
    uint16_t preset;
};

static const Program kPrograms[UI_PROG_COUNT] = {
    {"Filter sweep", "FILT", "Cutoff", "Resonance", 0},
    {"Band sweep", "BAND", "Center", "Width", 2},
    {"Acid", "Acid", "Cutoff", "Resonance", 3},
    {"Touch scale", "SCALE", "Note", "Filter", 6},
    {"FM bell", "FM", "Ratio", "Mod index", 7},
    {"Arp", "Arp", "Rate", "Span", 1},
    {"Vibrato", "VIB", "Rate", "Depth", 36},
    {"Tremolo gate", "TREM", "Rate", "Depth", 37},
    {"PWM lead", "PWM", "Note", "Pulse width", 1},
    {"Bitcrush", "CRUSH", "Bits", "Cutoff", 26},
    {"Pluck", "Pluck", "Note", "Damping", 12},
    {"Organ", "Organ", "Note", "Reverb", 8},
};

/* Instrument assigned to each ring wedge; user-editable and persisted in NVS. */
struct Slot {
    char name[24];
    char ring[24];
    uint8_t layout;   /* index into kPrograms for axis labels */
    uint16_t preset;
};

static Slot g_slots[UI_PROG_COUNT];
static constexpr const char *kNvsNamespace = "synth_ui";
static constexpr const char *kNvsSlotsKey = "wedges_v1";

/* Notes the pad offers for each wedge: X snaps to `scale` notes from root in
 * `low_octave` upward for `octaves`; SCALE_FREE glides continuously. */
struct NoteSettings {
    uint8_t root;        /* 0 = C .. 11 = B */
    uint8_t scale;       /* index into kScales */
    uint8_t low_octave;  /* MIDI octave of the lowest root (C3 = 3) */
    uint8_t octaves;
};

struct Scale {
    const char *name;
    uint16_t mask;       /* bit n set = n semitones above root allowed */
};

static const Scale kScales[] = {
    {"Free", 0},
    {"Chromatic", 0xFFF},
    {"Major", 0xAB5},
    {"Minor", 0x5AD},
    {"Dorian", 0x6AD},
    {"Pentatonic", 0x295},
    {"Minor pent", 0x4A9},
    {"Blues", 0x4E9},
};
static constexpr uint8_t kScaleCount = sizeof(kScales) / sizeof(kScales[0]);
static constexpr uint8_t SCALE_FREE = 0;
static const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static constexpr uint8_t kMinOctave = 1, kMaxOctave = 6, kMaxOctaves = 4;

static NoteSettings g_notes[UI_PROG_COUNT];
static constexpr const char *kNvsNotesKey = "notes_v1";

/* Per-wedge amplitude envelope; defaults to the instrument's factory envelope. */
static synth_envelope_t g_env[UI_PROG_COUNT];
static constexpr const char *kNvsEnvKey = "env_v1";

/* Per-wedge volume (0-100%), applied to its live notes, layers and loop. */
static uint8_t g_level[UI_PROG_COUNT];
static constexpr const char *kNvsLevelKey = "level_v1";

static uint16_t wedge_volume(uint16_t volume, uint8_t wedge)
{
    return (uint16_t)((uint32_t)volume * g_level[wedge] / 100);
}
static const uint16_t kTimeSteps[] = {0, 1, 2, 5, 10, 20, 35, 50, 75, 100, 150, 200, 300, 450,
                                      600, 800, 1000, 1250, 1500, 2000, 3000};
static constexpr int kTimeStepCount = sizeof(kTimeSteps) / sizeof(kTimeSteps[0]);

/* Notes held on a wedge keep sounding on their instrument after you move to
 * another wedge (or swap this wedge's instrument). Hold is per wedge; turning it
 * off on a wedge releases that wedge's layer. */
/* When an arp started; the audio task steps it from here on its own clock. */
struct ArpClock {
    uint32_t started_ms;    /* esp_timer milliseconds */
    uint32_t base_trigger;
};

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static constexpr unsigned kLayerSlots = SYNTH_UI_MAX_VOICES - SYNTH_UI_MAX_CONTACTS;

/* Notes detached together form a group; an arpeggiating group keeps its own clock. */
struct LayerGroup {
    bool used;
    uint8_t wedge;
    bool arp;
    ArpClock clock;
};

struct HeldVoice {
    bool used;
    uint8_t group;
    synth_ui_voice_t voice;
};
static HeldVoice g_layers[kLayerSlots];
static LayerGroup g_groups[kLayerSlots];
static bool g_wedge_hold[UI_PROG_COUNT];
static bool g_wedge_arp[UI_PROG_COUNT];

/* Tap-to-record sequencer: each wedge records up to 16 steps (one per pad tap,
 * rests from the Rest pill) and loops them on its instrument. */
enum SeqState : uint8_t { SEQ_OFF, SEQ_REC, SEQ_PLAY };
struct SeqPattern {
    uint8_t length;
    synth_seq_step_t steps[SYNTH_UI_SEQ_STEPS];
};
static_assert(SYNTH_UI_MAX_SEQUENCES == UI_PROG_COUNT, "one sequence per wedge");
static SeqPattern g_patterns[UI_PROG_COUNT];
static SeqState g_seq_state[UI_PROG_COUNT];
static uint32_t g_seq_start[UI_PROG_COUNT];
static uint32_t g_seq_trigger[UI_PROG_COUNT];
/* Muted loops keep their place on the beat grid and just stay silent. */
static bool g_seq_muted[UI_PROG_COUNT];
static constexpr const char *kNvsSeqKey = "seq_v1";

/* Global arp tempo and tempo-synced echo, edited on the FX page. */
static uint16_t g_bpm = 120;
static constexpr uint16_t kMinBpm = 60, kMaxBpm = 200, kBpmStep = 5;
static constexpr const char *kNvsBpmKey = "bpm";

struct DelayDivision {
    const char *name;
    uint8_t sixteenths;  /* echo time in sixteenth notes; 0 = off */
};
static const DelayDivision kDelayDivisions[] = {{"Off", 0}, {"1/16", 1}, {"1/8", 2}, {"1/8 dot", 3}, {"1/4", 4}};
static constexpr uint8_t kDelayDivisionCount = sizeof(kDelayDivisions) / sizeof(kDelayDivisions[0]);

struct FxSettings {
    uint8_t division;
    uint8_t feedback_pct;
    uint8_t mix_pct;
};
static FxSettings g_fx = {0, 40, 30};
static constexpr uint8_t kMaxFeedbackPct = 90;
static constexpr const char *kNvsFxKey = "fx_v1";

struct MenuItem {
    const char *name;
    uint8_t program;  /* kPrograms layout for axis labels */
    uint16_t preset;
};

static constexpr uint16_t kNoPreset = 0xFFFF;

struct MenuGroup {
    const char *name;
    MenuItem items[6];
    uint8_t count;
};

static const MenuGroup kMenu[] = {
    {"Synth", {{"Saw lead", 0, 0}, {"Pulse lead", 8, 1}, {"Filter synth", 0, 2}, {"Acid bass", 2, 5}, {"Sub bass", 2, 4}}, 5},
    {"Keys", {{"FM e-piano", 4, 6}, {"FM bell", 4, 7}, {"Organ B3", 11, 8}, {"Organ + Leslie", 11, 9}}, 4},
    {"Plucked", {{"Nylon guitar", 10, 12}, {"Harp", 10, 13}, {"Pizzicato", 10, 14}}, 3},
    {"Drums", {{"Noise hat", 9, 18}, {"Noise snare", 9, 19}, {"Sine kick", 9, 20}}, 3},
    {"Wavetables", {{"Glass", 4, 24}, {"Formant", 0, 25}, {"Digital", 9, 26}}, 3},
    {"Samples", {{"Placeholders", 0, kNoPreset}}, 1},
};

/* Picker group 0 is "Originals": the factory wedge instruments from kPrograms. */
static constexpr uint8_t kGroupCount = 1 + sizeof(kMenu) / sizeof(kMenu[0]);
static constexpr uint8_t kPickerRows = 6;

static const char *group_name(uint8_t g) { return g == 0 ? "Originals" : kMenu[g - 1].name; }
static uint8_t group_size(uint8_t g) { return g == 0 ? UI_PROG_COUNT : kMenu[g - 1].count; }
static MenuItem group_item(uint8_t g, uint8_t i)
{
    if(g == 0) return {kPrograms[i].name, i, kPrograms[i].preset};
    return kMenu[g - 1].items[i];
}

struct UiState {
    struct Contact {
        lv_indev_t *device = nullptr;
        bool down = false;
        bool ring = false;
        bool long_fired = false;
        uint32_t stationary_since = 0;
        uint32_t pressed_at = 0;
        lv_point_t anchor{};
        lv_point_t point{};
        synth_ui_note_t note{};
        lv_obj_t *marker = nullptr;
    } contacts[SYNTH_UI_MAX_CONTACTS];
    synth_ui_audio_callbacks_t audio{};
    lv_obj_t *screen = nullptr;
    lv_obj_t *pad = nullptr;
    lv_obj_t *picker = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *subtitle = nullptr;
    lv_obj_t *readout = nullptr;
    lv_obj_t *hold_button = nullptr;
    lv_obj_t *hold_label = nullptr;
    lv_obj_t *arp_button = nullptr;
    lv_obj_t *arp_label = nullptr;
    lv_obj_t *seq_button = nullptr;
    lv_obj_t *seq_label = nullptr;
    lv_obj_t *touch_halo = nullptr;
    lv_obj_t *touch_core = nullptr;
    lv_obj_t *touch_dot = nullptr;
    lv_obj_t *cross_x = nullptr;
    lv_obj_t *cross_y = nullptr;
    lv_obj_t *trail[UI_TRAIL_POOL]{};
    lv_obj_t *ring = nullptr;                 /* draws all 12 wedge segments */
    lv_obj_t *program_labels[UI_PROG_COUNT]{};
    uint8_t highlighted = 0xFF;               /* wedge currently drawn as selected */
    lv_obj_t *picker_crumb = nullptr;
    lv_obj_t *picker_hint = nullptr;
    lv_obj_t *picker_rows[6]{};
    lv_obj_t *picker_ticks[UI_TICK_COUNT]{};
    lv_obj_t *picker_band = nullptr;
    lv_obj_t *picker_scroll = nullptr;
    uint8_t program = 0;        /* selected wedge */
    uint8_t picker_slot = 0;    /* wedge the open picker will reassign */
    uint8_t notes_slot = 0;     /* wedge the note editor is editing */
    uint8_t last_tap_wedge = 0xFF;
    uint32_t last_tap_at = 0;
    lv_obj_t *notes_panel = nullptr;
    lv_obj_t *notes_title = nullptr;
    lv_obj_t *notes_values[5]{};
    lv_obj_t *notes_rows[5][2]{};   /* < and > buttons per row */
    lv_obj_t *notes_tabs[3]{};
    uint8_t notes_page = 0;         /* 0 = Notes, 1 = Sound, 2 = FX */
    uint16_t active_preset = 0;
    bool acid_sweep = false;
    uint16_t acid_cutoff = 350;
    uint8_t acid_resonance = 140;
    uint8_t menu_group = 0;
    uint8_t menu_item = 0;
    uint8_t list_first = 0;     /* first menu entry shown in the 6-row window */
    bool list_touch = false;    /* current picker press started on the list */
    bool list_drag = false;     /* ...and has moved far enough to scroll */
    uint8_t drag_first = 0;
    bool picker_in_group = false;
    bool hold = false;
    bool touching = false;
    lv_timer_t *audition_timer = nullptr;
    lv_indev_t *picker_owner = nullptr;
    bool arp = false;
    bool arp_enabled = false;
    ArpClock arp_clock{};
    uint32_t trigger = 0;
    bool wheel = false;
    lv_point_t press_point{};
    float last_angle = 0.0f;
    float wheel_degrees = 0.0f;
    float wheel_travel = 0.0f;
    uint8_t trail_index = 0;
};

static UiState g_ui;

static lv_color_t col(uint32_t hex) { return lv_color_hex(hex); }

static void style_base(lv_obj_t *obj)
{
    /* This UI supplies its own styles. Remove theme transitions and states,
     * and prevent LVGL's built-in scrolling from stealing wheel gestures. */
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

/* Visible buttons: no transparent pressed/focused overrides, and no focus on
 * click, otherwise the pill vanishes after every tap. */
static void style_button(lv_obj_t *obj)
{
    style_base(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

static void style_plain(lv_obj_t *obj)
{
    style_base(obj);
    /* These objects are also used as transparent touch layers. Do not let the
     * default LVGL theme paint its pressed/focused color over the synth UI. */
    const lv_style_selector_t active = LV_PART_MAIN | LV_STATE_PRESSED;
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, active);
    lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, active);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, active);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, active);
    lv_obj_set_style_bg_color(obj, col(UI_COL_BG), active);
    lv_obj_set_style_border_color(obj, col(UI_COL_BG), active);
    lv_obj_set_style_border_width(obj, 0, active);
    lv_obj_set_style_radius(obj, 0, active);
    lv_obj_set_style_outline_color(obj, col(UI_COL_BG), active);
    lv_obj_set_style_outline_width(obj, 0, active);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(obj, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUSED);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       lv_opa_t opa)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, col(UI_COL_FG), 0);
    lv_obj_set_style_text_opa(obj, opa, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

static lv_obj_t *circle(lv_obj_t *parent, int diameter, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    style_plain(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(obj, diameter, diameter);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    return obj;
}

static void hide(lv_obj_t *obj)
{
    if(obj) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void show(lv_obj_t *obj)
{
    if(obj) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void load_slots(void)
{
    for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) {
        Slot &s = g_slots[i];
        snprintf(s.name, sizeof(s.name), "%s", kPrograms[i].name);
        snprintf(s.ring, sizeof(s.ring), "%s", kPrograms[i].ring_name);
        s.layout = i;
        s.preset = kPrograms[i].preset;
    }
    nvs_handle_t nvs;
    if(nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    Slot stored[UI_PROG_COUNT];
    size_t size = sizeof(stored);
    if(nvs_get_blob(nvs, kNvsSlotsKey, stored, &size) == ESP_OK && size == sizeof(stored)) {
        for(auto &s : stored) {
            s.name[sizeof(s.name) - 1] = s.ring[sizeof(s.ring) - 1] = '\0';
            if(s.layout >= UI_PROG_COUNT) s.layout = 0;
        }
        memcpy(g_slots, stored, sizeof(g_slots));
        ESP_LOGI("SYNTH_UI", "Loaded wedge assignments from NVS");
    }
    nvs_close(nvs);
}

/* Presets played by the Open303 voice: 3 = Acid, 5 = darker Acid bass. */
static bool is_acid_preset(uint16_t preset) { return preset == 3 || preset == 5; }

static NoteSettings default_notes(uint16_t preset)
{
    /* The 303 only sounds right in the bass: two chromatic octaves from C1.
     * Everything else glides over C3-C6. */
    return is_acid_preset(preset) ? NoteSettings{0, 1, 1, 2} : NoteSettings{0, SCALE_FREE, 3, 3};
}

static void load_notes(void)
{
    for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) g_notes[i] = default_notes(g_slots[i].preset);
    nvs_handle_t nvs;
    if(nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    NoteSettings stored[UI_PROG_COUNT];
    size_t size = sizeof(stored);
    uint16_t bpm = 0;
    if(nvs_get_u16(nvs, kNvsBpmKey, &bpm) == ESP_OK && bpm >= kMinBpm && bpm <= kMaxBpm) g_bpm = bpm;
    FxSettings fx;
    size_t fx_size = sizeof(fx);
    if(nvs_get_blob(nvs, kNvsFxKey, &fx, &fx_size) == ESP_OK && fx_size == sizeof(fx) &&
       fx.division < kDelayDivisionCount && fx.feedback_pct <= kMaxFeedbackPct && fx.mix_pct <= 100) g_fx = fx;
    if(nvs_get_blob(nvs, kNvsNotesKey, stored, &size) == ESP_OK && size == sizeof(stored)) {
        for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) {
            const NoteSettings &n = stored[i];
            if(n.root < 12 && n.scale < kScaleCount && n.low_octave >= kMinOctave &&
               n.low_octave <= kMaxOctave && n.octaves >= 1 && n.octaves <= kMaxOctaves) g_notes[i] = n;
        }
    }
    nvs_close(nvs);
}

static void send_fx(void)
{
    if(g_ui.audio.set_tempo) g_ui.audio.set_tempo(g_bpm);
    const uint16_t time_ms = (uint16_t)(15000u * kDelayDivisions[g_fx.division].sixteenths / g_bpm);
    if(g_ui.audio.set_delay) g_ui.audio.set_delay(time_ms, g_fx.feedback_pct, g_fx.mix_pct);
}

static void load_patterns(void)
{
    nvs_handle_t nvs;
    if(nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    SeqPattern stored[UI_PROG_COUNT];
    size_t size = sizeof(stored);
    if(nvs_get_blob(nvs, kNvsSeqKey, stored, &size) == ESP_OK && size == sizeof(stored)) {
        for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) if(stored[i].length <= SYNTH_UI_SEQ_STEPS) g_patterns[i] = stored[i];
    }
    nvs_close(nvs);
}

static void save_patterns(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if(err == ESP_OK) {
        err = nvs_set_blob(nvs, kNvsSeqKey, g_patterns, sizeof(g_patterns));
        if(err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if(err != ESP_OK) ESP_LOGE("SYNTH_UI", "Saving sequences failed: %s", esp_err_to_name(err));
}

/* Sequences play the wedge's current instrument and envelope. */
static void send_sequence(uint8_t wedge)
{
    if(!g_ui.audio.set_sequence) return;
    synth_ui_sequence_t seq{};
    seq.playing = g_seq_state[wedge] == SEQ_PLAY && !g_seq_muted[wedge];
    seq.length = g_patterns[wedge].length;
    seq.preset = g_slots[wedge].preset;
    seq.envelope = g_env[wedge];
    seq.start_step = g_seq_start[wedge];
    seq.base_trigger = g_seq_trigger[wedge];
    memcpy(seq.steps, g_patterns[wedge].steps, sizeof(seq.steps));
    for(auto &step : seq.steps) step.volume = wedge_volume(step.volume, wedge);
    g_ui.audio.set_sequence(wedge, &seq);
}

static void load_env(void)
{
    for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) g_env[i] = preset_envelope(g_slots[i].preset);
    memset(g_level, 100, sizeof(g_level));
    nvs_handle_t nvs;
    if(nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t levels[UI_PROG_COUNT];
    size_t level_size = sizeof(levels);
    if(nvs_get_blob(nvs, kNvsLevelKey, levels, &level_size) == ESP_OK && level_size == sizeof(levels)) {
        for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) if(levels[i] <= 100) g_level[i] = levels[i];
    }
    synth_envelope_t stored[UI_PROG_COUNT];
    size_t size = sizeof(stored);
    if(nvs_get_blob(nvs, kNvsEnvKey, stored, &size) == ESP_OK && size == sizeof(stored)) {
        memcpy(g_env, stored, sizeof(g_env));
    }
    nvs_close(nvs);
}

static void save_env(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if(err == ESP_OK) {
        err = nvs_set_blob(nvs, kNvsEnvKey, g_env, sizeof(g_env));
        if(err == ESP_OK) err = nvs_set_blob(nvs, kNvsLevelKey, g_level, sizeof(g_level));
        if(err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if(err != ESP_OK) ESP_LOGE("SYNTH_UI", "Saving envelopes failed: %s", esp_err_to_name(err));
}

static void save_notes(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if(err == ESP_OK) {
        err = nvs_set_blob(nvs, kNvsNotesKey, g_notes, sizeof(g_notes));
        if(err == ESP_OK) err = nvs_set_u16(nvs, kNvsBpmKey, g_bpm);
        if(err == ESP_OK) err = nvs_set_blob(nvs, kNvsFxKey, &g_fx, sizeof(g_fx));
        if(err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if(err != ESP_OK) ESP_LOGE("SYNTH_UI", "Saving note settings failed: %s", esp_err_to_name(err));
}

static void save_slots(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if(err == ESP_OK) {
        err = nvs_set_blob(nvs, kNvsSlotsKey, g_slots, sizeof(g_slots));
        if(err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if(err != ESP_OK) ESP_LOGE("SYNTH_UI", "Saving wedge assignments failed: %s", esp_err_to_name(err));
}

/* Wedge i is centred at LVGL angle i*30 + 270 (0 = 3 o'clock, clockwise). */
static int wedge_start_angle(uint8_t i) { return (i * UI_PROG_SEG_DEG + 270 - UI_PROG_SEG_DEG / 2 + UI_PROG_SEG_GAP_DEG) % 360; }
static int wedge_end_angle(uint8_t i) { return (i * UI_PROG_SEG_DEG + 270 + UI_PROG_SEG_DEG / 2 - UI_PROG_SEG_GAP_DEG) % 360; }

/* Screen box covering wedge i between two radii, padded by `margin_deg`. */
static lv_area_t wedge_box(uint8_t i, int r_in, int r_out, int margin_deg)
{
    const int centre = i * UI_PROG_SEG_DEG + 270;
    const int half = UI_PROG_SEG_DEG / 2 - UI_PROG_SEG_GAP_DEG + margin_deg;
    lv_area_t box = {UI_SCREEN_W, UI_SCREEN_H, 0, 0};
    for(int a = centre - half; a <= centre + half; a += 2) {
        const float rad = a * (float)M_PI / 180.0f;
        for(int r : {r_in, r_out}) {
            const int x = UI_CX + (int)lroundf(cosf(rad) * r), y = UI_CY + (int)lroundf(sinf(rad) * r);
            box.x1 = LV_MIN(box.x1, x); box.y1 = LV_MIN(box.y1, y);
            box.x2 = LV_MAX(box.x2, x); box.y2 = LV_MAX(box.y2, y);
        }
    }
    return box;
}

static void ring_draw_event(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.center = {UI_CX, UI_CY};
    dsc.radius = UI_PROG_RING_R;
    dsc.width = UI_PROG_RING_W;
    for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) {
        const bool selected = i == g_ui.highlighted;
        dsc.start_angle = wedge_start_angle(i);
        dsc.end_angle = wedge_end_angle(i);
        dsc.color = col(selected ? UI_COL_ACCENT : UI_COL_FG);
        dsc.opa = selected ? LV_OPA_COVER : UI_OPA_SEG_IDLE;
        lv_draw_arc(layer, &dsc);
    }
}

/* Repaint only the old and new selected wedges: restyling full-screen ring
 * objects forced a whole-screen redraw and stalled every wedge change. */
static void set_highlighted_wedge(uint8_t wedge)
{
    if(wedge == g_ui.highlighted) return;
    const uint8_t previous = g_ui.highlighted;
    g_ui.highlighted = wedge;
    for(uint8_t i : {previous, wedge}) {
        if(i >= UI_PROG_COUNT) continue;
        const bool selected = i == wedge;
        lv_area_t box = wedge_box(i, UI_PROG_RING_R - UI_PROG_RING_W - 2, UI_PROG_RING_R + 2, 1);
        lv_obj_invalidate_area(g_ui.ring, &box);
        lv_obj_set_style_text_color(g_ui.program_labels[i], col(selected ? UI_COL_BG : UI_COL_FG), 0);
        lv_obj_set_style_text_opa(g_ui.program_labels[i], selected ? LV_OPA_COVER : UI_OPA_TEXT_LABEL, 0);
    }
}

static void update_pad_text(void)
{
    const Slot &slot = g_slots[g_ui.program];
    lv_label_set_text(g_ui.title, slot.name);
    const NoteSettings &n = g_notes[g_ui.program];
    lv_label_set_text_fmt(g_ui.subtitle, "P%02u - %s %s", (unsigned)(g_ui.program + 1),
                          kNoteNames[n.root], kScales[n.scale].name);
    lv_label_set_text(g_ui.readout, "Touch to play");
    set_highlighted_wedge(g_ui.program);
}

static void set_touch_point(int x, int y)
{
    const int halo = UI_TOUCH_HALO_R * 2;
    const int core = UI_TOUCH_CORE_R * 2;
    const int dot = UI_TOUCH_DOT_R * 2;
    lv_obj_set_pos(g_ui.touch_halo, x - UI_TOUCH_HALO_R, y - UI_TOUCH_HALO_R);
    lv_obj_set_pos(g_ui.touch_core, x - UI_TOUCH_CORE_R, y - UI_TOUCH_CORE_R);
    lv_obj_set_pos(g_ui.touch_dot, x - UI_TOUCH_DOT_R, y - UI_TOUCH_DOT_R);
    lv_obj_set_pos(g_ui.cross_x, 0, y - UI_CROSSHAIR_W / 2);
    lv_obj_set_pos(g_ui.cross_y, x - UI_CROSSHAIR_W / 2, 0);
    lv_obj_set_size(g_ui.touch_halo, halo, halo);
    lv_obj_set_size(g_ui.touch_core, core, core);
    lv_obj_set_size(g_ui.touch_dot, dot, dot);
    show(g_ui.touch_halo);
    show(g_ui.touch_core);
    show(g_ui.touch_dot);
    show(g_ui.cross_x);
    show(g_ui.cross_y);
}

static void trail_exec(void *obj, int32_t value)
{
    lv_obj_t *trail = static_cast<lv_obj_t *>(obj);
    lv_obj_set_style_opa(trail, (lv_opa_t)value, 0);
    int diameter = UI_TRAIL_R_END * 2 +
                   ((UI_TRAIL_R_START - UI_TRAIL_R_END) * value) / UI_OPA_TRAIL_START;
    if(diameter < UI_TRAIL_R_END * 2) diameter = UI_TRAIL_R_END * 2;
    lv_obj_set_size(trail, diameter, diameter);
    lv_obj_set_style_radius(trail, LV_RADIUS_CIRCLE, 0);
    lv_obj_align_to(trail, g_ui.touch_dot, LV_ALIGN_CENTER, 0, 0);
}

static void add_trail(int x, int y)
{
    lv_obj_t *trail = g_ui.trail[g_ui.trail_index++ % UI_TRAIL_POOL];
    lv_obj_set_pos(trail, x - UI_TRAIL_R_START, y - UI_TRAIL_R_START);
    lv_anim_del(trail, trail_exec);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, trail);
    lv_anim_set_values(&anim, UI_OPA_TRAIL_START, 0);
    lv_anim_set_duration(&anim, UI_TRAIL_LIFE_MS);
    lv_anim_set_exec_cb(&anim, trail_exec);
    lv_anim_set_completed_cb(&anim, [](lv_anim_t *a) {
        hide(static_cast<lv_obj_t *>(a->var));
    });
    show(trail);
    lv_anim_start(&anim);
}

static uint32_t midi_centi_hz(float midi)
{
    return (uint32_t)(44000.0f * powf(2.0f, (midi - 69.0f) / 12.0f));
}

static uint32_t pad_frequency(int x)
{
    const NoteSettings &n = g_notes[g_ui.program];
    const float t = (float)(x - UI_MAP_MIN) / (UI_MAP_MAX - UI_MAP_MIN);
    const int low = 12 * (n.low_octave + 1) + n.root;
    const int span = 12 * n.octaves;
    if(n.scale == SCALE_FREE) return midi_centi_hz(low + t * span);
    /* Equal-width zones per available note, top root included. */
    uint8_t notes[12 * kMaxOctaves + 1];
    int count = 0;
    for(int step = 0; step <= span; ++step) {
        if(kScales[n.scale].mask & (1u << (step % 12))) notes[count++] = (uint8_t)(low + step);
    }
    int index = (int)(t * count);
    if(index >= count) index = count - 1;
    return midi_centi_hz(notes[index]);
}

static synth_ui_note_t pad_note(int x, int y, uint32_t trigger)
{
    x = x < UI_MAP_MIN ? UI_MAP_MIN : (x > UI_MAP_MAX ? UI_MAP_MAX : x);
    y = y < UI_MAP_MIN ? UI_MAP_MIN : (y > UI_MAP_MAX ? UI_MAP_MAX : y);
    const uint32_t freq = pad_frequency(x);
    uint16_t volume = (uint16_t)(((uint32_t)(UI_MAP_MAX - y) * 255u) /
                                 (UI_MAP_MAX - UI_MAP_MIN));
    if(is_acid_preset(g_ui.active_preset)) volume = 200;
    return {freq, volume, true, trigger, false, false, true};
}

static void update_acid_gesture(void)
{
    unsigned count = 0;
    int x = 0, y = 0;
    for(const auto &contact : g_ui.contacts) if(contact.down && !contact.ring) {
        ++count; x += contact.point.x; y += contact.point.y;
    }
    const bool sweep = is_acid_preset(g_ui.active_preset) && count >= 3;
    if(sweep != g_ui.acid_sweep) ESP_LOGI("SYNTH_UI", "Acid sweep=%d contacts=%u", sweep, count);
    g_ui.acid_sweep = sweep;
    if(!sweep) return;
    float nx = ((float)x / count - UI_MAP_MIN) / (UI_MAP_MAX - UI_MAP_MIN);
    float ny = ((float)y / count - UI_MAP_MIN) / (UI_MAP_MAX - UI_MAP_MIN);
    nx = fmaxf(0.0f, fminf(1.0f, nx));
    ny = fmaxf(0.0f, fminf(1.0f, ny));
    g_ui.acid_cutoff = (uint16_t)(80.0f * powf(37.5f, nx));
    g_ui.acid_resonance = (uint8_t)((1.0f - ny) * 255);
    if(g_ui.audio.set_acid_filter) g_ui.audio.set_acid_filter(g_ui.acid_cutoff, g_ui.acid_resonance);
}

static void publish_contacts(void)
{
    update_acid_gesture();
    synth_ui_note_t notes[SYNTH_UI_MAX_CONTACTS]{};
    unsigned count = 0;
    for(unsigned i = 0; i < SYNTH_UI_MAX_CONTACTS; ++i) {
        notes[i] = g_ui.contacts[i].note;
        if(notes[i].pressed) ++count;
    }
    if(count > 0 && g_ui.arp_enabled && !g_ui.arp) {
        g_ui.arp = true;
        g_ui.arp_clock = {now_ms(), ++g_ui.trigger};
        ESP_LOGI("SYNTH_UI", "Arp started explicitly with %u held notes", count);
    }
    if(count == 0) {
        g_ui.arp = false;
        lv_label_set_text(g_ui.readout, "Touch to play");
    } else if(g_ui.arp) {
        lv_label_set_text_fmt(g_ui.readout, "ARP - %u held - %u BPM", count, g_bpm);
    } else {
        lv_label_set_text_fmt(g_ui.readout, "%u note%s", count, count == 1 ? "" : "s");
    }
    if(g_seq_state[g_ui.program] == SEQ_REC) {
        lv_label_set_text_fmt(g_ui.readout, "REC - step %u/%u", g_patterns[g_ui.program].length + 1, SYNTH_UI_SEQ_STEPS);
    }
    if(g_ui.acid_sweep) lv_label_set_text_fmt(g_ui.readout, "ACID%s - %u Hz - res %u%%",
        g_ui.arp ? " ARP" : "", g_ui.acid_cutoff, g_ui.acid_resonance * 100 / 255);

    /* Only sounding notes are sent; audio counts each occupied slot (an arp
     * group as one) so the arp's off-gates don't pump other voices' level. */
    synth_ui_voice_t voices[SYNTH_UI_MAX_VOICES]{};
    for(unsigned i = 0; i < SYNTH_UI_MAX_CONTACTS; ++i) {
        if(!notes[i].pressed) continue;
        voices[i] = {notes[i], g_ui.active_preset, g_env[g_ui.program],
                     (uint8_t)(g_ui.arp ? 1 : 0), g_ui.arp_clock.started_ms, g_ui.arp_clock.base_trigger};
        voices[i].note.volume = wedge_volume(notes[i].volume, g_ui.program);
    }
    for(unsigned i = 0; i < kLayerSlots; ++i) {
        if(!g_layers[i].used) continue;
        const LayerGroup &group = g_groups[g_layers[i].group];
        synth_ui_voice_t &voice = voices[SYNTH_UI_MAX_CONTACTS + i];
        voice = g_layers[i].voice;
        voice.note.volume = wedge_volume(voice.note.volume, group.wedge);
        /* Arp group ids: 1 = live contacts, 2.. = background layer groups. */
        voice.arp_group = group.arp ? (uint8_t)(2 + g_layers[i].group) : 0;
        voice.arp_start_ms = group.clock.started_ms;
        voice.arp_base_trigger = group.clock.base_trigger;
    }
    if(g_ui.audio.set_voices) g_ui.audio.set_voices(voices);
}

static void update_arp_button(void)
{
    /* While recording a sequence the Arp pill becomes Rest. */
    if(g_seq_state[g_ui.program] == SEQ_REC) {
        lv_label_set_text(g_ui.arp_label, "Rest");
        lv_obj_set_style_bg_color(g_ui.arp_button, col(UI_COL_FG), 0);
        lv_obj_set_style_bg_opa(g_ui.arp_button, 150, 0);
        return;
    }
    lv_label_set_text(g_ui.arp_label, g_ui.arp_enabled ? "Arp on" : "Arp off");
    lv_obj_set_style_bg_color(g_ui.arp_button, col(g_ui.arp_enabled ? UI_COL_ACCENT : UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.arp_button, g_ui.arp_enabled ? LV_OPA_COVER : 150, 0);
}

static void update_hold_button(void);

static void update_seq_button(void)
{
    const uint8_t w = g_ui.program;
    const unsigned length = g_patterns[w].length;
    switch(g_seq_state[w]) {
    case SEQ_REC:
        lv_label_set_text_fmt(g_ui.seq_label, "Rec %u", length);
        lv_obj_set_style_bg_color(g_ui.seq_button, col(UI_COL_REC), 0);
        lv_obj_set_style_bg_opa(g_ui.seq_button, LV_OPA_COVER, 0);
        break;
    case SEQ_PLAY:
        lv_label_set_text_fmt(g_ui.seq_label, "Seq %u", length);
        lv_obj_set_style_bg_color(g_ui.seq_button, col(UI_COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(g_ui.seq_button, LV_OPA_COVER, 0);
        break;
    case SEQ_OFF:
        if(length) lv_label_set_text_fmt(g_ui.seq_label, "Seq %u", length);
        else lv_label_set_text(g_ui.seq_label, "Seq");
        lv_obj_set_style_bg_color(g_ui.seq_button, col(UI_COL_FG), 0);
        lv_obj_set_style_bg_opa(g_ui.seq_button, 150, 0);
        break;
    }
}

/* Recording ends in playback (starting on the next 16th of the shared grid),
 * or off if nothing was recorded. */
static void start_playing(uint8_t wedge)
{
    g_seq_state[wedge] = SEQ_PLAY;
    g_seq_muted[wedge] = false;
    g_seq_start[wedge] = now_ms() / (15000u / g_bpm) + 1;
    g_seq_trigger[wedge] = ++g_ui.trigger;
    send_sequence(wedge);
}

static void start_recording(uint8_t wedge)
{
    g_patterns[wedge].length = 0;
    g_seq_state[wedge] = SEQ_REC;
    send_sequence(wedge);
}

static void finish_recording(uint8_t wedge)
{
    save_patterns();  /* an empty recording clears the saved loop */
    ESP_LOGI("SYNTH_UI", "Wedge %u sequence: %u steps", wedge + 1, g_patterns[wedge].length);
    if(g_patterns[wedge].length) {
        start_playing(wedge);
    } else {
        g_seq_state[wedge] = SEQ_OFF;
        send_sequence(wedge);
    }
}

static void record_step(uint32_t frequency_centi_hz, uint16_t volume, uint8_t flags)
{
    const uint8_t w = g_ui.program;
    SeqPattern &pattern = g_patterns[w];
    if(g_seq_state[w] != SEQ_REC || pattern.length >= SYNTH_UI_SEQ_STEPS) return;
    pattern.steps[pattern.length++] = {frequency_centi_hz, volume, flags};
    if(pattern.length == SYNTH_UI_SEQ_STEPS) {
        finish_recording(w);
        update_arp_button();
        update_hold_button();
    }
    update_seq_button();
}

/* Tap: play/stop the saved loop (record if there is none; finish if recording).
 * Long-press: record a new loop over the old one. */
static void seq_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if(code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
    const uint8_t w = g_ui.program;
    if(code == LV_EVENT_LONG_PRESSED) {
        if(g_seq_state[w] == SEQ_REC) return;
        start_recording(w);
    } else {
        switch(g_seq_state[w]) {
        case SEQ_OFF:
            if(g_patterns[w].length) start_playing(w);
            else start_recording(w);
            break;
        case SEQ_REC:
            finish_recording(w);
            break;
        case SEQ_PLAY:
            g_seq_state[w] = SEQ_OFF;
            send_sequence(w);
            break;
        }
    }
    ESP_LOGI("SYNTH_UI", "Wedge %u sequencer state=%u", w + 1, g_seq_state[w]);
    update_seq_button();
    update_arp_button();
    update_hold_button();
}

static void arp_event(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    if(g_seq_state[g_ui.program] == SEQ_REC) {
        record_step(0, 0, 0);
        return;
    }
    g_ui.arp_enabled = !g_ui.arp_enabled;
    g_ui.arp = false;
    update_arp_button();
    ESP_LOGI("SYNTH_UI", "Arp enabled=%d", g_ui.arp_enabled);
}

static void stop_contacts(void)
{
    for(auto &contact : g_ui.contacts) {
        contact.down = false;
        contact.note.pressed = false;
        hide(contact.marker);
    }
    g_ui.arp = false;
    publish_contacts();
    hide(g_ui.touch_halo); hide(g_ui.touch_core); hide(g_ui.touch_dot);
    hide(g_ui.cross_x); hide(g_ui.cross_y);
}

/* Turn this wedge's held notes into a background layer on its current instrument. */
static void detach_held_notes(void)
{
    if(!g_ui.hold) return;
    LayerGroup *group = nullptr;
    for(auto &candidate : g_groups) if(!candidate.used) { group = &candidate; break; }
    if(!group) return;
    *group = {true, g_ui.program, g_ui.arp_enabled && g_ui.arp, g_ui.arp_clock};
    unsigned moved = 0;
    for(auto &contact : g_ui.contacts) {
        /* Skip notes a finger is still playing on the pad; the finger on the
         * ring may share the contact of the held note. */
        if(!contact.note.pressed || (contact.down && !contact.ring)) continue;
        HeldVoice *slot = nullptr;
        for(auto &layer : g_layers) if(!layer.used) { slot = &layer; break; }
        if(slot) {
            *slot = {true, (uint8_t)(group - g_groups), {contact.note, g_ui.active_preset, g_env[g_ui.program]}};
            ++moved;
        } else {
            ESP_LOGW("SYNTH_UI", "No free layer voice; held note released");
        }
        contact.note.pressed = false;
        hide(contact.marker);
    }
    if(!moved) group->used = false;
    else ESP_LOGI("SYNTH_UI", "Wedge %u: %u held note%s kept as a layer%s", g_ui.program + 1, moved,
                  moved == 1 ? "" : "s", group->arp ? " (arp)" : "");
    g_ui.arp = false;
}

static void release_layers(uint8_t wedge)
{
    for(auto &group : g_groups) {
        if(!group.used || group.wedge != wedge) continue;
        const unsigned g = &group - g_groups;
        for(auto &layer : g_layers) if(layer.used && layer.group == g) layer.used = false;
        group.used = false;
    }
}

static void update_hold_button(void)
{
    /* While this wedge's loop plays, the Hold pill becomes its Mute. */
    if(g_seq_state[g_ui.program] == SEQ_PLAY) {
        const bool muted = g_seq_muted[g_ui.program];
        lv_label_set_text(g_ui.hold_label, muted ? "Muted" : "Mute");
        lv_obj_set_style_bg_color(g_ui.hold_button, col(muted ? UI_COL_REC : UI_COL_FG), 0);
        lv_obj_set_style_bg_opa(g_ui.hold_button, muted ? LV_OPA_COVER : 150, 0);
        return;
    }
    lv_label_set_text(g_ui.hold_label, "Hold");
    lv_obj_set_style_bg_color(g_ui.hold_button, col(g_ui.hold ? UI_COL_ACCENT : UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.hold_button, g_ui.hold ? LV_OPA_COVER : 150, 0);
}

static uint8_t wedge_at(const lv_point_t &point)
{
    float angle = atan2f(point.x - UI_CX, UI_CY - point.y) * 180.0f / (float)M_PI;
    if(angle < 0) angle += 360.0f;
    return (int)((angle + UI_PROG_SEG_DEG / 2) / UI_PROG_SEG_DEG) % UI_PROG_COUNT;
}

static void apply_slot(uint8_t wedge)
{
    if(wedge != g_ui.program) {
        if(g_seq_state[g_ui.program] == SEQ_REC) finish_recording(g_ui.program);
        detach_held_notes();
        g_wedge_hold[g_ui.program] = g_ui.hold;
        g_ui.hold = g_wedge_hold[wedge];
        update_hold_button();
        g_wedge_arp[g_ui.program] = g_ui.arp_enabled;
        g_ui.arp_enabled = g_wedge_arp[wedge];
        g_ui.arp = false;
    }
    g_ui.program = wedge;
    g_ui.active_preset = g_slots[wedge].preset;
    if(g_ui.audio.set_envelope) g_ui.audio.set_envelope(&g_env[wedge]);
    if(g_ui.audio.select_preset) g_ui.audio.select_preset(g_ui.active_preset);
    ESP_LOGI("SYNTH_UI", "Wedge %u: %s (preset %u)", wedge + 1, g_slots[wedge].name, g_ui.active_preset);
    update_pad_text();
    if(g_ui.arp_button) update_arp_button();
    if(g_ui.seq_button) update_seq_button();
    publish_contacts();
}

static void select_ring_program(const lv_point_t &point)
{
    const uint8_t wedge = wedge_at(point);
    if(g_ui.program != wedge) apply_slot(wedge);
}

static void update_picker_rows(void);

enum EditField : uint8_t {
    F_KEY, F_SCALE, F_LOW, F_OCTAVES, F_TEMPO,
    F_ATTACK, F_DECAY, F_SUSTAIN, F_RELEASE, F_FILTER_DECAY,
    F_LEVEL, F_DELAY, F_FEEDBACK, F_MIX, F_NONE
};
static constexpr uint8_t kEditRows = 5;
static constexpr uintptr_t kEditDone = 0xFF, kEditTab = 0xF0;  /* kEditTab + page index */
static constexpr uint8_t kEditPages = 3;

/* Which field each row edits on the current page; the 303 has its own envelope. */
static const EditField *edit_fields(void)
{
    static const EditField notes[kEditRows] = {F_KEY, F_SCALE, F_LOW, F_OCTAVES, F_NONE};
    static const EditField fx[kEditRows] = {F_TEMPO, F_DELAY, F_FEEDBACK, F_MIX, F_NONE};
    static const EditField sound[kEditRows] = {F_ATTACK, F_DECAY, F_SUSTAIN, F_RELEASE, F_LEVEL};
    static const EditField acid[kEditRows] = {F_DECAY, F_FILTER_DECAY, F_RELEASE, F_LEVEL, F_NONE};
    if(g_ui.notes_page == 0) return notes;
    if(g_ui.notes_page == 2) return fx;
    return is_acid_preset(g_slots[g_ui.notes_slot].preset) ? acid : sound;
}

static uint16_t step_time(uint16_t value, int dir)
{
    int i = 0;
    while(i + 1 < kTimeStepCount && kTimeSteps[i + 1] <= value) ++i;
    if(dir > 0) i = i + 1 < kTimeStepCount ? i + 1 : i;
    else if(kTimeSteps[i] == value) i = i > 0 ? i - 1 : 0;
    return kTimeSteps[i];
}

static void format_time(char *out, size_t size, const char *name, uint16_t ms)
{
    if(ms >= 1000) snprintf(out, size, "%s  %u.%02u s", name, ms / 1000, (ms % 1000) / 10);
    else snprintf(out, size, "%s  %u ms", name, ms);
}

static void update_notes_editor(void)
{
    const NoteSettings &n = g_notes[g_ui.notes_slot];
    const synth_envelope_t &e = g_env[g_ui.notes_slot];
    lv_label_set_text(g_ui.notes_title, g_slots[g_ui.notes_slot].name);
    for(uint8_t tab = 0; tab < kEditPages; ++tab) {
        lv_obj_set_style_bg_color(g_ui.notes_tabs[tab], col(tab == g_ui.notes_page ? UI_COL_ACCENT : UI_COL_FG), 0);
        lv_obj_set_style_bg_opa(g_ui.notes_tabs[tab], tab == g_ui.notes_page ? LV_OPA_COVER : UI_OPA_SEG_IDLE * 2, 0);
    }
    const EditField *fields = edit_fields();
    for(uint8_t row = 0; row < kEditRows; ++row) {
        char text[40] = "";
        switch(fields[row]) {
        case F_KEY: snprintf(text, sizeof(text), "Key  %s", kNoteNames[n.root]); break;
        case F_SCALE: snprintf(text, sizeof(text), "%s", kScales[n.scale].name); break;
        case F_LOW: snprintf(text, sizeof(text), "From  %s%u", kNoteNames[n.root], n.low_octave); break;
        case F_OCTAVES: snprintf(text, sizeof(text), "%u octave%s", n.octaves, n.octaves == 1 ? "" : "s"); break;
        case F_TEMPO: snprintf(text, sizeof(text), "Arp  %u BPM", g_bpm); break;
        case F_ATTACK: format_time(text, sizeof(text), "Attack", e.attack); break;
        case F_DECAY: format_time(text, sizeof(text), "Decay", e.decay); break;
        case F_SUSTAIN: snprintf(text, sizeof(text), "Sustain  %u%%", (e.sustain * 100 + 127) / 255); break;
        case F_RELEASE: format_time(text, sizeof(text), "Release", e.release); break;
        case F_FILTER_DECAY: format_time(text, sizeof(text), "Filter", e.filter_decay); break;
        case F_DELAY: snprintf(text, sizeof(text), "Echo  %s", kDelayDivisions[g_fx.division].name); break;
        case F_FEEDBACK: snprintf(text, sizeof(text), "Repeats  %u%%", g_fx.feedback_pct); break;
        case F_MIX: snprintf(text, sizeof(text), "Echo mix  %u%%", g_fx.mix_pct); break;
        case F_LEVEL: snprintf(text, sizeof(text), "Volume  %u%%", g_level[g_ui.notes_slot]); break;
        case F_NONE: break;
        }
        lv_label_set_text(g_ui.notes_values[row], text);
        for(lv_obj_t *button : g_ui.notes_rows[row]) {
            if(fields[row] == F_NONE) hide(button);
            else show(button);
        }
    }
}

static void open_notes_editor(uint8_t wedge)
{
    detach_held_notes();
    stop_contacts();
    g_ui.notes_slot = wedge;
    g_ui.notes_page = 0;
    update_notes_editor();
    lv_obj_clear_flag(g_ui.notes_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui.pad, LV_OBJ_FLAG_HIDDEN);
    hide(g_ui.hold_button);
    hide(g_ui.arp_button);
    hide(g_ui.seq_button);
    if(g_ui.audio.note_off) g_ui.audio.note_off();
    ESP_LOGI("SYNTH_UI", "Wedge editor opened for wedge %u", wedge + 1);
}

static void notes_button_event(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const uintptr_t action = (uintptr_t)lv_event_get_user_data(event);
    NoteSettings &n = g_notes[g_ui.notes_slot];
    synth_envelope_t &e = g_env[g_ui.notes_slot];
    if(action == kEditDone) {
        save_notes();
        save_env();
        lv_obj_add_flag(g_ui.notes_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_ui.pad, LV_OBJ_FLAG_HIDDEN);
        show(g_ui.hold_button);
        show(g_ui.arp_button);
        show(g_ui.seq_button);
        update_pad_text();
        ESP_LOGI("SYNTH_UI", "Wedge %u: %s %s oct %u x%u; env A%u D%u S%u R%u F%u", g_ui.notes_slot + 1,
                 kNoteNames[n.root], kScales[n.scale].name, n.low_octave, n.octaves,
                 e.attack, e.decay, e.sustain, e.release, e.filter_decay);
        return;
    }
    if(action >= kEditTab && action < kEditTab + kEditPages) {
        g_ui.notes_page = (uint8_t)(action - kEditTab);
        update_notes_editor();
        return;
    }
    const int dir = (action & 1) ? 1 : -1;
    switch(edit_fields()[action >> 1]) {
    case F_KEY: n.root = (n.root + 12 + dir) % 12; break;
    case F_SCALE: n.scale = (n.scale + kScaleCount + dir) % kScaleCount; break;
    case F_LOW:
        if(n.low_octave + dir >= kMinOctave && n.low_octave + dir <= kMaxOctave) n.low_octave += dir;
        break;
    case F_OCTAVES:
        if(n.octaves + dir >= 1 && n.octaves + dir <= kMaxOctaves) n.octaves += dir;
        break;
    case F_TEMPO: {
        const int bpm = g_bpm + dir * kBpmStep;
        if(bpm >= kMinBpm && bpm <= kMaxBpm) g_bpm = (uint16_t)bpm;
        break;
    }
    case F_ATTACK: e.attack = step_time(e.attack, dir); break;
    case F_DECAY: e.decay = step_time(e.decay, dir); break;
    case F_SUSTAIN: {
        int pct = (e.sustain * 100 + 127) / 255 + dir * 10;
        pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
        e.sustain = (uint8_t)(pct * 255 / 100);
        break;
    }
    case F_RELEASE: e.release = step_time(e.release, dir); break;
    case F_FILTER_DECAY: e.filter_decay = step_time(e.filter_decay, dir); break;
    case F_DELAY: g_fx.division = (g_fx.division + kDelayDivisionCount + dir) % kDelayDivisionCount; break;
    case F_FEEDBACK: {
        const int pct = g_fx.feedback_pct + dir * 10;
        if(pct >= 0 && pct <= kMaxFeedbackPct) g_fx.feedback_pct = (uint8_t)pct;
        break;
    }
    case F_MIX: {
        const int pct = g_fx.mix_pct + dir * 10;
        if(pct >= 0 && pct <= 100) g_fx.mix_pct = (uint8_t)pct;
        break;
    }
    case F_LEVEL: {
        const int pct = g_level[g_ui.notes_slot] + dir * 10;
        if(pct >= 0 && pct <= 100) g_level[g_ui.notes_slot] = (uint8_t)pct;
        publish_contacts();  /* re-level this wedge's held layers right away */
        break;
    }
    case F_NONE: break;
    }
    send_fx();
    /* Envelope edits apply live when this wedge is the one playing. */
    if(g_ui.notes_slot == g_ui.program && g_ui.audio.set_envelope) g_ui.audio.set_envelope(&e);
    send_sequence(g_ui.notes_slot);
    update_notes_editor();
}

static lv_obj_t *notes_button(const char *text, int x, int y, int w, uintptr_t action)
{
    lv_obj_t *button = lv_obj_create(g_ui.notes_panel);
    style_button(button);
    lv_obj_set_size(button, w, UI_NOTES_BTN);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, UI_NOTES_BTN / 2, 0);
    lv_obj_set_style_bg_color(button, col(UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(button, UI_OPA_SEG_IDLE * 2, 0);
    lv_obj_set_style_bg_color(button, col(UI_COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *text_label = label(button, text, UI_FONT_LIST, LV_OPA_COVER);
    lv_obj_center(text_label);
    lv_obj_add_event_cb(button, notes_button_event, LV_EVENT_CLICKED, (void *)action);
    return button;
}

static void create_notes_editor(void)
{
    g_ui.notes_panel = lv_obj_create(g_ui.screen);
    style_plain(g_ui.notes_panel);
    lv_obj_set_size(g_ui.notes_panel, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(g_ui.notes_panel, 0, 0);
    lv_obj_set_style_bg_color(g_ui.notes_panel, col(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(g_ui.notes_panel, LV_OPA_COVER, 0);
    g_ui.notes_title = label(g_ui.notes_panel, "", UI_FONT_TITLE, UI_OPA_TEXT_TITLE);
    lv_obj_align(g_ui.notes_title, LV_ALIGN_TOP_MID, 0, UI_NOTES_TABS_Y - 50);
    static const char *const kTabNames[kEditPages] = {"Notes", "Sound", "FX"};
    for(uint8_t tab = 0; tab < kEditPages; ++tab) {
        g_ui.notes_tabs[tab] = notes_button(kTabNames[tab], UI_CX - 195 + tab * 135, UI_NOTES_TABS_Y, 120, kEditTab + tab);
    }
    for(uint8_t row = 0; row < kEditRows; ++row) {
        const int y = UI_NOTES_TOP + row * UI_NOTES_ROW_H;
        g_ui.notes_rows[row][0] = notes_button("<", UI_CX - UI_NOTES_ROW_W / 2, y, UI_NOTES_BTN, (uintptr_t)row << 1);
        g_ui.notes_rows[row][1] = notes_button(">", UI_CX + UI_NOTES_ROW_W / 2 - UI_NOTES_BTN, y, UI_NOTES_BTN,
                                               ((uintptr_t)row << 1) | 1);
        g_ui.notes_values[row] = label(g_ui.notes_panel, "", UI_FONT_LIST, LV_OPA_COVER);
        lv_obj_set_width(g_ui.notes_values[row], UI_NOTES_ROW_W - 2 * UI_NOTES_BTN);
        lv_obj_set_style_text_align(g_ui.notes_values[row], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_ui.notes_values[row], UI_CX - UI_NOTES_ROW_W / 2 + UI_NOTES_BTN, y + 14);
    }
    notes_button("Done", UI_CX - 80, UI_NOTES_TOP + kEditRows * UI_NOTES_ROW_H + 6, 160, kEditDone);
    lv_obj_add_flag(g_ui.notes_panel, LV_OBJ_FLAG_HIDDEN);
}

static void open_picker(uint8_t wedge)
{
    detach_held_notes();
    stop_contacts();
    g_ui.picker_slot = wedge;
    g_ui.picker_owner = nullptr;
    g_ui.picker_in_group = false;
    g_ui.menu_item = 0;
    g_ui.list_first = 0;
    update_picker_rows();
    lv_obj_clear_flag(g_ui.picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui.pad, LV_OBJ_FLAG_HIDDEN);
    /* Hold/Arp live on the screen, not the pad, so hide them explicitly. */
    hide(g_ui.hold_button);
    hide(g_ui.arp_button);
    hide(g_ui.seq_button);
    if(g_ui.audio.note_off) g_ui.audio.note_off();
    ESP_LOGI("SYNTH_UI", "Picker opened for wedge %u", wedge + 1);
}

static void pad_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point{};
    if(indev) lv_indev_get_point(indev, &point);
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    UiState::Contact *contact = nullptr;
    for(auto &candidate : g_ui.contacts) if(candidate.device == indev) { contact = &candidate; break; }
    if(!contact && code == LV_EVENT_PRESSED) {
        for(auto &candidate : g_ui.contacts) if(!candidate.device) { candidate.device = indev; contact = &candidate; break; }
    }
    if(!contact) return;
    contact->point = point;
    if(code == LV_EVENT_PRESSED) {
        const int dx = point.x - UI_CX, dy = point.y - UI_CY;
        contact->ring = dx * dx + dy * dy >= UI_PAD_R * UI_PAD_R;
        contact->down = true;
        contact->anchor = point;
        contact->stationary_since = lv_tick_get();
        /* A ring touch can reuse the contact that still holds a note: leave it alone. */
        if(!contact->ring) {
            contact->note = pad_note(point.x, point.y, ++g_ui.trigger);
            const bool accent = point.y < UI_MAP_MIN + (UI_MAP_MAX - UI_MAP_MIN) * UI_SEQ_ACCENT_PCT / 100;
            record_step(contact->note.frequency_centi_hz, contact->note.volume,
                        SYNTH_SEQ_NOTE | (accent ? SYNTH_SEQ_ACCENT : 0));
        }
    }
    if(code == LV_EVENT_PRESS_LOST) {
        contact->down = false;
        contact->note.pressed = false;
        hide(contact->marker);
        publish_contacts();
        return;
    }
    if(contact->ring) {
        bool pad_active = false;
        for(auto &c : g_ui.contacts) if(c.device != indev && c.down && !c.ring) pad_active = true;
        if(code == LV_EVENT_RELEASED) {
            contact->down = false;
            if(!contact->long_fired && lv_tick_elaps(contact->pressed_at) < UI_TAP_MAX_MS) {
                g_ui.last_tap_wedge = wedge_at(point);
                g_ui.last_tap_at = lv_tick_get();
            }
            return;
        }
        if(pad_active) return;
        if(code == LV_EVENT_PRESSED) {
            contact->long_fired = false;
            contact->pressed_at = lv_tick_get();
            if(wedge_at(point) == g_ui.last_tap_wedge && lv_tick_elaps(g_ui.last_tap_at) < UI_DOUBLE_TAP_MS) {
                g_ui.last_tap_wedge = 0xFF;
                contact->long_fired = true;  /* this press must not also long-press */
                open_notes_editor(wedge_at(point));
                return;
            }
        }
        if(contact->long_fired) return;
        select_ring_program(point);
        const int mx = point.x - contact->anchor.x, my = point.y - contact->anchor.y;
        if(mx * mx + my * my > UI_LONG_PRESS_SLOP * UI_LONG_PRESS_SLOP) {
            contact->anchor = point;
            contact->stationary_since = lv_tick_get();
        } else if(!contact->long_fired && lv_tick_elaps(contact->stationary_since) >= UI_LONG_PRESS_MS) {
            contact->long_fired = true;
            open_picker(wedge_at(point));
        }
        return;
    }
    if(code == LV_EVENT_RELEASED) {
        contact->down = false;
        if(!g_ui.hold) contact->note.pressed = false;
        if(!g_ui.hold) hide(contact->marker);
        publish_contacts();
        g_ui.touching = false;
        if(!g_ui.hold) hide(g_ui.touch_halo), hide(g_ui.touch_core), hide(g_ui.touch_dot),
            hide(g_ui.cross_x), hide(g_ui.cross_y);
        return;
    }
    g_ui.touching = true;
    const int dx = point.x - contact->anchor.x, dy = point.y - contact->anchor.y;
    if(dx * dx + dy * dy > 12 * 12) {
        contact->anchor = point;
        contact->stationary_since = lv_tick_get();
    }
    set_touch_point(point.x, point.y);
    add_trail(point.x, point.y);
    update_acid_gesture();
    if(!g_ui.acid_sweep) contact->note = pad_note(point.x, point.y, contact->note.trigger);
    lv_obj_set_pos(contact->marker, point.x - 12, point.y - 12);
    show(contact->marker);
    publish_contacts();
}


static void hold_event(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const uint8_t w = g_ui.program;
    if(g_seq_state[w] == SEQ_PLAY) {
        g_seq_muted[w] = !g_seq_muted[w];
        send_sequence(w);
        update_hold_button();
        ESP_LOGI("SYNTH_UI", "Wedge %u sequence muted=%d", w + 1, g_seq_muted[w]);
        return;
    }
    g_ui.hold = !g_ui.hold;
    if(g_ui.hold) {
        bool any = false;
        UiState::Contact *last = nullptr;
        for(auto &contact : g_ui.contacts) {
            any |= contact.note.pressed;
            if(!contact.ring && contact.note.frequency_centi_hz &&
               (!last || (int32_t)(contact.note.trigger - last->note.trigger) > 0)) last = &contact;
        }
        if(!any && last) {
            last->note.pressed = true;
            last->note.trigger = ++g_ui.trigger;
            show(last->marker);
        }
        publish_contacts();
    }
    if(!g_ui.hold) {
        for(auto &contact : g_ui.contacts) if(!contact.down) {
            contact.note.pressed = false;
            hide(contact.marker);
        }
        release_layers(g_ui.program);
        publish_contacts();
    }
    update_hold_button();
    ESP_LOGI("SYNTH_UI", "Hold enabled=%d", g_ui.hold);
}

static float point_angle(const lv_point_t &p)
{
    return atan2f((float)p.x - UI_CX, (float)UI_CY - p.y) * 180.0f / (float)M_PI;
}

static float point_radius(const lv_point_t &p)
{
    float dx = (float)p.x - UI_CX;
    float dy = (float)p.y - UI_CY;
    return sqrtf(dx * dx + dy * dy);
}

static float unwrap_delta(float delta)
{
    while(delta > 180.0f) delta -= 360.0f;
    while(delta < -180.0f) delta += 360.0f;
    return delta;
}

static uint8_t picker_count(void)
{
    return g_ui.picker_in_group ? group_size(g_ui.menu_group) : kGroupCount;
}

/* Menu entry under a list tap, or -1. */
static int picker_item_at(const lv_point_t &p)
{
    if(p.x < UI_CX - UI_LIST_SEL_W / 2 || p.x >= UI_CX + UI_LIST_SEL_W / 2 || p.y < 225) return -1;
    const int row = (p.y - 225) / UI_LIST_ROW_H;
    if(row >= kPickerRows) return -1;
    const int index = g_ui.list_first + row;
    return index < picker_count() ? index : -1;
}

static void update_picker_rows(void)
{
    /* Scroll the window only when the selection leaves it, so taps never shift rows. */
    if(g_ui.menu_item < g_ui.list_first) g_ui.list_first = g_ui.menu_item;
    if(g_ui.menu_item >= g_ui.list_first + kPickerRows) g_ui.list_first = g_ui.menu_item - kPickerRows + 1;
    if(g_ui.picker_in_group) {
        lv_label_set_text_fmt(g_ui.picker_crumb, "Wedge %u > %s", g_ui.picker_slot + 1, group_name(g_ui.menu_group));
    } else {
        lv_label_set_text_fmt(g_ui.picker_crumb, "Wedge %u > Instruments", g_ui.picker_slot + 1);
    }
    for(uint8_t i = 0; i < kPickerRows; ++i) {
        const uint8_t index = g_ui.list_first + i;
        const char *text = "";
        if(index < picker_count()) {
            text = g_ui.picker_in_group ? group_item(g_ui.menu_group, index).name : group_name(index);
        }
        lv_label_set_text(g_ui.picker_rows[i], text);
        lv_obj_set_y(g_ui.picker_rows[i], 225 + (int)i * UI_LIST_ROW_H);
        const bool selected = index == g_ui.menu_item;
        lv_obj_set_style_text_opa(g_ui.picker_rows[i], selected ? LV_OPA_COVER : UI_OPA_TEXT_LABEL, 0);
        lv_obj_set_style_text_color(g_ui.picker_rows[i], col(selected ? UI_COL_ACCENT_TEXT : UI_COL_FG), 0);
        lv_obj_set_style_bg_color(g_ui.picker_rows[i], col(UI_COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(g_ui.picker_rows[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_ver(g_ui.picker_rows[i], 8, 0);
    }
    lv_obj_set_style_arc_opa(g_ui.picker_band, UI_OPA_WHEEL_IDLE, LV_PART_INDICATOR);
}

static void close_picker(void)
{
    if(g_ui.audition_timer) {
        lv_timer_delete(g_ui.audition_timer);
        g_ui.audition_timer = nullptr;
        if(g_ui.audio.note_off) g_ui.audio.note_off();
    }
    if(g_ui.audio.set_envelope) g_ui.audio.set_envelope(&g_env[g_ui.program]);
    if(g_ui.audio.select_preset) g_ui.audio.select_preset(g_ui.active_preset);
    lv_obj_add_flag(g_ui.picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_ui.pad, LV_OBJ_FLAG_HIDDEN);
    show(g_ui.hold_button);
    show(g_ui.arp_button);
    show(g_ui.seq_button);
}

static void close_picker_async(void *)
{
    close_picker();
    update_pad_text();
}

static void request_close_picker(void)
{
    lv_async_call(close_picker_async, nullptr);
}

enum PickerButtonAction : uintptr_t {
    PICKER_BACK = 1,
    PICKER_AUDITION,
    PICKER_PREVIOUS,
    PICKER_NEXT,
};

static void audition_picker_item(void)
{
    if(g_ui.picker_in_group) {
        const uint16_t preset = group_item(g_ui.menu_group, g_ui.menu_item).preset;
        if(preset == kNoPreset) return;
        const synth_envelope_t envelope = preset_envelope(preset);
        if(g_ui.audio.set_envelope) g_ui.audio.set_envelope(&envelope);
        if(g_ui.audio.select_preset) g_ui.audio.select_preset(preset);
    }
    if(g_ui.audition_timer) lv_timer_delete(g_ui.audition_timer);
    if(g_ui.audio.note_on) g_ui.audio.note_on(44000, 160);
    g_ui.audition_timer = lv_timer_create([](lv_timer_t *) {
        if(g_ui.audio.note_off) g_ui.audio.note_off();
        g_ui.audition_timer = nullptr;
    }, 300, nullptr);
    lv_timer_set_repeat_count(g_ui.audition_timer, 1);
}

static void select_picker_item(void)
{
    if(g_ui.picker_in_group) {
        const MenuItem item = group_item(g_ui.menu_group, g_ui.menu_item);
        if(item.preset == kNoPreset) {
            lv_label_set_text(g_ui.picker_hint, "No samples installed");
            return;
        }
        Slot &slot = g_slots[g_ui.picker_slot];
        snprintf(slot.name, sizeof(slot.name), "%s", item.name);
        snprintf(slot.ring, sizeof(slot.ring), "%s", g_ui.menu_group == 0 ? kPrograms[item.program].ring_name : item.name);
        slot.layout = item.program;
        slot.preset = item.preset;
        save_slots();
        g_notes[g_ui.picker_slot] = default_notes(item.preset);
        save_notes();
        g_env[g_ui.picker_slot] = preset_envelope(item.preset);
        save_env();
        send_sequence(g_ui.picker_slot);
        lv_arclabel_set_text(g_ui.program_labels[g_ui.picker_slot], slot.ring);
        apply_slot(g_ui.picker_slot);
        request_close_picker();
    } else {
        g_ui.menu_group = g_ui.menu_item;
        g_ui.menu_item = 0;
        g_ui.list_first = 0;
        g_ui.picker_in_group = true;
        update_picker_rows();
    }
}

static void picker_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point{};
    if(indev) lv_indev_get_point(indev, &point);
    if(code == LV_EVENT_PRESSED && !g_ui.picker_owner) g_ui.picker_owner = indev;
    if(g_ui.picker_owner != indev) return;
    if(code == LV_EVENT_RELEASED) g_ui.picker_owner = nullptr;
    if(code == LV_EVENT_PRESSED) {
        g_ui.press_point = point;
        g_ui.last_angle = point_angle(point);
        g_ui.wheel_degrees = 0.0f;
        g_ui.wheel_travel = 0.0f;
        g_ui.wheel = point_radius(point) >= UI_WHEEL_R_IN;
        g_ui.list_touch = !g_ui.wheel && point.x >= UI_CX - UI_LIST_SEL_W / 2 &&
                          point.x < UI_CX + UI_LIST_SEL_W / 2 && point.y >= 225;
        g_ui.list_drag = false;
        g_ui.drag_first = g_ui.list_first;
        const int index = g_ui.wheel ? -1 : picker_item_at(point);
        if(index >= 0) {
            g_ui.menu_item = static_cast<uint8_t>(index);
            update_picker_rows();
        }
        ESP_LOGI("SYNTH_UI", "Picker press x=%d y=%d wheel=%d free=%u largest=%u",
                 point.x, point.y, g_ui.wheel,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }
    if(code == LV_EVENT_PRESSING && g_ui.list_touch) {
        const int dy = point.y - g_ui.press_point.y;
        if(!g_ui.list_drag && (dy > UI_LIST_DRAG_SLOP || dy < -UI_LIST_DRAG_SLOP)) g_ui.list_drag = true;
        if(!g_ui.list_drag) return;
        const int count = picker_count();
        const int max_first = count > kPickerRows ? count - kPickerRows : 0;
        int first = g_ui.drag_first - (dy >= 0 ? dy + UI_LIST_ROW_H / 2 : dy - UI_LIST_ROW_H / 2) / UI_LIST_ROW_H;
        first = first < 0 ? 0 : (first > max_first ? max_first : first);
        if(first != g_ui.list_first) {
            g_ui.list_first = (uint8_t)first;
            /* Keep the highlight inside the visible window while dragging. */
            if(g_ui.menu_item < first) g_ui.menu_item = first;
            if(g_ui.menu_item >= first + kPickerRows) g_ui.menu_item = first + kPickerRows - 1;
            update_picker_rows();
        }
        return;
    }
    if(code == LV_EVENT_PRESSING && g_ui.wheel) {
        float delta = unwrap_delta(point_angle(point) - g_ui.last_angle);
        g_ui.last_angle = point_angle(point);
        g_ui.wheel_degrees += delta;
        g_ui.wheel_travel += fabsf(delta);
        while(g_ui.wheel_degrees >= UI_WHEEL_STEP_DEG) {
            g_ui.wheel_degrees -= UI_WHEEL_STEP_DEG;
            uint8_t count = picker_count();
            g_ui.menu_item = (g_ui.menu_item + 1) % count;
            update_picker_rows();
            ESP_LOGI("SYNTH_UI", "Wheel next item=%u", g_ui.menu_item);
        }
        while(g_ui.wheel_degrees <= -UI_WHEEL_STEP_DEG) {
            g_ui.wheel_degrees += UI_WHEEL_STEP_DEG;
            uint8_t count = picker_count();
            g_ui.menu_item = (g_ui.menu_item + count - 1) % count;
            update_picker_rows();
            ESP_LOGI("SYNTH_UI", "Wheel previous item=%u", g_ui.menu_item);
        }
        ESP_LOGD("SYNTH_UI", "Wheel delta=%.1f item=%u", delta, g_ui.menu_item);
        return;
    }
    if(code != LV_EVENT_RELEASED) return;
    if(!g_ui.wheel) {
        if(g_ui.list_drag) return;
        const int index = picker_item_at(point);
        if(index < 0) return;
        g_ui.menu_item = static_cast<uint8_t>(index);
        ESP_LOGI("SYNTH_UI", "Picker item=%d group=%d", index, g_ui.picker_in_group);
        select_picker_item();
        return;
    }
    if(g_ui.wheel && g_ui.wheel_travel < UI_WHEEL_TAP_DEG) {
        float angle = point_angle(g_ui.press_point);
        if(fabsf(angle) < UI_WHEEL_BTN_DEG) {
            if(g_ui.picker_in_group) {
                g_ui.picker_in_group = false;
                g_ui.menu_item = g_ui.menu_group;
                update_picker_rows();
            } else {
                request_close_picker();
            }
        } else if(fabsf(fabsf(angle) - 180.0f) < UI_WHEEL_BTN_DEG) {
            /* Audition is deliberately explicit; scrolling is silent. */
            audition_picker_item();
        } else if(fabsf(fabsf(angle) - 90.0f) < UI_WHEEL_BTN_DEG) {
            const uint8_t count = picker_count();
            g_ui.menu_item = (g_ui.menu_item + (angle > 0 ? 1 : count - 1)) % count;
            update_picker_rows();
        } else {
            select_picker_item();
        }
    }
}

static void create_pad(void)
{
    g_ui.pad = lv_obj_create(g_ui.screen);
    style_plain(g_ui.pad);
    lv_obj_set_size(g_ui.pad, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(g_ui.pad, 0, 0);

    lv_obj_t *circle_area = lv_obj_create(g_ui.pad);
    style_plain(circle_area);
    lv_obj_set_size(circle_area, UI_PAD_R * 2, UI_PAD_R * 2);
    lv_obj_set_pos(circle_area, UI_MAP_MIN, UI_MAP_MIN);
    lv_obj_set_style_radius(circle_area, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(circle_area, UI_PAD_EDGE_W, 0);
    lv_obj_set_style_border_color(circle_area, col(UI_COL_FG), 0);
    lv_obj_set_style_border_opa(circle_area, UI_OPA_HAIRLINE, 0);
    lv_obj_set_style_clip_corner(circle_area, true, 0);

    for(int i = 0; i <= UI_PAD_R * 2 / UI_GRID_STEP; ++i) {
        lv_obj_t *line = lv_line_create(circle_area);
        static lv_point_precise_t points[26][2];
        points[i][0] = {(lv_coord_t)(i * UI_GRID_STEP), 0};
        points[i][1] = {(lv_coord_t)(i * UI_GRID_STEP), (lv_coord_t)(UI_PAD_R * 2)};
        lv_line_set_points(line, points[i], 2);
        lv_obj_set_style_line_color(line, col(UI_COL_FG), 0);
        lv_obj_set_style_line_opa(line, UI_OPA_GRID, 0);
        lv_obj_set_style_line_width(line, UI_GRID_LINE_W, 0);
    }
    for(int i = 0; i <= UI_PAD_R * 2 / UI_GRID_STEP; ++i) {
        lv_obj_t *line = lv_line_create(circle_area);
        static lv_point_precise_t points[26][2];
        points[i][0] = {0, (lv_coord_t)(i * UI_GRID_STEP)};
        points[i][1] = {(lv_coord_t)(UI_PAD_R * 2), (lv_coord_t)(i * UI_GRID_STEP)};
        lv_line_set_points(line, points[i], 2);
        lv_obj_set_style_line_color(line, col(UI_COL_FG), 0);
        lv_obj_set_style_line_opa(line, UI_OPA_GRID, 0);
        lv_obj_set_style_line_width(line, UI_GRID_LINE_W, 0);
    }

    g_ui.ring = lv_obj_create(g_ui.pad);
    style_base(g_ui.ring);
    lv_obj_clear_flag(g_ui.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_ui.ring, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(g_ui.ring, 0, 0);
    lv_obj_add_event_cb(g_ui.ring, ring_draw_event, LV_EVENT_DRAW_MAIN, nullptr);

    for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) {
        /* Curved label in the segment band, in a box just around its segment so
         * recolouring it repaints only that box. Text circles the screen centre. */
        const int text_r = UI_PROG_RING_R - UI_PROG_RING_W / 2;
        const lv_area_t box = wedge_box(i, text_r - 15, text_r + 15, 3);
        const int w = lv_area_get_width(&box), h = lv_area_get_height(&box);
        lv_obj_t *ring_label = lv_arclabel_create(g_ui.pad);
        lv_obj_remove_style_all(ring_label);
        lv_obj_clear_flag(ring_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(ring_label, box.x1, box.y1);
        lv_obj_set_size(ring_label, w, h);
        /* Offsets are stored as signed coordinates despite the uint32_t setter. */
        lv_arclabel_set_center_offset_x(ring_label, (uint32_t)(UI_CX - (box.x1 + w / 2)));
        lv_arclabel_set_center_offset_y(ring_label, (uint32_t)(UI_CY - (box.y1 + h / 2)));
        lv_obj_set_style_text_font(ring_label, UI_FONT_RING, 0);
        lv_obj_set_style_text_color(ring_label, col(UI_COL_FG), 0);
        lv_obj_set_style_text_opa(ring_label, UI_OPA_TEXT_LABEL, 0);
        lv_arclabel_set_text(ring_label, g_slots[i].ring);
        lv_arclabel_set_overflow(ring_label, LV_ARCLABEL_OVERFLOW_ELLIPSIS);
        lv_arclabel_set_radius(ring_label, text_r);
        const int centre = (i * UI_PROG_SEG_DEG + 270) % 360;
        const int span = UI_PROG_SEG_DEG - 2 * UI_PROG_SEG_GAP_DEG;
        lv_arclabel_set_angle_start(ring_label, centre - span / 2);
        lv_arclabel_set_angle_size(ring_label, span);
        /* Bottom half runs counter-clockwise so text stays upright. */
        lv_arclabel_set_dir(ring_label, centre > 0 && centre < 180 ? LV_ARCLABEL_DIR_COUNTER_CLOCKWISE
                                                                   : LV_ARCLABEL_DIR_CLOCKWISE);
        lv_arclabel_set_text_horizontal_align(ring_label, LV_ARCLABEL_TEXT_ALIGN_CENTER);
        lv_arclabel_set_text_vertical_align(ring_label, LV_ARCLABEL_TEXT_ALIGN_CENTER);
        g_ui.program_labels[i] = ring_label;
    }

    g_ui.title = label(g_ui.pad, "", UI_FONT_TITLE, UI_OPA_TEXT_TITLE);
    lv_obj_align(g_ui.title, LV_ALIGN_TOP_MID, 0, UI_PAD_TITLE_Y - 18);
    g_ui.subtitle = label(g_ui.pad, "", UI_FONT_SUB, UI_OPA_TEXT_SUB);
    lv_obj_align(g_ui.subtitle, LV_ALIGN_TOP_MID, 0, UI_PAD_SUB_Y - 16);
    g_ui.readout = label(g_ui.pad, "", UI_FONT_READOUT, UI_OPA_TEXT_SUB);
    lv_obj_align(g_ui.readout, LV_ALIGN_TOP_MID, 0, UI_PAD_READOUT_Y - 10);

    g_ui.hold_button = lv_obj_create(g_ui.screen);
    style_button(g_ui.hold_button);
    lv_obj_set_size(g_ui.hold_button, UI_HOLD_W, UI_HOLD_H);
    lv_obj_align(g_ui.hold_button, LV_ALIGN_TOP_MID, -UI_PILL_SPACING, UI_HOLD_CY - UI_HOLD_H / 2);
    lv_obj_set_style_radius(g_ui.hold_button, UI_HOLD_RADIUS, 0);
    lv_obj_set_style_bg_color(g_ui.hold_button, col(UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.hold_button, 150, 0);
    lv_obj_set_style_border_width(g_ui.hold_button, 0, 0);
    g_ui.hold_label = label(g_ui.hold_button, "Hold", UI_FONT_SMALL, LV_OPA_COVER);
    lv_obj_center(g_ui.hold_label);
    lv_obj_add_event_cb(g_ui.hold_button, hold_event, LV_EVENT_CLICKED, nullptr);

    g_ui.arp_button = lv_obj_create(g_ui.screen);
    style_button(g_ui.arp_button);
    lv_obj_set_size(g_ui.arp_button, UI_HOLD_W, UI_HOLD_H);
    lv_obj_align(g_ui.arp_button, LV_ALIGN_TOP_MID, 0, UI_HOLD_CY - UI_HOLD_H / 2);
    lv_obj_set_style_radius(g_ui.arp_button, UI_HOLD_RADIUS, 0);
    lv_obj_set_style_bg_color(g_ui.arp_button, col(UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.arp_button, 150, 0);
    g_ui.arp_label = label(g_ui.arp_button, "Arp off", UI_FONT_SMALL, LV_OPA_COVER);
    lv_obj_center(g_ui.arp_label);
    lv_obj_add_event_cb(g_ui.arp_button, arp_event, LV_EVENT_CLICKED, nullptr);

    g_ui.seq_button = lv_obj_create(g_ui.screen);
    style_button(g_ui.seq_button);
    lv_obj_set_size(g_ui.seq_button, UI_HOLD_W, UI_HOLD_H);
    lv_obj_align(g_ui.seq_button, LV_ALIGN_TOP_MID, UI_PILL_SPACING, UI_HOLD_CY - UI_HOLD_H / 2);
    lv_obj_set_style_radius(g_ui.seq_button, UI_HOLD_RADIUS, 0);
    g_ui.seq_label = label(g_ui.seq_button, "Seq", UI_FONT_SMALL, LV_OPA_COVER);
    lv_obj_center(g_ui.seq_label);
    lv_obj_add_event_cb(g_ui.seq_button, seq_event, LV_EVENT_ALL, nullptr);
    update_seq_button();

    lv_obj_t *touch = lv_obj_create(g_ui.pad);
    style_plain(touch);
    lv_obj_set_size(touch, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_add_event_cb(touch, pad_event, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_PRESS_LOCK);

    for(uint8_t i = 0; i < UI_TRAIL_POOL; ++i) {
        g_ui.trail[i] = circle(g_ui.pad, UI_TRAIL_R_START * 2, col(UI_COL_ACCENT), 0);
        hide(g_ui.trail[i]);
    }
    g_ui.cross_x = lv_obj_create(g_ui.pad); style_plain(g_ui.cross_x); lv_obj_set_size(g_ui.cross_x, 600, UI_CROSSHAIR_W); lv_obj_set_style_bg_color(g_ui.cross_x, col(UI_COL_ACCENT), 0); lv_obj_set_style_bg_opa(g_ui.cross_x, UI_OPA_CROSSHAIR, 0); hide(g_ui.cross_x);
    g_ui.cross_y = lv_obj_create(g_ui.pad); style_plain(g_ui.cross_y); lv_obj_set_size(g_ui.cross_y, UI_CROSSHAIR_W, 600); lv_obj_set_style_bg_color(g_ui.cross_y, col(UI_COL_ACCENT), 0); lv_obj_set_style_bg_opa(g_ui.cross_y, UI_OPA_CROSSHAIR, 0); hide(g_ui.cross_y);
    g_ui.touch_halo = circle(g_ui.pad, UI_TOUCH_HALO_R * 2, col(UI_COL_ACCENT_LIGHT), UI_OPA_TOUCH_HALO); hide(g_ui.touch_halo);
    g_ui.touch_core = circle(g_ui.pad, UI_TOUCH_CORE_R * 2, col(UI_COL_ACCENT_LIGHT), LV_OPA_COVER); hide(g_ui.touch_core);
    g_ui.touch_dot = circle(g_ui.pad, UI_TOUCH_DOT_R * 2, col(UI_COL_FG), LV_OPA_COVER); hide(g_ui.touch_dot);
    lv_obj_clear_flag(g_ui.cross_x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_ui.cross_y, LV_OBJ_FLAG_CLICKABLE);
    for(auto &contact : g_ui.contacts) {
        contact.marker = circle(g_ui.pad, 24, col(UI_COL_ACCENT_LIGHT), LV_OPA_COVER);
        hide(contact.marker);
    }
    update_pad_text();
}

static lv_obj_t *picker_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                               PickerButtonAction action)
{
    (void)action;
    lv_obj_t *button = lv_obj_create(parent);
    style_plain(button);
    /* Wheel touch layer owns the complete gesture, even over these labels. */
    lv_obj_clear_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, w, h);
    lv_obj_set_pos(button, x, y);
    lv_obj_t *button_label = label(button, text, UI_FONT_SUB, UI_OPA_TEXT_LABEL);
    lv_obj_center(button_label);
    return button;
}

static void create_picker(void)
{
    g_ui.picker = lv_obj_create(g_ui.screen);
    style_plain(g_ui.picker);
    lv_obj_set_size(g_ui.picker, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(g_ui.picker, col(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(g_ui.picker, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(g_ui.picker, picker_event, LV_EVENT_ALL, nullptr);

    g_ui.picker_band = lv_arc_create(g_ui.picker);
    lv_obj_remove_style_all(g_ui.picker_band);
    lv_obj_set_size(g_ui.picker_band, UI_WHEEL_R_OUT * 2, UI_WHEEL_R_OUT * 2);
    lv_obj_align(g_ui.picker_band, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(g_ui.picker_band, 0, 360);
    lv_arc_set_angles(g_ui.picker_band, 0, 360);
    lv_obj_set_style_arc_width(g_ui.picker_band, UI_WHEEL_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_ui.picker_band, col(UI_COL_FG), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g_ui.picker_band, UI_OPA_WHEEL_IDLE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g_ui.picker_band, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(g_ui.picker_band, LV_OBJ_FLAG_CLICKABLE);

    g_ui.picker_crumb = label(g_ui.picker, "Instruments", UI_FONT_SMALL, UI_OPA_TEXT_SUB);
    lv_obj_align(g_ui.picker_crumb, LV_ALIGN_TOP_MID, 0, UI_CRUMB_Y - 12);
    for(uint8_t i = 0; i < 6; ++i) {
        g_ui.picker_rows[i] = label(g_ui.picker, "", UI_FONT_LIST, UI_OPA_TEXT_LABEL);
        lv_obj_set_width(g_ui.picker_rows[i], UI_LIST_SEL_W);
        lv_obj_set_style_text_align(g_ui.picker_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_ui.picker_rows[i], UI_CX - UI_LIST_SEL_W / 2, 225 + i * UI_LIST_ROW_H);
    }
    g_ui.picker_hint = label(g_ui.picker, "Tap center to select", UI_FONT_HINT, UI_OPA_TEXT_HINT);
    lv_obj_align(g_ui.picker_hint, LV_ALIGN_TOP_MID, 0, UI_HINT_Y - 10);
    lv_obj_t *picker_touch = lv_obj_create(g_ui.picker);
    style_plain(picker_touch);
    lv_obj_set_size(picker_touch, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(picker_touch, 0, 0);
    lv_obj_add_flag(picker_touch, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_radius(picker_touch, 0, 0);
    lv_obj_add_event_cb(picker_touch, picker_event, LV_EVENT_ALL, nullptr);
    picker_button(g_ui.picker, "Menu", UI_CX - 75, 46, 150, 70, PICKER_BACK);
    picker_button(g_ui.picker, "Audition", UI_CX - 75, UI_SCREEN_H - 116, 150, 70, PICKER_AUDITION);
    picker_button(g_ui.picker, LV_SYMBOL_PREV, 36, UI_CY - 50, 110, 100, PICKER_PREVIOUS);
    picker_button(g_ui.picker, LV_SYMBOL_NEXT, UI_SCREEN_W - 146, UI_CY - 50, 110, 100, PICKER_NEXT);
    update_picker_rows();
    hide(g_ui.picker);
}

} // namespace

static void render_timing_event(lv_event_t *event)
{
    static int64_t started = 0;
    if(lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        started = esp_timer_get_time();
        return;
    }
    const int64_t ms = (esp_timer_get_time() - started) / 1000;
    if(ms > UI_SLOW_FRAME_MS) ESP_LOGW("SYNTH_UI", "Slow frame: %lld ms", (long long)ms);
}

void synth_ui_init(lv_obj_t *screen, const synth_ui_audio_callbacks_t *audio)
{
    g_ui = UiState{};
    g_ui.screen = screen;
    style_plain(screen);
    if(audio) g_ui.audio = *audio;
    lv_obj_set_style_bg_color(screen, col(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(screen, col(UI_COL_BG), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    load_slots();
    load_notes();
    load_env();
    load_patterns();
    create_pad();
    create_picker();
    create_notes_editor();
    send_fx();
    apply_slot(0);
    lv_display_t *display = lv_obj_get_display(screen);
    lv_display_add_event_cb(display, render_timing_event, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(display, render_timing_event, LV_EVENT_RENDER_READY, nullptr);
}
