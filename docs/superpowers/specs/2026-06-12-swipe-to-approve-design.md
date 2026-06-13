# Swipe-to-Approve — Design

**Date:** 2026-06-12
**Status:** Approved
**Scope:** Bidirectional approval flow — the Clawdmeter device approves Claude Code
tool-permission requests via a Tinder-style swipe. Builds on the session-event
notification feature (`2026-06-12-session-event-notifications-design.md`).

## Goal

When a Claude Code session asks permission to run a tool, the user can approve it
from the physical device with a right swipe. A left swipe (or timeout, or a
disconnected device) defers the request to the normal terminal prompt — it never
denies. This turns Clawdmeter from a read-only monitor into a two-way control.

## Intervention catalog

Claude Code interrupts for several reasons. This feature handles only the
swipe-able ones; the rest stay on the existing notify-only path (banner + Clawdio).

| Intervention | Device behavior |
|---|---|
| Tool permission — action (Bash/Edit/Write) | **Swipe** |
| Tool permission — read (Read/Grep/…) | **Swipe** |
| Tool permission — MCP tool | **Swipe** |
| Plan approval (ExitPlanMode) | Notify only (approve in terminal) |
| Multiple choice (AskUserQuestion) | Notify only |
| Trust folder/workspace | Notify only |
| MCP auth / login | Notify only |
| Session finished / waiting input | Green "listo" banner (existing) |

All tool-permission prompts are swipe-able; everything else is notify-only.

## Architecture

```
Claude requests a tool
  → PreToolUse hook fires
  → device-ready flag fresh?
       NO  → return {permissionDecision:"ask"} immediately (normal terminal prompt)
       YES → write request {id,proj,tool,cmd} to a per-id file; poll for a decision (≤30s)
  → daemon sees the request file → sends it to the device over BLE (RX char)
  → device: Clawdio surprised + swipe card (project + tool + command)
  → swipe RIGHT  → device notifies {id,d:"approve"} over BLE (TX char)
    swipe LEFT   → device notifies {id,d:"dismiss"}
  → daemon writes the decision to the per-id file the hook is polling
  → hook returns:  approve → {permissionDecision:"allow"} · dismiss/timeout → {"ask"}
  → Claude runs the tool (allow) or shows the normal prompt (ask)
```

Two hooks coexist:
- **`PreToolUse`** (new, blocking) — handles the swipe flow for tool permissions.
- **`Notification`/`Stop`** (existing) — feed the banner/Clawdio for the notify-only
  interventions and the green "listo" banner.

When a swipe returns `allow`, the normal permission prompt never appears, so the
old amber banner does not fire for that request — the swipe card replaces it.

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

**RX routing:** an `{"ev":"ask",…}` payload opens the swipe card; existing event
and usage payloads keep their paths.

**`ble.cpp`:** add a helper to notify the TX characteristic with a decision
payload `{"id","d"}` when the user swipes.

**`data.h`:** new struct
```c
struct ApprovalRequest {
    char id[16];
    char proj[24];
    char tool[16];
    char cmd[64];
    uint8_t pos;    // 1-based position in the queue
    uint8_t total;  // queue length
    bool fresh;
};
```

**`ui.cpp`:** a swipe card on `lv_layer_top()` (overlays any screen, rotates with
the display). Shows project, tool, command, swipe hints (`↩ terminal` / `aprobar ✓`),
and a `● ○ ○  1 de 3` queue indicator. Drag handled via `LV_EVENT_PRESSING`
(translate + tint: green right, dim left) and `LV_EVENT_RELEASED` (commit past ~40%
of width, else spring back). Commit animates the card off-screen and calls a
firmware callback that sends the BLE decision. A ~30 s on-device timer auto-dismisses
(= dismiss). Clawdio pins to `expression surprise` while a card is showing.

## Error handling

- Device disconnects mid-wait → daemon clears `device-ready`; hook times out → `ask`.
- Daemon down → stale flag → hook returns `ask` instantly.
- Orphan decision (arrives after timeout) → no waiting `.res` consumer; daemon ignores.
- Over-long `cmd` → daemon truncates; firmware clips with `LV_LABEL_LONG_DOT`.
- Card appears over any screen via `lv_layer_top()`.
- Rotated orientation: swipe direction mapping verified in hardware QA; possible
  refinement noted, not a blocker.

## Testing

- **Hook:** bash test feeding `PreToolUse` JSON, simulating `approve`/`dismiss`/no
  response (timeout) and a stale/fresh `device-ready`; assert `allow` vs `ask` output.
- **Daemon:** unit tests for `ApprovalBroker` (request→decision routing, FIFO queue,
  `id` correlation, orphan drop) and TX decision parsing.
- **Firmware:** physical QA on the C6 (no screenshot support) — card over splash,
  drag right/left, spring-back, queue advance, timeout auto-dismiss.

## Out of scope

- Denying from the device (left swipe only dismisses to terminal; there is no deny).
- Selecting among options for AskUserQuestion (notify-only).
- Windows/Linux daemon ports (macOS first, same file/BLE contract).
- Approving plan/trust/MCP-auth interventions from the device (notify-only).
