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

struct ApprovalRequest {
    char id[40];     // request id (session-epoch-pid); echoed back on decision
    char proj[24];
    char tool[16];
    char cmd[64];
    uint8_t pos;     // 1-based position in the queue
    uint8_t total;   // queue length
    uint8_t opts;    // prompt option count: 2 (Yes/No) or 3 (Yes/Yes-all/No)
    bool fresh;
};

struct Milestone {
    char label[40];  // e.g. "🔥 7 días seguidos"
    char anim[20];   // festive animation name
    bool fresh;
};
