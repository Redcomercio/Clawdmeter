# Session Event Notifications — Design

**Date:** 2026-06-12
**Status:** Approved
**Scope:** macOS daemon + firmware (all boards). Windows/Linux daemons port later using the same event-file interface.

## Goal

The Clawdmeter device alerts the user in real time when:

1. A Claude Code session is **waiting for approval** (tool permission prompt).
2. A Claude Code session has **finished responding** (idle, waiting for the user).

Multiple concurrent sessions are supported; the banner identifies which project needs attention.

## Architecture

```
Claude Code (hooks) → events.jsonl → macOS daemon (Python) → BLE RX char → ESP32 (banner overlay)
```

- **Real-time source:** Claude Code native hooks (`Notification`, `Stop`, `PostToolUse`). No polling, no transcript scraping, no extra API calls.
- **Transport on host:** append-only JSONL file at `~/.config/claude-usage-monitor/events.jsonl`. A FIFO is explicitly rejected: FIFO writes block when no reader exists, which would hang Claude Code hooks whenever the daemon is stopped. Appending to a regular file never blocks.
- **Transport to device:** the existing BLE GATT RX characteristic (`...0002`). Event payloads are sent immediately, independent of the 60 s usage poll.

## Components

### 1. Hook script — `daemon/clawdmeter-hook.sh`

One POSIX shell script, registered for three hook events. Reads the hook JSON
from stdin, appends one normalized line to the event file, exits 0 always
(a failing hook must never disturb a Claude session).

Normalized event line:

```json
{"ts": 1765574400, "sid": "<session_id>", "proj": "<basename of cwd>", "ev": "approval" | "done" | "activity"}
```

Mapping:

| Claude Code hook | Condition | `ev` |
|---|---|---|
| `Notification` | permission request notification | `approval` |
| `Stop` | always | `done` |
| `PostToolUse` | always | `activity` |

Registration lives in the user's `~/.claude/settings.json` (documented in
README; an install snippet is provided, not auto-edited by the installer).

### 2. macOS daemon — `daemon/claude_usage_daemon.py`

New asyncio task `watch_events()`:

- Polls the event file's size every 1 s (cheap stat; no extra dependency).
- Reads only newly appended lines; tolerates malformed lines (skip + log).
- Truncates the file on daemon startup (stale events are meaningless).

Per-session state machine (in memory, keyed by `sid`):

| Event | Transition |
|---|---|
| `approval` | session → `pending_approval`; push banner update |
| `activity` | if session was `pending_approval` → clear it (user approved; tools are running again); push update |
| `done` | clear `pending_approval` if set; session → `done`; push banner update |

Sessions are evicted from memory 10 minutes after their last event.

BLE event payload (separate small JSON write, same RX characteristic):

```json
{"ev": "approval", "proj": "Clawdmeter", "n": 2}
{"ev": "done",     "proj": "backend-parkingapp"}
{"ev": "clear"}
```

- `n` = total sessions currently pending approval (sent when ≥ 1).
- `proj` = project of the most recent event.
- `clear` = no sessions pending approval anymore (dismisses the amber banner).

### 3. Firmware

**`ble.cpp`** — extend the RX write handler: a payload containing an `"ev"`
key is routed to the new event handler; payloads with the existing usage keys
keep their current path. Both payload shapes coexist; neither breaks the other.

**`data.h`** — new struct:

```c
struct SessionEvent {
    char type[12];   // "approval" | "done" | "clear"
    char proj[24];   // project name, truncated
    uint8_t count;   // pending approvals
    bool fresh;      // set by BLE, consumed by UI
};
```

**`ui.cpp`** — banner overlay on `lv_layer_top()` so it floats above whichever
screen is active, on every board (sized via `board_caps()`, respecting the
20 px rounded-corner margin):

- **Amber banner** — `⏸ <proj> · aprobación`, plus `<n> pendientes` when n > 1.
  Persists until the daemon sends `clear` (or a `done` for the last pending
  session). Tap hides it locally (it reappears on the next `approval` event).
- **Green banner** — `✓ <proj> · listo`. Auto-dismisses after 30 s or on tap.
- Banner uses existing Styrene fonts; no new assets required.

No board-specific code: the banner is shared UI driven by `board_caps()`,
per project rule 10 (no `#ifdef BOARD_*` in shared code).

## Error handling

- Hook script always exits 0; failures are silent to Claude Code.
- Daemon skips malformed JSONL lines and logs them.
- BLE disconnected when an event fires: daemon retains banner state and
  re-sends the current aggregate state on reconnect (same mechanism as the
  usage payload refresh).
- Firmware ignores unknown `ev` values.

## Testing

- **Hook script:** bash test feeding sample hook JSON via stdin, asserting the
  appended line (lives near `daemon/tests/`).
- **Daemon:** Python unit tests for the line parser and the session state
  machine (extend `daemon/tests/`).
- **Firmware:** visual QA via `./screenshot.sh` — banner over splash and over
  the usage screen, on at least the 2.16 board; compile-check all three envs.

## Out of scope

- Windows and Linux daemon ports (JP ports Windows using `events.jsonl` as the
  contract).
- Sound/vibration (no buzzer on current boards).
- Per-session list screen (banner-only, per design decision).
