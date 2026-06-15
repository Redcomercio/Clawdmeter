#include "ui.h"
#include "splash.h"
#include "ble.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "hal/imu_hal.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// ---- Session-event banner (floats on the top layer over any screen) ----
static lv_obj_t* banner = nullptr;
static lv_obj_t* banner_lbl = nullptr;
static uint32_t banner_hide_at = 0;   // lv_tick when a "done" banner self-hides

static void banner_tap_cb(lv_event_t* e) {
    LV_UNUSED(e);
    if (banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    banner_hide_at = 0;
}

static void banner_ensure(void) {
    if (banner) return;
    const BoardCaps& c = board_caps();
    banner = lv_obj_create(lv_layer_top());
    // Full-width strip near the top, inside the 20px rounded-corner margin.
    lv_obj_set_size(banner, c.width - 40, 56);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(banner, 12, 0);
    lv_obj_set_style_border_width(banner, 0, 0);
    lv_obj_set_style_pad_all(banner, 8, 0);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(banner, banner_tap_cb, LV_EVENT_CLICKED, nullptr);

    banner_lbl = lv_label_create(banner);
    lv_obj_set_style_text_font(banner_lbl, &font_styrene_20, 0);
    lv_obj_set_style_text_color(banner_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(banner_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(banner_lbl, c.width - 56);
    lv_obj_center(banner_lbl);
}

// ---- Speech cloud over the splash creature (approval-pending only) ----
// A puffy white "nube" with an amber "!" that appears above the creature on
// the splash screen, as if it were trying to get your attention. Coexists
// with the banner. Lives on the top layer; visibility is gated on both an
// active approval AND the splash screen being current.
static lv_obj_t* cloud = nullptr;
static bool      approval_on = false;

// Pixel-art speech bubble, drawn block-by-block to match Clawdio's 8-bit look.
// Legend:  K = dark outline   # = white fill   s = soft shadow
//          ! = amber exclamation   . = transparent
// Each char is one CELL×CELL square (CELL = splash_cell_px()), sharp corners.
// Rectangular bubble with a thick outline and a tail trailing toward Clawdio
// (lower-left, since the bubble sits upper-right of the centered creature).
static const char* const CLOUD_ART[] = {
    ".KKKKKKKKKKK.",
    "K###########K",
    "K####!#####sK",
    "K####!#####sK",
    "K####!#####sK",
    "K##########sK",
    "K####!#####sK",
    "K#########ssK",
    ".KKK#KKKKKKK.",
    "..KK#K.......",
    "...KK........",
};
#define CLOUD_ROWS 11
#define CLOUD_COLS 13

#define CLOUD_OUTLINE 0x14142b   // dark navy outline
#define CLOUD_SHADOW  0xcdcde8   // soft lavender inner shadow
#define CLOUD_AMBER   0xB8860B   // exclamation

static void cloud_block(lv_obj_t* parent, int x, int y, int s, uint32_t hex) {
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, s, s);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 0, 0);          // sharp, 8-bit corners
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(hex), 0);
}

static void cloud_ensure(void) {
    if (cloud) return;
    int s = splash_cell_px();
    if (s <= 0) s = 10;  // splash not yet initialized; fall back to C6 cell

    cloud = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cloud);
    lv_obj_set_size(cloud, CLOUD_COLS * s, CLOUD_ROWS * s);
    lv_obj_clear_flag(cloud, LV_OBJ_FLAG_SCROLLABLE);
    // Upper-right of the centered creature, clear of the top banner.
    lv_obj_align(cloud, LV_ALIGN_CENTER, 78, -96);

    for (int r = 0; r < CLOUD_ROWS; r++) {
        for (int col = 0; col < CLOUD_COLS; col++) {
            switch (CLOUD_ART[r][col]) {
            case 'K': cloud_block(cloud, col * s, r * s, s, CLOUD_OUTLINE); break;
            case '#': cloud_block(cloud, col * s, r * s, s, 0xFFFFFF);      break;
            case 's': cloud_block(cloud, col * s, r * s, s, CLOUD_SHADOW);  break;
            case '!': cloud_block(cloud, col * s, r * s, s, CLOUD_AMBER);   break;
            default: break;  // '.' transparent
            }
        }
    }

    lv_obj_add_flag(cloud, LV_OBJ_FLAG_HIDDEN);
}

// Reflect the approval state in the splash creature: pin it to a surprised
// animation while pending, release it otherwise. Independent of which screen
// is showing — the creature only renders on splash, but the pin must persist
// so it's already surprised if the user switches back to splash.
static void cloud_update_creature(void) {
    if (approval_on) splash_pin_anim("expression surprise");
    else             splash_unpin_anim();
}

// Show the cloud only while an approval is pending AND the splash is current.
static void cloud_update_visibility(void) {
    cloud_update_creature();
    if (!cloud) return;
    if (approval_on && current_screen == SCREEN_SPLASH)
        lv_obj_clear_flag(cloud, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(cloud, LV_OBJ_FLAG_HIDDEN);
}

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

void ui_show_event(const SessionEvent* ev) {
    if (!ev) return;
    banner_ensure();
    cloud_ensure();
    char text[48];

    if (strcmp(ev->type, "clear") == 0) {
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        banner_hide_at = 0;
        approval_on = false;
        cloud_update_visibility();
        return;
    }

    if (strcmp(ev->type, "approval") == 0) {
        lv_obj_set_style_bg_color(banner, lv_color_hex(0xB8860B), 0);  // amber
        if (ev->count > 1) {
            snprintf(text, sizeof(text), LV_SYMBOL_WARNING " %s  (%u pendientes)",
                     ev->proj, (unsigned)ev->count);
        } else {
            snprintf(text, sizeof(text), LV_SYMBOL_WARNING " %s  aprobacion", ev->proj);
        }
        lv_label_set_text(banner_lbl, text);
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
        banner_hide_at = 0;  // persists until daemon clears it
        approval_on = true;
        cloud_update_visibility();
        return;
    }

    if (strcmp(ev->type, "done") == 0) {
        lv_obj_set_style_bg_color(banner, lv_color_hex(0x1E7B34), 0);  // green
        snprintf(text, sizeof(text), LV_SYMBOL_OK " %s  listo", ev->proj);
        lv_label_set_text(banner_lbl, text);
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
        banner_hide_at = lv_tick_get() + 30000;  // auto-dismiss in 30s
        approval_on = false;  // nothing pending when a done payload arrives
        cloud_update_visibility();
        splash_pin_anim_for("dance bounce", 2500);  // festejo: tarea terminada
        return;
    }
}

void ui_banner_tick(void) {
    if (banner_hide_at != 0 && lv_tick_get() >= banner_hide_at) {
        if (banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        banner_hide_at = 0;
    }
}

bool ui_banner_visible(void) {
    return banner && !lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

// Manual clear: wipe the notification banner AND its pending state (the amber
// "aprobación" + Clawdio's cloud). Safety valve for when the daemon's clear
// event is missed and a banner stays stuck after you resolved it in the terminal.
void ui_banner_dismiss(void) {
    if (banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    banner_hide_at = 0;
    approval_on = false;
    cloud_update_visibility();   // hides the cloud + unpins Clawdio
}

// ---- Approval card (overlays any screen; decided with the physical buttons) ----
// Sized for the 480x480 square AMOLED. The screen auto-rotates upright, so the
// physically-upper action button = "Aprobar" (top label) and the lower one =
// "Continuar" (bottom label, defers to the terminal). Rotation is FROZEN while
// the card is up so the button↔label mapping can't shift under the user.
//
// Which physical button (PRIMARY/BOOT vs SECONDARY/KEY) is the upper one depends
// on the frozen quadrant. PRIMARY_IS_UPPER() encodes that; tuned on hardware.
static lv_obj_t* card = nullptr;
static lv_obj_t* card_proj = nullptr;
static lv_obj_t* card_tool = nullptr;
static lv_obj_t* card_cmd  = nullptr;
static lv_obj_t* card_pos  = nullptr;
static lv_obj_t* card_yes_lbl = nullptr;  // top    → key '1' (Yes once)
static lv_obj_t* card_mid_lbl = nullptr;  // middle → key '2' (Yes allow all; 3-opt only)
static lv_obj_t* card_no_lbl  = nullptr;  // bottom → key '2'(2-opt) / '3'(3-opt) (No)
static char      card_id[40] = {0};
static uint32_t  card_hide_at = 0;     // lv_tick when the 30s timeout fires
static bool      card_visible = false;
static uint8_t   card_quadrant = 0;    // rotation quadrant frozen at show time
static uint8_t   card_opts = 2;        // 2 (Yes/No) or 3 (Yes/Yes-all/No)

// At quadrant 0 the PRIMARY (BOOT) button is the gravity-upper one; at 180°
// (quadrant 2) the column flips so SECONDARY is upper. Quadrants 1/3 (buttons
// horizontal) have no true upper — default to the quadrant-0 assignment.
// QA on hardware: if approve/continue feel swapped, flip this predicate.
static bool primary_is_upper(uint8_t q) { return !(q == 2); }

// ---- Decision confirmation flash (full-screen colour + word, ~1.5s) ----
static lv_obj_t* confirm = nullptr;
static lv_obj_t* confirm_lbl = nullptr;
static uint32_t  confirm_hide_at = 0;

static void confirm_ensure(void) {
    if (confirm) return;
    const BoardCaps& c = board_caps();
    confirm = lv_obj_create(lv_layer_top());
    lv_obj_set_size(confirm, c.width, c.height);
    lv_obj_align(confirm, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(confirm, 0, 0);
    lv_obj_set_style_border_width(confirm, 0, 0);
    lv_obj_clear_flag(confirm, LV_OBJ_FLAG_SCROLLABLE);
    confirm_lbl = lv_label_create(confirm);
    lv_obj_set_style_text_font(confirm_lbl, &font_styrene_48, 0);
    lv_obj_center(confirm_lbl);
    lv_obj_add_flag(confirm, LV_OBJ_FLAG_HIDDEN);
}

// Flash feedback for the digit typed: 1=Sí (green), 2=Permitir todo (green),
// 3=No (red). digit 0 = silent dismiss (no flash).
static void confirm_flash_digit(uint8_t digit) {
    confirm_ensure();
    if (digit == 1) {
        lv_obj_set_style_bg_color(confirm, lv_color_hex(0x1E7B34), 0);   // green
        lv_obj_set_style_text_color(confirm_lbl, lv_color_white(), 0);
        lv_label_set_text(confirm_lbl, LV_SYMBOL_OK "  Sí");
    } else if (digit == 2) {
        lv_obj_set_style_bg_color(confirm, lv_color_hex(0x1E7B34), 0);   // green
        lv_obj_set_style_text_color(confirm_lbl, lv_color_white(), 0);
        lv_label_set_text(confirm_lbl, LV_SYMBOL_OK "  Permitir todo");
    } else {
        lv_obj_set_style_bg_color(confirm, lv_color_hex(0xB23b3b), 0);   // red
        lv_obj_set_style_text_color(confirm_lbl, lv_color_white(), 0);
        lv_label_set_text(confirm_lbl, "No");
    }
    lv_obj_center(confirm_lbl);
    lv_obj_move_foreground(confirm);
    lv_obj_clear_flag(confirm, LV_OBJ_FLAG_HIDDEN);
    confirm_hide_at = lv_tick_get() + 1500;
}

// Hide the card + release pins, without typing anything (timeout / daemon clear).
static void card_dismiss_silent(void) {
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = 0;
    card_visible = false;
    card_id[0] = '\0';
    imu_hal_lock_rotation(false);
    splash_unpin_anim();
}

// A button answered the prompt: type the digit (1/2/3) into the focused terminal,
// tell the daemon to advance the queue, flash feedback.
static void card_answer(uint8_t digit) {
    if (!card_visible) return;
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = 0;
    card_visible = false;
    imu_hal_lock_rotation(false);
    uint8_t key = (digit == 1) ? 0x1E : (digit == 2) ? 0x1F : 0x20;  // '1'/'2'/'3'
    ble_keyboard_press(key, 0);
    ble_keyboard_release();
    if (card_id[0]) {
        ble_send_decision(card_id, digit == 3 ? "dismiss" : "approve");  // advances queue
        card_id[0] = '\0';
    }
    splash_unpin_anim();
    if (digit != 3) splash_pin_anim_for("expression wink", 1500);  // contento
    confirm_flash_digit(digit);
}

// Daemon-initiated clear (prompt resolved in the terminal, or timed out at 60s).
void ui_hide_approval(void) {
    if (!card_visible) return;
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = 0;
    card_visible = false;
    card_id[0] = '\0';
    imu_hal_lock_rotation(false);
    splash_unpin_anim();
}

// ---- Milestone celebration: burst + proud state + tappable replay badge ----
static lv_obj_t* mile_toast = nullptr;
static lv_obj_t* mile_toast_lbl = nullptr;
static lv_obj_t* mile_badge = nullptr;
static lv_obj_t* mile_badge_lbl = nullptr;
static char      mile_label[40] = {0};
static char      mile_anim[20] = {0};
static uint32_t  mile_toast_hide_at = 0;   // burst toast end
static uint32_t  mile_proud_until = 0;     // proud-state end

static void mile_burst(void) {
    splash_pin_anim_for(mile_anim, 5000);                 // festive dance ~5s
    lv_label_set_text(mile_toast_lbl, mile_label);
    lv_obj_clear_flag(mile_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mile_toast);
    mile_toast_hide_at = lv_tick_get() + 5000;
    // proud window: random 30–60 min (gentle sway), badge stays visible.
    mile_proud_until = lv_tick_get() + (uint32_t)random(1800000, 3600001);
}

static void mile_ensure(void) {
    if (mile_toast) return;
    const BoardCaps& c = board_caps();
    // Festive toast — wide strip near the top.
    mile_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mile_toast, c.width - 40, 64);
    lv_obj_align(mile_toast, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(mile_toast, 14, 0);
    lv_obj_set_style_border_width(mile_toast, 0, 0);
    lv_obj_set_style_bg_color(mile_toast, lv_color_hex(0x7a5cff), 0);  // festive purple
    lv_obj_clear_flag(mile_toast, LV_OBJ_FLAG_SCROLLABLE);
    mile_toast_lbl = lv_label_create(mile_toast);
    lv_obj_set_style_text_font(mile_toast_lbl, &font_styrene_24, 0);
    lv_obj_set_style_text_color(mile_toast_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(mile_toast_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(mile_toast_lbl, c.width - 72);
    lv_obj_center(mile_toast_lbl);
    lv_obj_add_flag(mile_toast, LV_OBJ_FLAG_HIDDEN);

    // Persistent tappable badge — bottom-left corner.
    mile_badge = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mile_badge, 64, 64);
    lv_obj_align(mile_badge, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_radius(mile_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mile_badge, 0, 0);
    lv_obj_set_style_bg_color(mile_badge, lv_color_hex(0x7a5cff), 0);
    lv_obj_clear_flag(mile_badge, LV_OBJ_FLAG_SCROLLABLE);
    // Badge is purely visual; replay is via the PWR button (touch is unreliable
    // when the display is rotated — see ui_milestone_replay).
    mile_badge_lbl = lv_label_create(mile_badge);
    lv_obj_set_style_text_font(mile_badge_lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(mile_badge_lbl, lv_color_white(), 0);
    lv_label_set_text(mile_badge_lbl, LV_SYMBOL_OK);  // trophy stand-in
    lv_obj_center(mile_badge_lbl);
    lv_obj_add_flag(mile_badge, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_milestone(const Milestone* m) {
    if (!m) return;
    mile_ensure();
    strlcpy(mile_label, m->label, sizeof(mile_label));
    strlcpy(mile_anim, m->anim[0] ? m->anim : "dance bounce", sizeof(mile_anim));
    lv_obj_clear_flag(mile_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mile_badge);
    mile_burst();
}

void ui_milestone_tick(void) {
    uint32_t now = lv_tick_get();
    if (mile_toast_hide_at != 0 && now >= mile_toast_hide_at) {
        if (mile_toast) lv_obj_add_flag(mile_toast, LV_OBJ_FLAG_HIDDEN);
        mile_toast_hide_at = 0;
        // After the burst, hold a calm-happy proud expression for the window.
        if (mile_proud_until > now) splash_pin_anim_for("dance sway", mile_proud_until - now);
    }
    if (mile_proud_until != 0 && now >= mile_proud_until) {
        mile_proud_until = 0;
        if (mile_badge) lv_obj_add_flag(mile_badge, LV_OBJ_FLAG_HIDDEN);  // badge decays
    }
}

// PWR button while a milestone badge is up → replay the festive burst.
// Returns true if it consumed the press (so the caller skips normal PWR action).
bool ui_milestone_replay(void) {
    if (mile_proud_until == 0 || !mile_label[0]) return false;  // no active celebration
    mile_burst();
    return true;
}

// ---- Notification center: full-screen list of stuck/active items ----
static lv_obj_t* notif_panel = nullptr;
static lv_obj_t* notif_list = nullptr;   // scrollable container of rows
static lv_obj_t* notif_empty = nullptr;
static NotifItem notif_items[NOTIF_MAX];
static uint8_t   notif_count = 0;

static void notif_x_cb(lv_event_t* e) {
    const char* id = (const char*)lv_event_get_user_data(e);
    if (id && id[0]) ble_send_decision(id, "notifclear");  // daemon clears that session's pending
    lv_obj_t* row = (lv_obj_t*)lv_obj_get_parent(lv_event_get_target_obj(e));
    if (row) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
}

static void notif_close_cb(lv_event_t* e) { LV_UNUSED(e); ui_notif_hide(); }

static void notif_ensure(void) {
    if (notif_panel) return;
    const BoardCaps& c = board_caps();
    notif_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(notif_panel, c.width, c.height);
    lv_obj_align(notif_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(notif_panel, 0, 0);
    lv_obj_set_style_border_width(notif_panel, 0, 0);
    lv_obj_set_style_bg_color(notif_panel, lv_color_hex(0x14141c), 0);
    lv_obj_set_style_pad_all(notif_panel, 12, 0);
    lv_obj_clear_flag(notif_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(notif_panel);
    lv_obj_set_style_text_font(title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Notificaciones");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    notif_empty = lv_label_create(notif_panel);
    lv_obj_set_style_text_font(notif_empty, &font_styrene_20, 0);
    lv_obj_set_style_text_color(notif_empty, lv_color_hex(0x8a8a92), 0);
    lv_label_set_text(notif_empty, "Sin notificaciones");
    lv_obj_center(notif_empty);

    notif_list = lv_obj_create(notif_panel);
    lv_obj_set_size(notif_list, c.width - 24, c.height - 130);
    lv_obj_align(notif_list, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(notif_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(notif_list, 0, 0);
    lv_obj_set_flex_flow(notif_list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* close = lv_obj_create(notif_panel);
    lv_obj_set_size(close, c.width - 80, 52);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(close, 12, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x3a3a44), 0);
    lv_obj_add_event_cb(close, notif_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* close_lbl = lv_label_create(close);
    lv_obj_set_style_text_font(close_lbl, &font_styrene_20, 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), 0);
    lv_label_set_text(close_lbl, "Cerrar");
    lv_obj_center(close_lbl);

    lv_obj_add_flag(notif_panel, LV_OBJ_FLAG_HIDDEN);
}

static void notif_render(void) {
    notif_ensure();
    lv_obj_clean(notif_list);                       // drop old rows
    if (notif_count == 0) lv_obj_clear_flag(notif_empty, LV_OBJ_FLAG_HIDDEN);
    else                  lv_obj_add_flag(notif_empty, LV_OBJ_FLAG_HIDDEN);
    const BoardCaps& c = board_caps();
    for (uint8_t i = 0; i < notif_count; i++) {
        lv_obj_t* row = lv_obj_create(notif_list);
        lv_obj_set_size(row, c.width - 32, 60);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x202028), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* txt = lv_label_create(row);
        lv_obj_set_style_text_font(txt, &font_styrene_20, 0);
        lv_obj_set_style_text_color(txt, lv_color_white(), 0);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_DOT);
        lv_obj_set_width(txt, c.width - 120);
        char line[44];
        snprintf(line, sizeof(line), "%s \xC2\xB7 %s", notif_items[i].proj, notif_items[i].tool);
        lv_label_set_text(txt, line);
        lv_obj_align(txt, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t* x = lv_obj_create(row);
        lv_obj_set_size(x, 44, 44);
        lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(x, 0, 0);
        lv_obj_set_style_bg_color(x, lv_color_hex(0xB23b3b), 0);
        lv_obj_align(x, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_event_cb(x, notif_x_cb, LV_EVENT_CLICKED, (void*)notif_items[i].id);
        lv_obj_t* xl = lv_label_create(x);
        lv_obj_set_style_text_font(xl, &font_styrene_20, 0);
        lv_obj_set_style_text_color(xl, lv_color_white(), 0);
        lv_label_set_text(xl, LV_SYMBOL_CLOSE);
        lv_obj_center(xl);
    }
}

void ui_notif_set_list(const NotifItem* items, uint8_t count) {
    if (count > NOTIF_MAX) count = NOTIF_MAX;
    notif_count = count;
    for (uint8_t i = 0; i < count; i++) notif_items[i] = items[i];
    if (ui_notif_visible()) notif_render();
}

void ui_notif_show(void) {
    notif_ensure();
    notif_render();
    lv_obj_clear_flag(notif_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(notif_panel);
}

void ui_notif_hide(void) {
    if (notif_panel) lv_obj_add_flag(notif_panel, LV_OBJ_FLAG_HIDDEN);
}

bool ui_notif_visible(void) {
    return notif_panel && !lv_obj_has_flag(notif_panel, LV_OBJ_FLAG_HIDDEN);
}

static void card_ensure(void) {
    if (card) return;
    const BoardCaps& c = board_caps();
    card = lv_obj_create(lv_layer_top());
    // Near-full-screen square card for the small 2.16" 480x480 panel — big text.
    lv_obj_set_size(card, c.width - 16, c.height - 16);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x202028), 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Action labels on the RIGHT edge, aligned to the 3 physical buttons:
    // top = Sí (key 1), middle = Sí, todo (key 2, 3-option only), bottom = No
    // (key 2 for 2-option prompts, key 3 for 3-option). Middle hidden for 2-opt.
    card_yes_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(card_yes_lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(card_yes_lbl, lv_color_hex(0x35c46a), 0);
    lv_label_set_text(card_yes_lbl, "Sí  " LV_SYMBOL_RIGHT);
    lv_obj_align(card_yes_lbl, LV_ALIGN_RIGHT_MID, 0, -150);

    card_mid_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(card_mid_lbl, &font_styrene_24, 0);
    lv_obj_set_style_text_color(card_mid_lbl, lv_color_hex(0xB8860B), 0);
    lv_label_set_text(card_mid_lbl, "Sí, todo  " LV_SYMBOL_RIGHT);
    lv_obj_align(card_mid_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    card_no_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(card_no_lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(card_no_lbl, lv_color_hex(0x9a9aa2), 0);
    lv_label_set_text(card_no_lbl, "No  " LV_SYMBOL_RIGHT);
    lv_obj_align(card_no_lbl, LV_ALIGN_RIGHT_MID, 0, 150);

    // Content (project / tool / command) fills the left, clear of the buttons.
    int32_t content_w = c.width - 72;

    card_proj = lv_label_create(card);
    lv_obj_set_style_text_font(card_proj, &font_styrene_48, 0);
    lv_obj_set_style_text_color(card_proj, lv_color_white(), 0);
    lv_label_set_long_mode(card_proj, LV_LABEL_LONG_DOT);
    lv_obj_set_width(card_proj, content_w);
    lv_obj_align(card_proj, LV_ALIGN_LEFT_MID, 0, -40);

    card_tool = lv_label_create(card);
    lv_obj_set_style_text_font(card_tool, &font_styrene_28, 0);
    lv_obj_set_style_text_color(card_tool, lv_color_hex(0xB8860B), 0);
    lv_obj_align(card_tool, LV_ALIGN_LEFT_MID, 0, 12);

    card_cmd = lv_label_create(card);
    lv_obj_set_style_text_font(card_cmd, &font_styrene_24, 0);
    lv_obj_set_style_text_color(card_cmd, lv_color_hex(0xcfcfd6), 0);
    lv_label_set_long_mode(card_cmd, LV_LABEL_LONG_DOT);
    lv_obj_set_width(card_cmd, content_w);
    lv_obj_align(card_cmd, LV_ALIGN_LEFT_MID, 0, 52);

    card_pos = lv_label_create(card);
    lv_obj_set_style_text_font(card_pos, &font_styrene_16, 0);
    lv_obj_set_style_text_color(card_pos, lv_color_hex(0x9a9aa2), 0);
    lv_obj_align(card_pos, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_approval(const ApprovalRequest* req) {
    if (!req) return;
    card_ensure();
    strlcpy(card_id, req->id, sizeof(card_id));
    lv_label_set_text(card_proj, req->proj);
    lv_label_set_text(card_tool, req->tool);
    lv_label_set_text(card_cmd, req->cmd);
    char pos[16];
    snprintf(pos, sizeof(pos), "%u de %u", (unsigned)req->pos, (unsigned)req->total);
    lv_label_set_text(card_pos, pos);
    card_opts = (req->opts == 3) ? 3 : 2;
    if (card_opts == 3) lv_obj_clear_flag(card_mid_lbl, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(card_mid_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = lv_tick_get() + 30000;  // auto-dismiss in 30s
    card_visible = true;
    card_quadrant = imu_hal_rotation_quadrant();
    imu_hal_lock_rotation(true);            // freeze so buttons don't remap
    splash_pin_anim("expression surprise");
}

bool ui_approval_active(void) { return card_visible; }

// Top button = Sí (key 1); bottom button = No (key 2 for 2-opt, key 3 for 3-opt).
// Which physical button is "top" comes from the frozen quadrant (primary_is_upper).
static uint8_t bottom_digit(void) { return card_opts == 3 ? 3 : 2; }

void ui_approval_primary(void) {
    if (!card_visible) return;
    card_answer(primary_is_upper(card_quadrant) ? 1 : bottom_digit());
}

void ui_approval_secondary(void) {
    if (!card_visible) return;
    card_answer(primary_is_upper(card_quadrant) ? bottom_digit() : 1);
}

// Middle (PWR) button while a 3-option card is up → key '2' (Yes, allow all).
// Returns true if it consumed the press (so the caller skips normal PWR action).
bool ui_approval_middle(void) {
    if (!card_visible || card_opts != 3) return false;
    card_answer(2);
    return true;
}

void ui_approval_tick(void) {
    if (card_hide_at != 0 && lv_tick_get() >= card_hide_at) {
        card_dismiss_silent();   // timeout: no keystroke, just clear
    }
    if (confirm_hide_at != 0 && lv_tick_get() >= confirm_hide_at) {
        if (confirm) lv_obj_add_flag(confirm, LV_OBJ_FLAG_HIDDEN);
        confirm_hide_at = 0;
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    build_pair_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    int s_pct = (int)(data->session_pct + 0.5f);

    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = ble_has_bonds() ? "Disconnected" : "Pairing";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
    cloud_update_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    // Connected → usage panels; otherwise → pairing hint. The bottom status
    // line carries the live state word (Connected / Disconnected / Pairing).
    if (usage_group && pair_group) {
        if (s_ble_connected) {
            lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
