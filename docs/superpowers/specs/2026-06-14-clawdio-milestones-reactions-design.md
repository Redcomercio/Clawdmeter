# Clawdio Milestones & Reactions — Design

**Date:** 2026-06-14
**Status:** Approved
**Scope:** Give Clawdio (the splash creature) Tamagotchi-style personality —
**reactions** to events and **milestones/achievements** — but with NO dependency,
NO death, NO penalties. Reward, never obligation. Builds on the session-event
notification + approve-from-device features.

## Principle

A classic Tamagotchi uses *needs that punish you*. Clawdio uses *reactions and
rewards*: he reacts to your work and celebrates milestones, but never decays,
dies, or demands attention. Achievements linger and can be re-played on demand —
a positive-feedback loop, not a chore.

## Architecture

- **Daemon = brain.** It has the data (usage %, session events), runs continuously,
  and persists state in `~/.config/claude-usage-monitor/clawdio-state.json`
  (streak, counters, which milestones are unlocked). It computes milestones and
  tells the device what to do.
- **Device = renderer.** It plays animations, shows toasts/badges, and (Phase B)
  renders a trophy gallery from a list the daemon sends. The device persists
  nothing across reboots; it re-receives state from the daemon on connect.
- **New BLE payloads** (daemon → device, over the existing RX channel; the device
  already routes `{"ev":...}`):
  - `{"ev":"milestone","id":"streak7","label":"🔥 7 días seguidos","anim":"dance bounce"}`
    — fire once when unlocked.
  - `{"ev":"trophies","list":[{"id","label"},...]}` — sent on connect to populate
    the gallery and the "last milestone" used by the badge replay (Phase B; in
    Phase A the device just remembers the most recent `milestone`).
  - `danger` flag on the existing `ask` payload: `{"ev":"ask",...,"danger":true}`
    — Clawdio reacts scared instead of merely surprised.

## Milestone catalog

Each milestone unlocks once; the daemon marks it in its state file and sends the
event. Thresholds live in a table in the daemon (easy to tune).

- **🔥 Day streaks** (an active day = ≥1 event that local day): 3, 7, 14, 30.
  Persist `last_active_date`, `streak_count`, `best_streak`.
- **✅ Completed tasks** (`done` events): per-day 10 ("Día productivo"), 25
  ("Máquina"); lifetime 100 ("Centenario"), 500, 1000. Per-day counter resets on
  date change; lifetime persists.
- **📊 Usage** (from usage %): "En su salsa" (first time reaching the in-the-zone
  tier ~30%), "Maratón" (a day usage hit ≥80% and survived — wink at "no muere").
- **💾 Commits** (extra plumbing): the events hook detects `git commit` in a Bash
  `tool_input.command` and emits a `commit` event; daemon counts. Milestones at
  1 ("Primer commit"), 10, 50, 100.

## Reactions to events

Momentary micro-animations: a short timed pin over Clawdio's base mood that
auto-reverts. New mechanism: `splash_pin_anim_for(name, ms)` (timed pin) on top of
the existing indefinite `splash_pin_anim`.

| Trigger | Reaction | Animation |
|---|---|---|
| `done` (task finished) | Festejo | `dance bounce` (~2.5 s) → back to mood |
| `ask` with `danger:true` | Susto | `expression surprise` (while the card is up) |
| Approve pressed (Continuar) | Contento | `expression wink` (~1.5 s, before the green flash) |
| Ongoing `activity` | Enfocado | reinforces `work coding`/`think` via the mood (exists) |

No dedicated "scared" animation exists; `expression surprise` stands in until a
scared sprite is scraped from claudepix. The approval card's susto takes priority
while it is shown.

## Celebration & persistent achievement (positive feedback loop)

When a `milestone` event arrives:

1. **Short festive burst (~5 s):** Clawdio plays the festive animation + a toast
   (trophy icon + label) on `lv_layer_top()`.
2. **Proud sustained state (30–60 min):** after the burst, Clawdio holds a
   content/proud expression that **overrides the normal mood** for a random
   30–60 min, and a **mini trophy badge** sits in a corner showing the last
   milestone.
3. **Tap the badge → replays the festive burst** (`ui_celebrate(last_milestone)`).
   Re-play on demand so you never miss a streak you weren't watching.
4. After 30–60 min (or when a new milestone arrives) Clawdio returns to the normal
   mood; the badge keeps showing the last achievement until it decays or is
   dismissed.

The badge is a small tappable LVGL object in a corner (like the banner), with its
own 30–60 min timer; the tap calls `ui_celebrate(last_milestone)` which reuses the
burst. `last_milestone` is remembered on the device from the most recent
`milestone` payload (Phase A) or the `trophies` list head (Phase B).

## Phasing

- **Phase A (fun core):** reactions + milestone computation/persistence in the
  daemon + festive burst + toast + proud state + mini-badge + tap-to-replay +
  the `commit`/`danger` plumbing. No new screen.
- **Phase B:** the **trophy gallery** — a new `SCREEN_TROPHIES` listing all unlocked
  achievements (from the `trophies` payload), plus screen navigation. Navigation is
  a Phase B design point: today PWR cycles splash animations / usage brightness;
  adding a third screen needs rework (e.g., PWR cycles splash→usage→trophies, with
  animation/brightness cycling moved to a long-press). Does not block Phase A.

## State file (`clawdio-state.json`)

```json
{
  "last_active_date": "2026-06-14",
  "streak_count": 7,
  "best_streak": 12,
  "tasks_today": 18,
  "tasks_today_date": "2026-06-14",
  "tasks_total": 432,
  "commits_total": 51,
  "usage_in_zone_seen": true,
  "usage_marathon_dates": ["2026-06-10"],
  "unlocked": ["streak3","streak7","tasks100","commit1","commit10","in_zone"]
}
```

Computed/updated as the daemon tails events and polls usage; written atomically.

## Error handling

- Corrupt/missing state file → daemon starts fresh (no milestones lost beyond what
  the file held); never crashes the daemon.
- Device reboots → milestones/badge come back from the daemon's `trophies` payload
  on reconnect; in-flight burst state is not persisted on the device (acceptable).
- A milestone already in `unlocked` never re-fires (no spam on daemon restart).
- Date rollover handled by comparing stored date strings; clock changes only affect
  streak/daily counters, never crash.
- Unknown `ev`/`anim` values are ignored by the firmware.

## Testing

- **Daemon:** unit tests for the milestone engine — streak increment/reset across
  date changes, daily counter reset, lifetime thresholds, usage milestones, commit
  counting, and "unlock fires once". Pure logic over injected events + dates.
- **Hook:** test that a Bash `git commit …` command emits a `commit` event and
  non-commit commands do not.
- **Firmware:** physical QA on the C6 — festive burst + toast, proud state holds,
  mini-badge appears, tap-to-replay re-runs the burst, danger→scared on a flagged
  approval card, done→festejo.

## Out of scope

- Any decay/health/hunger/death mechanic (explicitly excluded).
- Penalties for inactivity.
- Phase B navigation specifics (its own design pass).
- Windows/Linux daemon ports (macOS first, same state-file/BLE contract).
