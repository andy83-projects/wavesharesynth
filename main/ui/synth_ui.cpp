#include "synth_ui.h"
#include "ui_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

struct Program {
    const char *name;
    const char *x_label;
    const char *y_label;
    bool lanes;
};

static const Program kPrograms[UI_PROG_COUNT] = {
    {"Filter sweep", "Cutoff", "Resonance", false},
    {"Band sweep", "Center", "Width", false},
    {"Acid", "Cutoff", "Resonance", false},
    {"Touch scale", "Note", "Filter", true},
    {"FM bell", "Ratio", "Mod index", false},
    {"Arp", "Rate", "Span", false},
    {"Vibrato", "Rate", "Depth", false},
    {"Tremolo gate", "Rate", "Depth", false},
    {"PWM lead", "Note", "Pulse width", true},
    {"Bitcrush", "Bits", "Cutoff", false},
    {"Pluck", "Note", "Damping", true},
    {"Organ", "Note", "Reverb", true},
};

struct MenuGroup {
    const char *name;
    const char *items[6];
    uint8_t count;
};

static const MenuGroup kMenu[] = {
    {"Synth", {"Saw lead", "Pulse lead", "Filter synth", "Acid bass", "Sub bass"}, 5},
    {"Keys", {"FM e-piano", "FM bell", "Organ B3", "Organ + Leslie"}, 4},
    {"Plucked", {"Nylon guitar", "Harp", "Pizzicato"}, 3},
    {"Drums", {"Noise hat", "Noise snare", "Sine kick"}, 3},
    {"Wavetables", {"Glass", "Formant", "Digital"}, 3},
    {"Samples", {"Placeholders"}, 1},
};

struct UiState {
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
    lv_obj_t *touch_halo = nullptr;
    lv_obj_t *touch_core = nullptr;
    lv_obj_t *touch_dot = nullptr;
    lv_obj_t *cross_x = nullptr;
    lv_obj_t *cross_y = nullptr;
    lv_obj_t *trail[UI_TRAIL_POOL]{};
    lv_obj_t *picker_crumb = nullptr;
    lv_obj_t *picker_hint = nullptr;
    lv_obj_t *picker_rows[6]{};
    lv_obj_t *picker_ticks[UI_TICK_COUNT]{};
    lv_obj_t *picker_band = nullptr;
    lv_obj_t *picker_scroll = nullptr;
    uint8_t program = 0;
    uint8_t menu_group = 0;
    uint8_t menu_item = 0;
    bool picker_in_group = false;
    bool hold = false;
    bool touching = false;
    bool wheel = false;
    bool long_pressed = false;
    lv_point_t press_point{};
    float last_angle = 0.0f;
    float wheel_degrees = 0.0f;
    uint8_t trail_index = 0;
};

static UiState g_ui;

static lv_color_t col(uint32_t hex) { return lv_color_hex(hex); }

static void style_plain(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
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
    lv_label_set_text(g_ui.title, p.name);
    lv_label_set_text_fmt(g_ui.subtitle, "P%02u · CPU --%%", (unsigned)(g_ui.program + 1));
    lv_label_set_text_fmt(g_ui.x_label, "%s →", p.x_label);
    lv_label_set_text_fmt(g_ui.y_label, "%s ↑", p.y_label);
    lv_label_set_text(g_ui.readout, "Touch to play");
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

static void set_pad_audio(int x, int y)
{
    x = x < UI_MAP_MIN ? UI_MAP_MIN : (x > UI_MAP_MAX ? UI_MAP_MAX : x);
    y = y < UI_MAP_MIN ? UI_MAP_MIN : (y > UI_MAP_MAX ? UI_MAP_MAX : y);
    uint32_t freq = 13000u + ((uint32_t)(x - UI_MAP_MIN) * 87000u) /
                    (UI_MAP_MAX - UI_MAP_MIN);
    uint16_t volume = (uint16_t)(((uint32_t)(UI_MAP_MAX - y) * 255u) /
                                 (UI_MAP_MAX - UI_MAP_MIN));
    if(g_ui.audio.note_on) g_ui.audio.note_on(freq, volume);
    lv_label_set_text_fmt(g_ui.readout, "%.1f Hz · vol %u", freq / 100.0, volume);
}

static void pad_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_LONG_PRESSED) {
        g_ui.long_pressed = true;
        lv_obj_clear_flag(g_ui.picker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_ui.pad, LV_OBJ_FLAG_HIDDEN);
        if(g_ui.audio.note_off) g_ui.audio.note_off();
        return;
    }
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;

    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point{};
    if(indev) lv_indev_get_point(indev, &point);
    if(code == LV_EVENT_RELEASED) {
        if(!g_ui.long_pressed && !g_ui.hold && g_ui.audio.note_off) g_ui.audio.note_off();
        g_ui.long_pressed = false;
        g_ui.touching = false;
        if(!g_ui.hold) hide(g_ui.touch_halo), hide(g_ui.touch_core), hide(g_ui.touch_dot),
            hide(g_ui.cross_x), hide(g_ui.cross_y);
        return;
    }
    g_ui.touching = true;
    set_touch_point(point.x, point.y);
    add_trail(point.x, point.y);
    set_pad_audio(point.x, point.y);
}

static void hold_event(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    g_ui.hold = !g_ui.hold;
    lv_obj_set_style_bg_color(g_ui.hold_button, col(g_ui.hold ? UI_COL_ACCENT : UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.hold_button, g_ui.hold ? LV_OPA_COVER : UI_OPA_PILL_IDLE, 0);
    lv_obj_set_style_text_color(g_ui.hold_label, col(g_ui.hold ? UI_COL_ACCENT_TEXT : UI_COL_FG), 0);
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
            if(i < kMenu[g_ui.menu_group].count) lv_label_set_text(g_ui.picker_rows[i], kMenu[g_ui.menu_group].items[i]);
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
        lv_obj_set_style_text_opa(g_ui.picker_rows[i], i == g_ui.menu_item ? UI_OPA_TEXT_TITLE : UI_OPA_TEXT_LABEL, 0);
    }
    lv_obj_set_style_bg_opa(g_ui.picker_band, UI_OPA_WHEEL_IDLE, 0);
}

static void close_picker(void)
{
    lv_obj_add_flag(g_ui.picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_ui.pad, LV_OBJ_FLAG_HIDDEN);
}

static void picker_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point{};
    if(indev) lv_indev_get_point(indev, &point);
    if(code == LV_EVENT_PRESSED) {
        g_ui.press_point = point;
        g_ui.last_angle = point_angle(point);
        g_ui.wheel_degrees = 0.0f;
        g_ui.wheel = point_radius(point) >= UI_WHEEL_R_IN;
        return;
    }
    if(code == LV_EVENT_PRESSING && g_ui.wheel) {
        float delta = unwrap_delta(point_angle(point) - g_ui.last_angle);
        g_ui.last_angle = point_angle(point);
        g_ui.wheel_degrees += delta;
        while(g_ui.wheel_degrees >= UI_WHEEL_STEP_DEG) {
            g_ui.wheel_degrees -= UI_WHEEL_STEP_DEG;
            uint8_t count = g_ui.picker_in_group ? kMenu[g_ui.menu_group].count : sizeof(kMenu) / sizeof(kMenu[0]);
            g_ui.menu_item = (g_ui.menu_item + 1) % count;
            update_picker_rows();
        }
        while(g_ui.wheel_degrees <= -UI_WHEEL_STEP_DEG) {
            g_ui.wheel_degrees += UI_WHEEL_STEP_DEG;
            uint8_t count = g_ui.picker_in_group ? kMenu[g_ui.menu_group].count : sizeof(kMenu) / sizeof(kMenu[0]);
            g_ui.menu_item = (g_ui.menu_item + count - 1) % count;
            update_picker_rows();
        }
        return;
    }
    if(code != LV_EVENT_RELEASED) return;
    if(g_ui.wheel && fabsf(g_ui.wheel_degrees) < UI_WHEEL_TAP_DEG) {
        float angle = point_angle(g_ui.press_point);
        if(fabsf(angle) < UI_WHEEL_BTN_DEG) {
            close_picker();
        } else if(fabsf(fabsf(angle) - 180.0f) < UI_WHEEL_BTN_DEG) {
            g_ui.program = (g_ui.program + (angle > 0 ? 1 : UI_PROG_COUNT - 1)) % UI_PROG_COUNT;
            update_pad_text();
            close_picker();
        } else if(fabsf(fabsf(angle) - 90.0f) < UI_WHEEL_BTN_DEG) {
            if(angle > 0 && g_ui.audio.note_on) g_ui.audio.note_on(44000, 180);
            if(angle > 0 && g_ui.audio.note_off) g_ui.audio.note_off();
        }
    } else if(g_ui.wheel && point_radius(g_ui.press_point) < UI_WHEEL_R_IN) {
        if(!g_ui.picker_in_group) {
            g_ui.menu_group = g_ui.menu_item;
            g_ui.menu_item = 0;
            g_ui.picker_in_group = true;
            update_picker_rows();
        } else {
            close_picker();
            update_pad_text();
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

    static lv_point_precise_t vertical[2];
    static lv_point_precise_t horizontal[2];
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
        lv_obj_set_size(arc, UI_PROG_RING_R * 2, UI_PROG_RING_R * 2);
        lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_bg_angles(arc, i * UI_PROG_SEG_DEG + UI_PROG_SEG_GAP_DEG,
                             (i + 1) * UI_PROG_SEG_DEG - UI_PROG_SEG_GAP_DEG);
        lv_arc_set_angles(arc, i * UI_PROG_SEG_DEG + UI_PROG_SEG_GAP_DEG,
                          (i + 1) * UI_PROG_SEG_DEG - UI_PROG_SEG_GAP_DEG);
        lv_obj_set_style_arc_width(arc, UI_PROG_RING_W, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, col(i == 0 ? UI_COL_ACCENT : UI_COL_FG), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, i == 0 ? LV_OPA_COVER : UI_OPA_SEG_IDLE, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    }

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
    lv_obj_set_size(g_ui.hold_button, UI_HOLD_W, UI_HOLD_H);
    lv_obj_align(g_ui.hold_button, LV_ALIGN_TOP_MID, 0, UI_HOLD_CY - UI_HOLD_H / 2);
    lv_obj_set_style_radius(g_ui.hold_button, UI_HOLD_RADIUS, 0);
    lv_obj_set_style_bg_color(g_ui.hold_button, col(UI_COL_FG), 0);
    lv_obj_set_style_bg_opa(g_ui.hold_button, UI_OPA_PILL_IDLE, 0);
    lv_obj_set_style_border_width(g_ui.hold_button, 0, 0);
    g_ui.hold_label = label(g_ui.hold_button, "Hold", UI_FONT_SMALL, UI_OPA_TEXT_LABEL);
    lv_obj_center(g_ui.hold_label);
    lv_obj_add_event_cb(g_ui.hold_button, hold_event, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *touch = lv_obj_create(g_ui.pad);
    style_plain(touch);
    lv_obj_set_size(touch, UI_PAD_R * 2, UI_PAD_R * 2);
    lv_obj_set_pos(touch, UI_MAP_MIN, UI_MAP_MIN);
    lv_obj_add_event_cb(touch, pad_event, LV_EVENT_ALL, nullptr);
    lv_obj_set_style_radius(touch, LV_RADIUS_CIRCLE, 0);

    for(uint8_t i = 0; i < UI_TRAIL_POOL; ++i) {
        g_ui.trail[i] = circle(g_ui.pad, UI_TRAIL_R_START * 2, col(UI_COL_ACCENT), 0);
        hide(g_ui.trail[i]);
    }
    g_ui.cross_x = lv_obj_create(g_ui.pad); style_plain(g_ui.cross_x); lv_obj_set_size(g_ui.cross_x, 600, UI_CROSSHAIR_W); lv_obj_set_style_bg_color(g_ui.cross_x, col(UI_COL_ACCENT), 0); lv_obj_set_style_bg_opa(g_ui.cross_x, UI_OPA_CROSSHAIR, 0); hide(g_ui.cross_x);
    g_ui.cross_y = lv_obj_create(g_ui.pad); style_plain(g_ui.cross_y); lv_obj_set_size(g_ui.cross_y, UI_CROSSHAIR_W, 600); lv_obj_set_style_bg_color(g_ui.cross_y, col(UI_COL_ACCENT), 0); lv_obj_set_style_bg_opa(g_ui.cross_y, UI_OPA_CROSSHAIR, 0); hide(g_ui.cross_y);
    g_ui.touch_halo = circle(g_ui.pad, UI_TOUCH_HALO_R * 2, col(UI_COL_ACCENT_LIGHT), UI_OPA_TOUCH_HALO); hide(g_ui.touch_halo);
    g_ui.touch_core = circle(g_ui.pad, UI_TOUCH_CORE_R * 2, col(UI_COL_ACCENT_LIGHT), LV_OPA_COVER); hide(g_ui.touch_core);
    g_ui.touch_dot = circle(g_ui.pad, UI_TOUCH_DOT_R * 2, col(UI_COL_FG), LV_OPA_COVER); hide(g_ui.touch_dot);
    update_pad_text();
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
    label(g_ui.picker, "Menu", UI_FONT_SUB, UI_OPA_TEXT_LABEL);
    lv_obj_align(lv_obj_get_child(g_ui.picker, lv_obj_get_child_count(g_ui.picker) - 1), LV_ALIGN_TOP_MID, 0, 70);
    label(g_ui.picker, "Audition", UI_FONT_SUB, UI_OPA_TEXT_LABEL);
    lv_obj_align(lv_obj_get_child(g_ui.picker, lv_obj_get_child_count(g_ui.picker) - 1), LV_ALIGN_BOTTOM_MID, 0, -100);
    label(g_ui.picker, LV_SYMBOL_PREV, UI_FONT_SUB, UI_OPA_TEXT_LABEL);
    lv_obj_align(lv_obj_get_child(g_ui.picker, lv_obj_get_child_count(g_ui.picker) - 1), LV_ALIGN_LEFT_MID, 70, 0);
    label(g_ui.picker, LV_SYMBOL_NEXT, UI_FONT_SUB, UI_OPA_TEXT_LABEL);
    lv_obj_align(lv_obj_get_child(g_ui.picker, lv_obj_get_child_count(g_ui.picker) - 1), LV_ALIGN_RIGHT_MID, -70, 0);
    update_picker_rows();
    hide(g_ui.picker);
}

} // namespace

void synth_ui_init(lv_obj_t *screen, const synth_ui_audio_callbacks_t *audio)
{
    g_ui = UiState{};
    g_ui.screen = screen;
    if(audio) g_ui.audio = *audio;
    lv_obj_set_style_bg_color(screen, col(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    create_pad();
    create_picker();
}
