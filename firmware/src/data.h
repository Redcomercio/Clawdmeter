#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

struct SessionEvent {
    char type[12];   // "approval" | "done" | "clear"
    char proj[24];   // project name, truncated
    uint8_t count;   // pending approvals (>=1 for "approval")
    bool fresh;      // set when a new event arrives, cleared by the UI
};
