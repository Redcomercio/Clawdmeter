#pragma once

// Tracks short-term rate of change in session_pct (%/min) so the UI can react
// to *how heavily* Claude is being used right now, not just the current bucket
// level. Returns one of 4 group indices for the splash to pick animations from.

// Feed in the latest session percentage every time fresh BLE data arrives.
void usage_rate_sample(float session_pct);

// 0 = idle, 1 = normal, 2 = active, 3 = heavy.
// Defaults to 0 when the buffer doesn't have enough samples yet.
int usage_rate_group(void);

// Clawdio's mood group, combining absolute usage level with work rate.
// The current session % sets a fatigue baseline (he loves working — peak mood
// around 30% — but past 80% he's exhausted); a hot work rate energizes him one
// notch (unless already exhausted), while coasting in the sweet spot relaxes
// him. Returns 0..4 — see GROUP_NAMES in splash.cpp:
//   0 waking · 1 in-the-zone (peak) · 2 busy · 3 tiring · 4 exhausted
int clawdio_mood_group(void);
