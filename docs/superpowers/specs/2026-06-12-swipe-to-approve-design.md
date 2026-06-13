# Approve-from-Device — Design

**Date:** 2026-06-12 (updated 2026-06-13: button input replaces the swipe gesture)
**Status:** Approved
**Scope:** Bidirectional approval flow — the Clawdmeter device approves Claude Code
tool-permission requests using its physical buttons. Builds on the session-event
notification feature (`2026-06-12-session-event-notifications-design.md`).

> **Design note:** This was originally specced as a Tinder-style swipe gesture.
> During hardware QA we pivoted to the physical buttons (the swipe drag was
> removed). The card UI and the daemon/hook plumbing are unchanged; only the
> input method differs. References below describe the button design.

## Goal

When a Claude Code session asks permission to run a tool, the user approves it
from the physical device with a button press. The other button (or timeout, or a
disconnected device) defers the request to the normal terminal prompt — it never
denies. This turns Clawdmeter from a read-only monitor into a two-way control.

## Intervention catalog

Claude Code interrupts for several reasons. This feature handles only the
button-actionable ones; the rest stay on the existing notify-only path
(banner + Clawdio).

| Intervention | Device behavior |
|---|---|
| Tool permission — action (Bash/Edit/Write) | **Buttons** |
| Tool permission — read (Read/Grep/…) | **Buttons** |
| Tool permission — MCP tool | **Buttons** |
| Plan approval (ExitPlanMode) | Notify only (approve in terminal) |
| Multiple choice (AskUserQuestion) | Notify only |
| Trust folder/workspace | Notify only |
| MCP auth / login | Notify only |
| Session finished / waiting input | Green "listo" banner (existing) |

All tool-permission prompts are button-actionable; everything else is notify-only.

## Architecture

```
Claude requests a tool
  → PreToolUse hook fires
  → device-ready flag fresh?
       NO  → return {permissionDecision:"ask"} immediately (normal terminal prompt)
       YES → write request {id,proj,tool,cmd} to a per-id file; poll for a decision (≤30s)
  → daemon sees the request file → sends it to the device over BLE (RX char)
  → device: Clawdio surprised + approval card (project + tool + command)
  → upper button (Continuar) → device notifies {id,d:"approve"} over BLE (TX char)
    lower button (Terminal)  → device notifies {id,d:"dismiss"}
  → device flashes a full-screen confirmation (~1.5s): green "Aprobado" / yellow "Terminal"
  → daemon writes the decision to the per-id file the hook is polling
  → hook returns:  approve → {permissionDecision:"allow"} · dismiss/timeout → {"ask"}
  → Claude runs the tool (allow) or shows the normal prompt (ask)
```

Two hooks coexist:
- **`PreToolUse`** (new, blocking) — handles the button flow for tool permissions.
- **`Notification`/`Stop`** (existing) — feed the banner/Clawdio for the notify-only
  interventions and the green "listo" banner.

When a button returns `allow`, the normal permission prompt never appears, so the
old amber banner does not fire for that request — the approval card replaces it.

## Components

### 1. PreToolUse hook — `daemon/clawdmeter-approve-hook.sh`

Reads the hook JSON on stdin. Behavior:

1. Read `device-ready` flag (`~/.config/claude-usage-monitor/device-ready`). If
   missing or older than 10 s → emit `ask` and exit (no wait). The daemon
   refreshes the flag every poll tick (~5 s) while connected, so 10 s gives a
   one-tick margin against false negatives.
2. Generate a unique `id` (`session_id` + epoch-ns).
3. Write the request to `~/.config/claude-usage-monitor/approve/<id>.req`:
   `{"id","sid","proj":<basename cwd>,"tool":<tool_name>,"cmd":<short tool_input>}`
   (cmd derived from `tool_input.command`, or the file path for Edit/Write, truncated
   to ~80 chars).
4. Poll `~/.config/claude-usage-monitor/approve/<id>.res` every 0.3 s up to 30 s.
   - `{"d":"approve"}` → emit `{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"allow"}}`
   - `{"d":"dismiss"}` or timeout → emit `permissionDecision:"ask"`
5. Always remove the req/res files on exit. Always exit 0.

Registered in `~/.claude/settings.json` for `PreToolUse` with an explicit
`"timeout": 35` (> the 30 s device window; avoids the known no-timeout PreToolUse
silent-close bug).

### 2. Daemon — `daemon/claude_usage_daemon.py`

- **`device-ready` flag:** while a BLE session is connected, touch the flag every
  poll tick (~5 s); remove it on disconnect/shutdown. The hook treats it as fresh
  for 10 s.
- **TX subscription:** subscribe to the TX characteristic (`...0003`) and parse
  device decisions `{"id","d":"approve"|"dismiss"}`.
- **`ApprovalBroker`:** watches the `approve/` directory for `<id>.req` files,
  maintains a FIFO queue of pending requests, and sends the head request to the
  device over RX as `{"ev":"ask","id","proj","tool","cmd"}`. On a TX decision (or
  timeout), writes `<id>.res` and advances the queue. One request is shown at a
  time; `n`/position is sent so the device can render `1 de 3`.
- A request whose `.req` file disappears (hook gave up) is dropped from the queue.

### 3. Firmware

**RX routing:** an `{"ev":"ask",…}` payload opens the approval card; existing event
and usage payloads keep their paths.

**`ble.cpp`:** `ble_send_decision(id, decision)` notifies the TX characteristic
with `{"id","d"}` when a button is pressed.

**`data.h`:** new struct
```c
struct ApprovalRequest {
    char id[40];
    char proj[24];
    char tool[16];
    char cmd[64];
    uint8_t pos;    // 1-based position in the queue
    uint8_t total;  // queue length
    bool fresh;
};
```

**`ui.cpp`:** a near-full-screen square card on `lv_layer_top()`, sized for the
480×480 2.16" panel with large text. Content (project / tool / command) sits on
the left; the two action labels live on the **right edge** next to the physical
buttons — upper-third **Continuar** (green, approve) and lower-third **Terminal**
(gray, defer). A `1 de N` queue indicator shows in the corner. Clawdio pins to
`expression surprise` while a card is showing.

- **Input = physical buttons** (not touch). `ui_approval_active()` lets the main
  loop route the two outer buttons to `ui_approval_primary()` / `ui_approval_secondary()`
  instead of their normal HID actions; each maps to approve/dismiss via the frozen
  rotation quadrant (`primary_is_upper()`, tuned on hardware). The middle (PWR)
  button is untouched.
- **Rotation freeze:** on show, the card captures the current quadrant and calls
  `imu_hal_lock_rotation(true)` so the button↔label mapping can't shift under the
  user; unlocked on finish.
- **Confirmation flash:** after a decision, a full-screen overlay flashes ~1.5s —
  green `✓ Aprobado` or yellow `Terminal` — then clears (handled in `ui_approval_tick`).
- A ~30 s on-device timer auto-dismisses to `dismiss` (Terminal).

`imu_hal_lock_rotation(bool)` is added to the IMU HAL: a real freeze on the
rotation-capable boards (216, 216_c6) and a no-op on the rest.

## Error handling

- Device disconnects mid-wait → daemon clears `device-ready`; hook times out → `ask`.
- Daemon down → stale flag → hook returns `ask` instantly.
- Orphan decision (arrives after timeout) → no waiting `.res` consumer; daemon ignores.
- Over-long `cmd`/`proj` → daemon truncates; firmware clips with `LV_LABEL_LONG_DOT`.
- Card appears over any screen via `lv_layer_top()`.
- Rotation is frozen while the card shows, so the button mapping is stable; the
  per-quadrant `primary_is_upper()` table is tuned on hardware (validated on the C6).

## Testing

- **Hook:** bash test feeding `PreToolUse` JSON, simulating `approve`/`dismiss`/no
  response (timeout) and a stale/fresh `device-ready`; assert `allow` vs `ask` output.
- **Daemon:** unit tests for `ApprovalBroker` (request→decision routing, FIFO queue,
  `id` correlation, orphan drop) and TX decision parsing.
- **Firmware:** physical QA on the C6 (no screenshot support) — card over splash,
  upper/lower button → approve/Terminal, confirmation flash, timeout auto-dismiss,
  button mapping correct under the frozen quadrant.

## Out of scope

- Denying from the device (the Terminal button only defers to the terminal; there is no deny).
- Selecting among options for AskUserQuestion (notify-only).
- Windows/Linux daemon ports (macOS first, same file/BLE contract).
- Approving plan/trust/MCP-auth interventions from the device (notify-only).
