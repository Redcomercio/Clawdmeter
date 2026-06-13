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
static char      card_id[40] = {0};
static uint32_t  card_hide_at = 0;     // lv_tick when the 30s timeout fires
static bool      card_visible = false;
static uint8_t   card_quadrant = 0;    // rotation quadrant frozen at show time

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

static void confirm_flash(const char* decision) {
    confirm_ensure();
    if (strcmp(decision, "approve") == 0) {
        lv_obj_set_style_bg_color(confirm, lv_color_hex(0x1E7B34), 0);   // green
        lv_obj_set_style_text_color(confirm_lbl, lv_color_white(), 0);
        lv_label_set_text(confirm_lbl, LV_SYMBOL_OK "  Aprobado");
    } else {
        lv_obj_set_style_bg_color(confirm, lv_color_hex(0xD9A521), 0);   // yellow
        lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(0x202028), 0);
        lv_label_set_text(confirm_lbl, "Terminal");
    }
    lv_obj_center(confirm_lbl);
    lv_obj_move_foreground(confirm);
    lv_obj_clear_flag(confirm, LV_OBJ_FLAG_HIDDEN);
    confirm_hide_at = lv_tick_get() + 1500;
}

static void card_finish(const char* decision) {
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = 0;
    card_visible = false;
    imu_hal_lock_rotation(false);   // resume auto-rotation
    if (card_id[0]) {
        ble_send_decision(card_id, decision);
        card_id[0] = '\0';
    }
    splash_unpin_anim();
    confirm_flash(decision);        // visual confirmation of what was pressed
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

    // Action labels live on the RIGHT edge, aligned to the two outer physical
    // buttons (upper third = upper button, lower third = lower button; the
    // middle button sits between them). Upper = Continuar (approve), lower =
    // Terminal (defer to the terminal).
    lv_obj_t* top = lv_label_create(card);
    lv_obj_set_style_text_font(top, &font_styrene_28, 0);
    lv_obj_set_style_text_color(top, lv_color_hex(0x35c46a), 0);
    lv_label_set_text(top, "Continuar  " LV_SYMBOL_RIGHT);
    lv_obj_align(top, LV_ALIGN_RIGHT_MID, 0, -120);

    lv_obj_t* bot = lv_label_create(card);
    lv_obj_set_style_text_font(bot, &font_styrene_28, 0);
    lv_obj_set_style_text_color(bot, lv_color_hex(0x9a9aa2), 0);
    lv_label_set_text(bot, "Terminal  " LV_SYMBOL_RIGHT);
    lv_obj_align(bot, LV_ALIGN_RIGHT_MID, 0, 120);

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
    lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = lv_tick_get() + 30000;  // auto-dismiss in 30s
    card_visible = true;
    card_quadrant = imu_hal_rotation_quadrant();
    imu_hal_lock_rotation(true);            // freeze so buttons don't remap
    splash_pin_anim("expression surprise");
}

bool ui_approval_active(void) { return card_visible; }

// Physical buttons resolve the card. The upper button approves; the lower one
// defers to the terminal. Which physical button is upper comes from the frozen
// quadrant (see primary_is_upper).
void ui_approval_primary(void) {
    if (!card_visible) return;
    card_finish(primary_is_upper(card_quadrant) ? "approve" : "dismiss");
}

void ui_approval_secondary(void) {
    if (!card_visible) return;
    card_finish(primary_is_upper(card_quadrant) ? "dismiss" : "approve");
}

void ui_approval_tick(void) {
    if (card_hide_at != 0 && lv_tick_get() >= card_hide_at) {
        card_finish("dismiss");
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
