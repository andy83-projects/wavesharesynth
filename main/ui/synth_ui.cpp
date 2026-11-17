#include "synth_ui.h"
#include "ui_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

namespace {

struct Program {
    const char *name;
    const char *ring_name;
    const char *x_label;
    const char *y_label;
    bool lanes;
};

static const Program kPrograms[UI_PROG_COUNT] = {
    {"Filter sweep", "FILT", "Cutoff", "Resonance", false},
    {"Band sweep", "BAND", "Center", "Width", false},
    {"Acid", "Acid", "Cutoff", "Resonance", false},
    {"Touch scale", "SCALE", "Note", "Filter", true},
    {"FM bell", "FM", "Ratio", "Mod index", false},
    {"Arp", "Arp", "Rate", "Span", false},
    {"Vibrato", "VIB", "Rate", "Depth", false},
    {"Tremolo gate", "TREM", "Rate", "Depth", false},
    {"PWM lead", "PWM", "Note", "Pulse width", true},
    {"Bitcrush", "CRUSH", "Bits", "Cutoff", false},
    {"Pluck", "Pluck", "Note", "Damping", true},
    {"Organ", "Organ", "Note", "Reverb", true},
};

struct MenuItem {
    const char *name;
    uint8_t program;
};

struct MenuGroup {
    const char *name;
    MenuItem items[6];
    uint8_t count;
};

static const MenuGroup kMenu[] = {
    {"Synth", {{"Saw lead", 0}, {"Pulse lead", 8}, {"Filter synth", 0}, {"Acid bass", 2}, {"Sub bass", 2}}, 5},
    {"Keys", {{"FM e-piano", 4}, {"FM bell", 4}, {"Organ B3", 11}, {"Organ + Leslie", 11}}, 4},
    {"Plucked", {{"Nylon guitar", 10}, {"Harp", 10}, {"Pizzicato", 10}}, 3},
    {"Drums", {{"Noise hat", 9}, {"Noise snare", 9}, {"Sine kick", 9}}, 3},
    {"Wavetables", {{"Glass", 4}, {"Formant", 0}, {"Digital", 9}}, 3},
    {"Samples", {{"Placeholders", 0}}, 1},
};

struct UiState {
    struct Contact {
        lv_indev_t *device = nullptr;
        bool down = false;
        bool ring = false;
        uint32_t stationary_since = 0;
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
    lv_obj_t *x_label = nullptr;
    lv_obj_t *y_label = nullptr;
    lv_obj_t *readout = nullptr;
    lv_obj_t *hold_button = nullptr;
    lv_obj_t *hold_label = nullptr;
    lv_obj_t *arp_button = nullptr;
    lv_obj_t *arp_label = nullptr;
    lv_obj_t *touch_halo = nullptr;
    lv_obj_t *touch_core = nullptr;
    lv_obj_t *touch_dot = nullptr;
    lv_obj_t *cross_x = nullptr;
    lv_obj_t *cross_y = nullptr;
    lv_obj_t *trail[UI_TRAIL_POOL]{};
    lv_obj_t *program_arcs[UI_PROG_COUNT]{};
    lv_obj_t *program_labels[UI_PROG_COUNT]{};
    lv_obj_t *picker_crumb = nullptr;
    lv_obj_t *picker_hint = nullptr;
    lv_obj_t *picker_rows[6]{};
    lv_obj_t *picker_ticks[UI_TICK_COUNT]{};
    lv_obj_t *picker_band = nullptr;
    lv_obj_t *picker_scroll = nullptr;
    uint8_t program = 0;
    uint16_t active_preset = 0;
    bool acid_sweep = false;
    uint16_t acid_cutoff = 350;
    uint8_t acid_resonance = 140;
    uint8_t menu_group = 0;
    uint8_t menu_item = 0;
    const char *instrument_name = nullptr;
    bool picker_in_group = false;
    bool hold = false;
    bool touching = false;
    lv_timer_t *audition_timer = nullptr;
    lv_indev_t *picker_owner = nullptr;
    bool arp = false;
    bool arp_enabled = false;
    uint32_t arp_started = 0;
    uint32_t arp_step = UINT32_MAX;
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

static void style_plain(lv_obj_t *obj)
{
    /* This UI supplies its own styles. Remove theme transitions and states,
     * and prevent LVGL's built-in scrolling from stealing wheel gestures. */
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);

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

static void update_pad_text(void)
{
    const Program &p = kPrograms[g_ui.program];
    lv_label_set_text(g_ui.title, g_ui.instrument_name ? g_ui.instrument_name : p.name);
    lv_label_set_text_fmt(g_ui.subtitle, "P%02u · CPU --%%", (unsigned)(g_ui.program + 1));
    lv_label_set_text_fmt(g_ui.x_label, "%s →", p.x_label);
    lv_label_set_text_fmt(g_ui.y_label, "%s ↑", p.y_label);
    lv_label_set_text(g_ui.readout, "Touch to play");
    for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) {
        const bool selected = i == g_ui.program;
        lv_obj_set_style_arc_color(g_ui.program_arcs[i], col(selected ? UI_COL_ACCENT : UI_COL_FG), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(g_ui.program_arcs[i], selected ? LV_OPA_COVER : UI_OPA_SEG_IDLE, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(g_ui.program_labels[i], col(selected ? UI_COL_ACCENT : UI_COL_FG), 0);
    }
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

static synth_ui_note_t pad_note(int x, int y, uint32_t trigger)
{
    x = x < UI_MAP_MIN ? UI_MAP_MIN : (x > UI_MAP_MAX ? UI_MAP_MAX : x);
    y = y < UI_MAP_MIN ? UI_MAP_MIN : (y > UI_MAP_MAX ? UI_MAP_MAX : y);
    uint32_t freq = 13000u + ((uint32_t)(x - UI_MAP_MIN) * 87000u) /
                    (UI_MAP_MAX - UI_MAP_MIN);
    if(g_ui.active_preset == 3) {
        const int midi = 24 + ((x - UI_MAP_MIN) * 24 + (UI_MAP_MAX - UI_MAP_MIN) / 2) / (UI_MAP_MAX - UI_MAP_MIN);
        freq = (uint32_t)(44000.0f * powf(2.0f, (midi - 69) / 12.0f));
    }
    uint16_t volume = (uint16_t)(((uint32_t)(UI_MAP_MAX - y) * 255u) /
                                 (UI_MAP_MAX - UI_MAP_MIN));
    if(g_ui.active_preset == 3) volume = 200;
    return {freq, volume, true, trigger, false, false, true};
}

static void update_acid_gesture(void)
{
    unsigned count = 0;
    int x = 0, y = 0;
    for(const auto &contact : g_ui.contacts) if(contact.down && !contact.ring) {
        ++count; x += contact.point.x; y += contact.point.y;
    }
    const bool sweep = g_ui.active_preset == 3 && count >= 3;
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
        g_ui.arp_started = lv_tick_get();
        g_ui.arp_step = UINT32_MAX;
        ESP_LOGI("SYNTH_UI", "Arp started explicitly with %u held notes", count);
    }
    if(count == 0) {
        g_ui.arp = false;
        lv_label_set_text(g_ui.readout, "Touch to play");
    } else if(g_ui.arp) {
        synth_ui_note_t held[SYNTH_UI_MAX_CONTACTS]{};
        unsigned n = 0;
        for(const auto &note : notes) if(note.pressed) held[n++] = note;
        for(unsigned i = 1; i < n; ++i) {
            for(unsigned j = i; j > 0 && held[j].frequency_centi_hz < held[j - 1].frequency_centi_hz; --j) {
                const auto tmp = held[j]; held[j] = held[j - 1]; held[j - 1] = tmp;
            }
        }
        const uint32_t elapsed = lv_tick_elaps(g_ui.arp_started);
        const uint32_t step = elapsed / 125; /* 120 BPM, sixteenth notes */
        if(step != g_ui.arp_step) {
            g_ui.arp_step = step;
            ++g_ui.trigger;
        }
        memset(notes, 0, sizeof(notes));
        notes[0] = held[step % n];
        if(n == 1) {
            static const float ratios[] = {1.0f, 1.1892071f, 1.4983071f, 2.0f};
            notes[0].frequency_centi_hz = (uint32_t)(held[0].frequency_centi_hz * ratios[step % 4]);
        }
        notes[0].trigger = g_ui.trigger;
        notes[0].sustain = false;
        if(g_ui.active_preset == 3) {
            notes[0].accent = step % 4 == 0;
            notes[0].slide = step % 4 == 2 || step % 4 == 3;
            const unsigned next = (step + 1) % 4;
            notes[0].pressed = next == 2 || next == 3 || elapsed % 125 < 95;
        } else {
            notes[0].pressed = elapsed % 125 < 95;
        }
        lv_label_set_text_fmt(g_ui.readout, "ARP · %u held · 120 BPM", count);
    } else {
        lv_label_set_text_fmt(g_ui.readout, "%u note%s", count, count == 1 ? "" : "s");
    }
    if(g_ui.acid_sweep) lv_label_set_text_fmt(g_ui.readout, "ACID%s · %u Hz · res %u%%",
        g_ui.arp ? " ARP" : "", g_ui.acid_cutoff, g_ui.acid_resonance * 100 / 255);
    if(g_ui.audio.set_notes) g_ui.audio.set_notes(notes);
}

static void arp_timer(lv_timer_t *)
{
    if(g_ui.arp_enabled && g_ui.arp) publish_contacts();
}

static void arp_event(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    g_ui.arp_enabled = !g_ui.arp_enabled;
    g_ui.arp = false;
    lv_label_set_text(g_ui.arp_label, g_ui.arp_enabled ? "Arp on" : "Arp off");
    lv_obj_set_style_bg_color(g_ui.arp_button, col(g_ui.arp_enabled ? UI_COL_ACCENT : UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.arp_button, g_ui.arp_enabled ? LV_OPA_COVER : UI_OPA_PILL_IDLE, 0);
    lv_obj_set_style_text_color(g_ui.arp_label, col(g_ui.arp_enabled ? UI_COL_ACCENT_TEXT : UI_COL_FG), 0);
    ESP_LOGI("SYNTH_UI", "Arp enabled=%d", g_ui.arp_enabled);
    publish_contacts();
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

static void select_ring_program(const lv_point_t &point)
{
    float angle = atan2f(point.x - UI_CX, UI_CY - point.y) * 180.0f / (float)M_PI;
    if(angle < 0) angle += 360.0f;
    const uint8_t program = (int)((angle + UI_PROG_SEG_DEG / 2) / UI_PROG_SEG_DEG) % UI_PROG_COUNT;
    if(g_ui.program == program && g_ui.instrument_name == kPrograms[program].name) return;
    g_ui.program = program;
    g_ui.instrument_name = kPrograms[program].name;
    static const uint16_t presets[UI_PROG_COUNT] = {0, 2, 3, 6, 7, 1, 36, 37, 1, 26, 12, 8};
    g_ui.active_preset = presets[program];
    if(g_ui.audio.select_preset) g_ui.audio.select_preset(g_ui.active_preset);
    ESP_LOGI("SYNTH_UI", "Ring program=%u", program + 1);
    update_pad_text();
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
        contact->note.trigger = ++g_ui.trigger;
        if(!contact->ring) contact->note = pad_note(point.x, point.y, contact->note.trigger);
    }
    if(code == LV_EVENT_PRESS_LOST) {
        contact->down = false;
        contact->note.pressed = false;
        hide(contact->marker);
        publish_contacts();
        return;
    }
    if(contact->ring) {
        if(code != LV_EVENT_RELEASED) select_ring_program(point);
        else contact->down = false;
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

static void open_picker_event(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    stop_contacts();
    g_ui.picker_owner = nullptr;
    lv_obj_clear_flag(g_ui.picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui.pad, LV_OBJ_FLAG_HIDDEN);
    if(g_ui.audio.note_off) g_ui.audio.note_off();
}

static void hold_event(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
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
        publish_contacts();
    }
    lv_obj_set_style_bg_color(g_ui.hold_button, col(g_ui.hold ? UI_COL_ACCENT : UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.hold_button, g_ui.hold ? LV_OPA_COVER : UI_OPA_PILL_IDLE, 0);
    lv_obj_set_style_text_color(g_ui.hold_label, col(g_ui.hold ? UI_COL_ACCENT_TEXT : UI_COL_FG), 0);
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

static void update_picker_rows(void)
{
    if(g_ui.picker_in_group) {
        lv_label_set_text_fmt(g_ui.picker_crumb, "Instruments › %s", kMenu[g_ui.menu_group].name);
        for(uint8_t i = 0; i < 6; ++i) {
            if(i < kMenu[g_ui.menu_group].count) lv_label_set_text(g_ui.picker_rows[i], kMenu[g_ui.menu_group].items[i].name);
            else lv_label_set_text(g_ui.picker_rows[i], "");
        }
    } else {
        lv_label_set_text(g_ui.picker_crumb, "Instruments");
        for(uint8_t i = 0; i < 6; ++i) {
            if(i < sizeof(kMenu) / sizeof(kMenu[0])) lv_label_set_text(g_ui.picker_rows[i], kMenu[i].name);
            else lv_label_set_text(g_ui.picker_rows[i], "");
        }
    }
    for(uint8_t i = 0; i < 6; ++i) {
        lv_obj_set_y(g_ui.picker_rows[i], 225 + (int)i * UI_LIST_ROW_H);
        const bool selected = i == g_ui.menu_item;
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
    if(g_ui.audio.select_preset) g_ui.audio.select_preset(g_ui.active_preset);
    lv_obj_add_flag(g_ui.picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_ui.pad, LV_OBJ_FLAG_HIDDEN);
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
    if(g_ui.picker_in_group && g_ui.menu_group == 5) return;
    if(g_ui.picker_in_group && g_ui.audio.select_preset) {
        g_ui.audio.select_preset(g_ui.menu_group * 6 + g_ui.menu_item);
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
        const MenuItem &item = kMenu[g_ui.menu_group].items[g_ui.menu_item];
        if(g_ui.menu_group == 5) {
            lv_label_set_text(g_ui.picker_hint, "No samples installed");
            return;
        }
        g_ui.program = item.program;
        g_ui.instrument_name = item.name;
        g_ui.active_preset = g_ui.menu_group * 6 + g_ui.menu_item;
        if(g_ui.audio.select_preset) g_ui.audio.select_preset(g_ui.active_preset);
        request_close_picker();
    } else {
        g_ui.menu_group = g_ui.menu_item;
        g_ui.menu_item = 0;
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
        if(!g_ui.wheel && point.x >= UI_CX - UI_LIST_SEL_W / 2 && point.x < UI_CX + UI_LIST_SEL_W / 2 && point.y >= 225) {
            const int row = (point.y - 225) / UI_LIST_ROW_H;
            const uint8_t count = g_ui.picker_in_group ? kMenu[g_ui.menu_group].count : sizeof(kMenu) / sizeof(kMenu[0]);
            if(row < count) {
                g_ui.menu_item = static_cast<uint8_t>(row);
                update_picker_rows();
            }
        }
        ESP_LOGI("SYNTH_UI", "Picker press x=%d y=%d wheel=%d free=%u largest=%u",
                 point.x, point.y, g_ui.wheel,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }
    if(code == LV_EVENT_PRESSING && g_ui.wheel) {
        float delta = unwrap_delta(point_angle(point) - g_ui.last_angle);
        g_ui.last_angle = point_angle(point);
        g_ui.wheel_degrees += delta;
        g_ui.wheel_travel += fabsf(delta);
        while(g_ui.wheel_degrees >= UI_WHEEL_STEP_DEG) {
            g_ui.wheel_degrees -= UI_WHEEL_STEP_DEG;
            uint8_t count = g_ui.picker_in_group ? kMenu[g_ui.menu_group].count : sizeof(kMenu) / sizeof(kMenu[0]);
            g_ui.menu_item = (g_ui.menu_item + 1) % count;
            update_picker_rows();
            ESP_LOGI("SYNTH_UI", "Wheel next item=%u", g_ui.menu_item);
        }
        while(g_ui.wheel_degrees <= -UI_WHEEL_STEP_DEG) {
            g_ui.wheel_degrees += UI_WHEEL_STEP_DEG;
            uint8_t count = g_ui.picker_in_group ? kMenu[g_ui.menu_group].count : sizeof(kMenu) / sizeof(kMenu[0]);
            g_ui.menu_item = (g_ui.menu_item + count - 1) % count;
            update_picker_rows();
            ESP_LOGI("SYNTH_UI", "Wheel previous item=%u", g_ui.menu_item);
        }
        ESP_LOGD("SYNTH_UI", "Wheel delta=%.1f item=%u", delta, g_ui.menu_item);
        return;
    }
    if(code != LV_EVENT_RELEASED) return;
    if(!g_ui.wheel) {
        const uint8_t count = g_ui.picker_in_group ? kMenu[g_ui.menu_group].count : sizeof(kMenu) / sizeof(kMenu[0]);
        if(point.x < UI_CX - UI_LIST_SEL_W / 2 || point.x >= UI_CX + UI_LIST_SEL_W / 2 ||
           point.y < 225 || point.y >= 225 + count * UI_LIST_ROW_H) return;
        const int row = (point.y - 225) / UI_LIST_ROW_H;
        g_ui.menu_item = static_cast<uint8_t>(row);
        ESP_LOGI("SYNTH_UI", "Picker row=%d group=%d", row, g_ui.picker_in_group);
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
            const uint8_t count = g_ui.picker_in_group ? kMenu[g_ui.menu_group].count : sizeof(kMenu) / sizeof(kMenu[0]);
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

    for(uint8_t i = 0; i < UI_PROG_COUNT; ++i) {
        lv_obj_t *arc = lv_arc_create(g_ui.pad);
        g_ui.program_arcs[i] = arc;
        lv_obj_remove_style_all(arc);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(arc, UI_PROG_RING_R * 2, UI_PROG_RING_R * 2);
        lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
        /* LVGL zero is right; centre each arc on its label (zero at top). */
        const int start = (i * UI_PROG_SEG_DEG + 270 - UI_PROG_SEG_DEG / 2 + UI_PROG_SEG_GAP_DEG) % 360;
        const int end = (i * UI_PROG_SEG_DEG + 270 + UI_PROG_SEG_DEG / 2 - UI_PROG_SEG_GAP_DEG) % 360;
        lv_arc_set_bg_angles(arc, start, end);
        lv_arc_set_angles(arc, start, end);
        lv_obj_set_style_arc_width(arc, UI_PROG_RING_W, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, col(i == 0 ? UI_COL_ACCENT : UI_COL_FG), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, i == 0 ? LV_OPA_COVER : UI_OPA_SEG_IDLE, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

        lv_obj_t *ring_label = label(g_ui.pad, kPrograms[i].ring_name, UI_FONT_RING, UI_OPA_TEXT_LABEL);
        g_ui.program_labels[i] = ring_label;
        lv_obj_set_width(ring_label, 58);
        lv_obj_set_style_text_align(ring_label, LV_TEXT_ALIGN_CENTER, 0);
        float angle = ((float)i * UI_PROG_SEG_DEG - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_set_pos(ring_label,
                       (int)(UI_CX + cosf(angle) * 320.0f - 29),
                       (int)(UI_CY + sinf(angle) * 320.0f - 8));

    }

    lv_obj_t *instrument = lv_obj_create(g_ui.pad);
    style_plain(instrument);
    lv_obj_set_size(instrument, 150, 30);
    lv_obj_set_pos(instrument, UI_CX - 75, 3);
    lv_obj_add_event_cb(instrument, open_picker_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_radius(instrument, 15, 0);
    lv_obj_set_style_border_width(instrument, 1, 0);
    lv_obj_set_style_border_color(instrument, col(UI_COL_ACCENT), 0);
    lv_obj_set_style_border_opa(instrument, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(instrument, col(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(instrument, LV_OPA_COVER, 0);
    lv_obj_t *instrument_label = label(instrument, "Instrument", UI_FONT_HINT, UI_OPA_TEXT_TITLE);
    lv_obj_set_style_text_color(instrument_label, col(UI_COL_ACCENT_LIGHT), 0);
    lv_obj_center(instrument_label);

    g_ui.title = label(g_ui.pad, "", UI_FONT_TITLE, UI_OPA_TEXT_TITLE);
    lv_obj_align(g_ui.title, LV_ALIGN_TOP_MID, 0, UI_PAD_TITLE_Y - 18);
    g_ui.subtitle = label(g_ui.pad, "", UI_FONT_SUB, UI_OPA_TEXT_SUB);
    lv_obj_align(g_ui.subtitle, LV_ALIGN_TOP_MID, 0, UI_PAD_SUB_Y - 16);
    g_ui.x_label = label(g_ui.pad, "", UI_FONT_HINT, UI_OPA_TEXT_HINT);
    lv_obj_align(g_ui.x_label, LV_ALIGN_TOP_MID, 0, UI_PAD_XLABEL_Y - 10);
    g_ui.y_label = label(g_ui.pad, "", UI_FONT_HINT, UI_OPA_TEXT_HINT);
    lv_obj_set_pos(g_ui.y_label, UI_PAD_YLABEL_X - 45, UI_CY - 12);
    g_ui.readout = label(g_ui.pad, "", UI_FONT_READOUT, UI_OPA_TEXT_SUB);
    lv_obj_align(g_ui.readout, LV_ALIGN_TOP_MID, 0, UI_PAD_READOUT_Y - 10);

    g_ui.hold_button = lv_obj_create(g_ui.pad);
    style_plain(g_ui.hold_button);
    lv_obj_set_size(g_ui.hold_button, UI_HOLD_W, UI_HOLD_H);
    lv_obj_align(g_ui.hold_button, LV_ALIGN_TOP_MID, -60, UI_HOLD_CY - UI_HOLD_H / 2);
    lv_obj_set_style_radius(g_ui.hold_button, UI_HOLD_RADIUS, 0);
    lv_obj_set_style_bg_color(g_ui.hold_button, col(UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.hold_button, UI_OPA_PILL_IDLE, 0);
    lv_obj_set_style_border_width(g_ui.hold_button, 0, 0);
    g_ui.hold_label = label(g_ui.hold_button, "Hold", UI_FONT_SMALL, UI_OPA_TEXT_LABEL);
    lv_obj_center(g_ui.hold_label);
    lv_obj_add_event_cb(g_ui.hold_button, hold_event, LV_EVENT_CLICKED, nullptr);

    g_ui.arp_button = lv_obj_create(g_ui.pad);
    style_plain(g_ui.arp_button);
    lv_obj_set_size(g_ui.arp_button, UI_HOLD_W, UI_HOLD_H);
    lv_obj_align(g_ui.arp_button, LV_ALIGN_TOP_MID, 60, UI_HOLD_CY - UI_HOLD_H / 2);
    lv_obj_set_style_radius(g_ui.arp_button, UI_HOLD_RADIUS, 0);
    lv_obj_set_style_bg_color(g_ui.arp_button, col(UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.arp_button, UI_OPA_PILL_IDLE, 0);
    g_ui.arp_label = label(g_ui.arp_button, "Arp off", UI_FONT_SMALL, UI_OPA_TEXT_LABEL);
    lv_obj_center(g_ui.arp_label);
    lv_obj_add_event_cb(g_ui.arp_button, arp_event, LV_EVENT_CLICKED, nullptr);

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
    lv_obj_move_foreground(g_ui.hold_button);
    lv_obj_move_foreground(g_ui.arp_button);
    lv_obj_move_foreground(instrument);
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
    create_pad();
    create_picker();
    lv_timer_create(arp_timer, 10, nullptr);
}
