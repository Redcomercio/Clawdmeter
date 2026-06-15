#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_show_event(const SessionEvent* ev);
void ui_show_approval(const ApprovalRequest* req);
void ui_approval_tick(void);   // call each loop; handles the 30s auto-dismiss
bool ui_approval_active(void); // true while an approval card is showing
void ui_approval_primary(void);    // PRIMARY (BOOT) pressed while card up
void ui_approval_secondary(void);  // SECONDARY (KEY) pressed while card up
bool ui_approval_middle(void);     // PWR pressed while 3-option card up → key '2'
void ui_hide_approval(void);       // daemon-initiated clear (clear-ask payload)
void ui_show_milestone(const Milestone* m);
void ui_milestone_tick(void);      // call each loop; burst/proud timers
bool ui_milestone_replay(void);    // PWR while a milestone badge is up → replay burst

#define NOTIF_MAX 8
struct NotifItem { char id[40]; char proj[24]; char tool[16]; };
void ui_notif_set_list(const NotifItem* items, uint8_t count);
void ui_notif_show(void);
void ui_notif_hide(void);
bool ui_notif_visible(void);
void ui_banner_tick(void);   // call each loop; handles the 30s auto-dismiss
bool ui_banner_visible(void);
void ui_banner_dismiss(void);  // manual clear of the notification banner + state
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
