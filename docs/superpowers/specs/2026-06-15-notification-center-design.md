# Notification Center + Touch-Rotation Fix — Design

**Date:** 2026-06-15
**Status:** Approved
**Scope:** A touch notification center on the device (iPhone-style slide-down) that
lists everything currently stuck/active across the user's 3-4 Claude sessions and
lets them delete items individually. Requires fixing touch input under display
rotation first. Builds on approve-from-device / approve-in-both.

## Why

The user runs 3-4 Claude sessions at once, so multiple approval prompts can be
pending and cards can get stuck on the device (e.g. a terminal was closed). They
want one view to see all pending/active notifications and clear stuck ones. The
device's touch is currently misaligned when the display is rotated, which must be
fixed for a touch-driven view.

## Decomposition (build in order)

**Part A — Touch-rotation fix (prerequisite, self-contained).**
The display rotates by CPU pixel-remap at flush (`rotate_strip`), but `my_touch_cb`
uses raw panel coordinates, so touch is misaligned when rotated. Fix: remap the
touch point by the current quadrant before handing it to LVGL.

**Part B — Notification center view.**
A new touch view listing all stuck/active items, with per-row delete.

## Part A — Touch-rotation fix

In `firmware/src/main.cpp` `my_touch_cb`, after `touch_hal_read(&x, &y, &pressed)`,
remap `(x, y)` using `q = imu_hal_rotation_quadrant()` and `S = board_caps().width`
(rotation-enabled panels are square 480×480; the AMOLED-1.8 has rotation disabled
so `q` is always 0 → identity, no effect):

```
q0: (x, y)
q1: (y, S-1-x)
q2: (S-1-x, S-1-y)
q3: (S-1-y, x)
```

The exact axis/sign per quadrant is **tuned on hardware** in QA (tap a known target
at each rotation and verify the hit), mirroring how `BOARD_ROTATION_OFFSET` was
tuned. This fix also repairs the milestone badge tap and the banner tap when rotated.

## Part B — Notification center

### Data (daemon → device)

- The daemon sends the full pending list whenever the approval queue changes (and on
  connect): `{"ev":"notif","items":[{"id","proj","tool"}, ...]}` (cmd omitted or
  short to keep the payload small; the row shows project + tool).
- This is in addition to the existing single `{"ev":"ask",...}` card flow — the card
  remains for actively answering one prompt with the buttons; the center is for
  overview + clearing.
- `ApprovalBroker` gains a `list()` method returning the current queue as
  `[{"id","proj","tool"}, ...]`; the daemon sends it on every queue change (after
  scan/decide/clear_current) and on connect.

### Delete (device → daemon)

- Tapping a row's **✕** sends `{"id":<id>,"d":"clear"}` over TX. The broker drops
  that `.req` (reuse `decide(id, "clear")` / a `clear` that unlinks + advances) with
  **no keystroke** — it just removes it from the device and the queue.
- The daemon then re-sends the updated `{"ev":"notif","items":[...]}`.

### View (firmware)

- New screen state `SCREEN_NOTIF` (or a full-screen overlay on `lv_layer_top()`).
- **Open:** a downward swipe from the top edge — handled via an LVGL gesture
  (`LV_EVENT_GESTURE`, `lv_indev_get_gesture_dir() == LV_DIR_BOTTOM`) so it's distinct
  from a tap (which toggles splash/usage). Works once Part A lands.
- **Layout:** dark full-screen panel, title "Notificaciones", a scrollable list of
  rows. Each row: `proj · tool` on the left, an **✕** button on the right.
- **Per-row delete:** tap ✕ → send the clear payload → daemon drops it → list
  re-renders. The row disappears.
- **Close:** a "Cerrar" button at the bottom (and/or an upward swipe).
- **Empty state:** "Sin notificaciones".
- The device stores the last `notif` list and rebuilds rows on each update.

## Error handling

- Touch fix on a non-rotating board (AMOLED-1.8): `q` is always 0 → identity, no risk.
- Device reconnect → daemon re-sends `notif` on connect, so the center repopulates.
- Delete of an `id` no longer in the queue (already cleared) → broker no-ops.
- Empty queue → center shows the empty state; closing returns to the previous screen.
- A new prompt arriving while the center is open → daemon re-sends the list; the view
  updates live.

## Testing

- **Touch fix:** physical QA per quadrant — tap a known on-screen target at 0/90/180/
  270 and confirm the correct element is hit (badge/banner now work rotated too).
- **Daemon:** unit test `ApprovalBroker.list()` (order, fields) and that a `clear`
  decision drops the item without writing a keystroke/`.res`.
- **Firmware:** physical QA — slide down opens the center, rows reflect the daemon's
  pending list, ✕ deletes a row (and the daemon drops it), Cerrar/slide-up closes,
  empty state shows; all verified while the display is rotated.

## Out of scope

- Answering prompts from the center (typing Yes/No) — that stays on the single card.
  The center only clears/removes (no keystroke).
- A historical feed of past events (done/milestones) — the center shows only
  currently stuck/active items.
- Windows/Linux daemon ports.
