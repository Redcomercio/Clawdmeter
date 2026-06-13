#include "usage_rate.h"
#include <Arduino.h>

// Thresholds in %/min. A 5-hour (300 min) session ÷ 100% = 0.33 %/min to fill
// exactly at the same pace as the session itself resets — the user wants the
// "heavy" tier to start right there (filling in 4–5 hours).
//   < 0.10  →  Idle    (17h+ to fill, basically dormant)
//   < 0.20  →  Normal  (8–17h to fill, slow steady use)
//   < 0.33  →  Active  (5–8h, heavy but not yet pace-matching)
//   >=0.33  →  Heavy   (≤5h, matching or beating the session reset)
#define RATE_THRESH_NORMAL  0.10f
#define RATE_THRESH_ACTIVE  0.20f
#define RATE_THRESH_HEAVY   0.33f

// Minimum span between oldest and newest sample before we trust the computed
// rate. The whole point of the ring buffer is to smooth out single-sample
// jitter — at 60s daemon polling, a 1% bump between two consecutive samples
// looks like 1 %/min (Heavy) but really just means you grew 1% in the last
// minute. We require ~4 min of accumulated history so the rate reflects a
// real trend, not one noisy delta. Side-effect: ~4 min warm-up after boot
// during which we report Idle.
#define MIN_WINDOW_MS       240000UL

#define RING_SIZE 6

struct Sample { uint32_t ms; float pct; };

static Sample ring[RING_SIZE];
static uint8_t count = 0;
static uint8_t head  = 0;  // index of next write slot
static float   last_pct = 0.0f;  // most recent absolute session level

static inline uint8_t oldest_idx(void) {
    return (head + RING_SIZE - count) % RING_SIZE;
}

static void usage_rate_reset(void) {
    count = 0;
    head  = 0;
}

void usage_rate_sample(float session_pct) {
    uint32_t now = millis();

    if (count > 0) {
        uint8_t latest = (head + RING_SIZE - 1) % RING_SIZE;
        // Session reset: pct dropped substantially. Restart tracking.
        if (session_pct + 5.0f < ring[latest].pct) {
            usage_rate_reset();
        }
    }

    ring[head] = { now, session_pct };
    head = (head + 1) % RING_SIZE;
    if (count < RING_SIZE) count++;
    last_pct = session_pct;
}

int usage_rate_group(void) {
    if (count < 2) return 0;

    uint8_t o = oldest_idx();
    uint8_t l = (head + RING_SIZE - 1) % RING_SIZE;
    uint32_t dt = ring[l].ms - ring[o].ms;
    if (dt < MIN_WINDOW_MS) return 0;

    float dp = ring[l].pct - ring[o].pct;
    if (dp < 0.0f) dp = 0.0f;
    float rate = dp * 60000.0f / (float)dt;

    if (rate < RATE_THRESH_NORMAL) return 0;
    if (rate < RATE_THRESH_ACTIVE) return 1;
    if (rate < RATE_THRESH_HEAVY)  return 2;
    return 3;
}

// Absolute-level fatigue thresholds (session %). Clawdio's optimum is ~30% —
// he loves working — and he's wiped out past 80%.
#define MOOD_WAKING_MAX   12.0f   // < 12%  : just warming up
#define MOOD_ZONE_MAX     45.0f   // 12–45% : in the zone (peak ~30%)
#define MOOD_BUSY_MAX     65.0f   // 45–65% : working hard
#define MOOD_TIRING_MAX   80.0f   // 65–80% : tiring; >= 80% exhausted

int clawdio_mood_group(void) {
    // Base mood from the absolute session level (the fatigue curve).
    int tier;
    if      (last_pct < MOOD_WAKING_MAX) tier = 0;  // waking
    else if (last_pct < MOOD_ZONE_MAX)   tier = 1;  // in the zone (peak)
    else if (last_pct < MOOD_BUSY_MAX)   tier = 2;  // busy
    else if (last_pct < MOOD_TIRING_MAX) tier = 3;  // tiring
    else                                 tier = 4;  // exhausted

    // Rate modulation — only while not yet exhausted (past 80% nothing perks
    // him up). Working hard right now energizes him one notch toward his happy
    // working zone; coasting in the sweet spot lets him relax a little.
    if (tier < 4) {
        int rate = usage_rate_group();  // 0 idle · 1 normal · 2 active · 3 heavy
        if (rate >= 2 && (tier == 2 || tier == 3)) tier = 1;  // grinding happily
        else if (rate == 0 && tier == 1)           tier = 0;  // sweet spot but idle
    }
    return tier;
}
