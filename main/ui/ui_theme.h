/**
 * ui_theme.h — Visual style constants for the wavesharesynth UI (LVGL 9).
 *
 * Source of truth: docs/UI_STYLE.md and the reference mockups in docs/mockups/.
 * All geometry is in screen pixels on the 720x720 round panel, origin top-left,
 * centre at (UI_CX, UI_CY). Angles are degrees, 0 = top (12 o'clock), clockwise.
 *
 * Header-only, no LVGL calls: include it from ui code and wrap colours with
 * lv_color_hex(UI_COL_xxx). Opacities are LVGL lv_opa_t values (0-255).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Screen
 * ------------------------------------------------------------------------- */
#define UI_SCREEN_W            720
#define UI_SCREEN_H            720
#define UI_CX                  360
#define UI_CY                  360
#define UI_SCREEN_R            360

/* ---------------------------------------------------------------------------
 * Palette — dark background, one amber accent, white at varying opacity
 * ------------------------------------------------------------------------- */
#define UI_COL_BG              0x161615  /* screen background                 */
#define UI_COL_FG              0xFFFFFF  /* text / lines, used with opacity    */
#define UI_COL_ACCENT          0xEF9F27  /* amber: active segment, trail, pill */
#define UI_COL_ACCENT_LIGHT    0xFAC775  /* touch point core, toast text       */
#define UI_COL_ACCENT_TEXT     0x412402  /* text on an amber fill              */

/* White-on-dark opacity steps (alpha of UI_COL_FG) */
#define UI_OPA_GRID            18   /* ~7%  grid lines                         */
#define UI_OPA_HAIRLINE        31   /* ~12% pad edge ring                      */
#define UI_OPA_SEG_IDLE        33   /* ~13% inactive program segment           */
#define UI_OPA_PILL_IDLE       26   /* ~10% hold pill (off) / scroll track     */
#define UI_OPA_TEXT_TITLE      224  /* ~88% program / screen title            */
#define UI_OPA_TEXT_LABEL      153  /* ~60% edge labels, pill text (off)       */
#define UI_OPA_TEXT_SUB        128  /* ~50% subtitle, readout                  */
#define UI_OPA_TEXT_HINT       89   /* ~35% axis labels, footer hint           */
#define UI_OPA_TEXT_FAINT      77   /* ~30% scale-lane note names              */

/* ---------------------------------------------------------------------------
 * Typography — LVGL built-in Montserrat. Enable these sizes in menuconfig:
 * CONFIG_LV_FONT_MONTSERRAT_{14,16,18,20,22,26,30}=y
 * Arrow glyphs: use LV_SYMBOL_PREV / LV_SYMBOL_NEXT (in the built-in fonts).
 * ------------------------------------------------------------------------- */
#define UI_FONT_TITLE          (&lv_font_montserrat_30)  /* program name        */
#define UI_FONT_LIST           (&lv_font_montserrat_26)  /* picker rows         */
#define UI_FONT_SUB            (&lv_font_montserrat_22)  /* Pxx · CPU, edge lbl */
#define UI_FONT_READOUT        (&lv_font_montserrat_20)  /* X/Y values, toast   */
#define UI_FONT_SMALL          (&lv_font_montserrat_18)  /* hold pill, crumb    */
#define UI_FONT_HINT           (&lv_font_montserrat_16)  /* axis lbl, lanes, hint */
#define UI_FONT_RING           (&lv_font_montserrat_14)  /* compact outer-ring labels */

/* ===========================================================================
 * PAD SCREEN
 * ========================================================================= */

/* Playable area */
#define UI_PAD_R               300  /* touches clamp to this radius             */
#define UI_PAD_EDGE_R          302  /* hairline ring radius                     */
#define UI_PAD_EDGE_W          2

/* Grid (static background image) */
#define UI_GRID_STEP           60
#define UI_GRID_LINE_W         2

/* Program ring: 12 segments around the edge */
#define UI_PROG_COUNT          12
#define UI_PROG_RING_R         338  /* arc centre radius                        */
#define UI_PROG_RING_W         18
#define UI_PROG_SEG_DEG        30
#define UI_PROG_SEG_GAP_DEG    2    /* gap at each end of a segment (~0.04 rad) */
#define UI_SLOW_FRAME_MS       40   /* log frames slower than this            */
#define UI_LONG_PRESS_MS       600  /* hold on a wedge to reassign its instrument */
#define UI_LONG_PRESS_SLOP     14   /* px of drift allowed during the hold        */
#define UI_TAP_MAX_MS          300  /* a wedge press shorter than this is a tap   */
#define UI_DOUBLE_TAP_MS       350  /* second tap within this opens note editor */

/* Per-wedge note editor */
#define UI_NOTES_TABS_Y        100
#define UI_NOTES_TOP           172
#define UI_NOTES_ROW_H         74
#define UI_NOTES_ROW_W         440
#define UI_NOTES_BTN           64

/* Value mapping: X/Y normalised over this box (pad diameter) */
#define UI_MAP_MIN             60
#define UI_MAP_MAX             660  /* y is inverted: top = 1.0                 */

/* Touch point */
#define UI_TOUCH_HALO_R        40   /* ACCENT_LIGHT @ UI_OPA_TOUCH_HALO          */
#define UI_TOUCH_CORE_R        24   /* ACCENT_LIGHT solid                       */
#define UI_TOUCH_DOT_R         10   /* white solid                              */
#define UI_OPA_TOUCH_HALO      64   /* ~25%                                     */
#define UI_CROSSHAIR_W         2
#define UI_OPA_CROSSHAIR       89   /* ~35% ACCENT                              */

/* Light trail: pool of reused circles, fade with lv_anim */
#define UI_TRAIL_POOL          20
#define UI_TRAIL_LIFE_MS       700
#define UI_TRAIL_R_START       34   /* radius at birth                          */
#define UI_TRAIL_R_END         4    /* radius at death                          */
#define UI_OPA_TRAIL_START     115  /* ~45% ACCENT, fades linearly to 0         */

/* Scale lanes (note programs) */
#define UI_LANE_COUNT          11   /* C minor pentatonic C3..C5                 */
#define UI_LANE_LABEL_Y        470
#define UI_OPA_LANE_ACTIVE     31   /* ~12% ACCENT fill on the active lane       */

/* Text placement (centre-anchored) */
#define UI_PAD_TITLE_Y         100
#define UI_PAD_SUB_Y           134  /* "P04 · CPU 28%"                          */
#define UI_PAD_XLABEL_Y        572  /* "Cutoff →"                               */
#define UI_PAD_READOUT_Y       612  /* "Cutoff 1.2 kHz · resonance Q 4.1"       */
#define UI_PAD_YLABEL_X        92   /* rotated -90°, centred on UI_CY           */

/* Hold pill */
#define UI_HOLD_CX             360
#define UI_HOLD_CY             520
#define UI_HOLD_W              92
#define UI_HOLD_H              32
#define UI_HOLD_RADIUS         16

/* Gestures */
#define UI_LONGPRESS_MS        600  /* centre long-press → instrument picker     */
#define UI_TOUCH_POLL_HZ       100  /* minimum touch sample rate                 */
#define UI_PARAM_SMOOTH_MS     5    /* audio-side parameter smoothing            */

/* ===========================================================================
 * INSTRUMENT PICKER (iPod-style click wheel) — SILENT: no click, no auto-audition
 * ========================================================================= */

/* Wheel band */
#define UI_WHEEL_R_IN          235  /* touches starting at r >= this = wheel     */
#define UI_WHEEL_R_OUT         335
#define UI_WHEEL_R_MID         285  /* draw as one arc: r=285, width=100         */
#define UI_WHEEL_W             100
#define UI_OPA_WHEEL_IDLE      13   /* ~5%                                      */
#define UI_OPA_WHEEL_ACTIVE    23   /* ~9% while a wheel gesture is live         */

/* Scrolling */
#define UI_WHEEL_STEP_DEG      24   /* one list item per 24° of travel (15/rev)  */
#define UI_WHEEL_TAP_DEG       8    /* < 8° total movement = tap, not scroll     */
#define UI_WHEEL_BTN_DEG       30   /* edge-button zone = ±30° around each axis  */
#define UI_LIST_ANIM_MS        150  /* list y-offset ease-out toward selection   */

/* Detent ticks (visual feedback only) */
#define UI_TICK_COUNT          15
#define UI_TICK_R_IN           326
#define UI_TICK_R_OUT          336
#define UI_TICK_W              3
#define UI_TICK_W_FLASH        5
#define UI_OPA_TICK            38   /* ~15%                                     */
#define UI_TICK_FLASH_MS       180  /* nearest tick flashes ACCENT on each step  */

/* Scroll-position arc (outer edge, top) */
#define UI_SCROLL_R            350
#define UI_SCROLL_W            6
#define UI_SCROLL_SPAN_DEG     57   /* track spans ±57° around 12 o'clock        */
#define UI_SCROLL_THUMB_DEG    7    /* thumb half-width                          */

/* Edge buttons (labels at r=287): top Menu/back, bottom Audition, left/right program */
#define UI_EDGE_LABEL_R        287

/* List */
#define UI_LIST_CLIP_TOP       210
#define UI_LIST_CLIP_BOT       510
#define UI_LIST_ROW_H          58
#define UI_LIST_SEL_W          320
#define UI_LIST_DRAG_SLOP      12   /* px before a list press becomes a scroll  */
#define UI_LIST_SEL_H          52
#define UI_LIST_SEL_RADIUS     26
#define UI_OPA_ROW_NEAR        191  /* ~75% at 0 px from centre                 */
#define UI_OPA_ROW_FAR         38   /* ~15% floor                                */
#define UI_ROW_FADE_PX         220  /* opacity falls ~linearly over this distance */

#define UI_CRUMB_Y             182  /* "Instruments › Keys"                      */
#define UI_HINT_Y              542  /* "Tap center to select" / toast            */
#define UI_TOAST_MS            1600

#ifdef __cplusplus
}
#endif
