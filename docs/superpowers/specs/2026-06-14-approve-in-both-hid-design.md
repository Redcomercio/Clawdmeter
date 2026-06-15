# Approve in Both (terminal + device via HID) — Design

**Date:** 2026-06-14
**Status:** Approved
**Scope:** When a Claude Code session asks tool permission, the prompt appears in
the **terminal** (always) AND a mirror card appears on the device. Pressing the
device's approve button **types the answer into the focused terminal over BLE-HID**.
Supersedes the blocking-decision approach in
`2026-06-12-swipe-to-approve-design.md` (the device no longer decides for the hook;
it acts as a remote keyboard).

## Motivation

The previous PreToolUse hook *blocked* waiting for a device decision and *replaced*
the terminal prompt. That (a) hid the question from the terminal and (b) — with a
broad matcher — intercepted every tool incl. `AskUserQuestion`. This redesign makes
the hook **non-blocking**: the terminal prompt always shows, the device mirrors it,
and the device's button just *types* the answer. Simpler, and the terminal is never
blocked.

## Key research finding (macOS)

Claude Code's permission prompt is keyboard-controllable and stable on macOS:
- **`1`** = approve once (immediate, no Enter).
- **`3`** = reject (immediate).
- `2` ("allow for session") is broken — never send it.
- The keys go to the **OS active window**, so the terminal must be focused.
~95% reliable; acceptable for an optional convenience.

## Architecture (non-blocking)

```
Claude requests a tool
  → PreToolUse hook (scoped to action tools; only if device-ready is fresh)
       · writes the request {id,proj,tool,cmd} for the daemon
       · returns {permissionDecision:"ask"} IMMEDIATELY  → the normal terminal prompt shows
       · exits (no polling, no decision file)
  → daemon sees the .req → sends a mirror card to the device (RX): {"ev":"ask",...}
  → device shows the card (Continuar / Terminal)
       Continuar → firmware types HID '1' to the focused terminal (approve once)
                   + notifies TX {"id","d":"approve"} so the daemon clears the card
                   + shows the green "Aprobado" flash (keystroke feedback)
       Terminal  → firmware notifies TX {"id","d":"dismiss"} + hides the card
                   (you answer in the terminal yourself; no keystroke sent)
  → the card also clears on the session's activity/done event or after a 60 s timeout
```

The hook no longer waits for or consumes a decision; the decision happens in the
terminal (typed by you, or by the device via HID). The device→daemon TX message is
now only used to **clear the card**, not to drive the hook.

## Components

### 1. Hook — `daemon/clawdmeter-approve-hook.sh` (simplified)

- Keep the action-tool guard (`Bash|Edit|Write|MultiEdit|NotebookEdit|mcp__*` → engage;
  everything else → `ask` instantly) and the `device-ready` freshness gate.
- When engaged: write `<id>.req` ({id,sid,proj,tool,cmd}) and emit `ask` **immediately**.
  No `<id>.res` polling, no `timeout` loop, no trap-wait. Exit 0.
- The `"timeout"` in the settings.json registration is no longer needed for blocking,
  but a small value is fine; the hook returns instantly regardless.

### 2. Daemon — `ApprovalBroker` + `claude_usage_daemon.py`

- Broker still scans the approve dir and sends the head `{"ev":"ask",...}` (incl. the
  `danger` flag) to the device.
- **Card lifecycle (new):** the broker clears the head request (delete `.req`, advance
  queue, tell device `{"ev":"clear-ask"}`) when ANY of:
  - a TX decision arrives for it (`approve`/`dismiss`), or
  - the session fires `activity`/`done` (the prompt was resolved in the terminal), or
  - 60 s elapse (stale).
- No `.res` file is written anymore (the hook doesn't read one).

### 3. Firmware

- **Approval card** (existing): on a button, instead of waiting for a daemon round-trip:
  - **Continuar** → `ble_keyboard_press('1', 0)` then release (HID types `1`), send TX
    `{"id","d":"approve"}`, show the green **"Aprobado"** flash, hide the card.
  - **Terminal** → send TX `{"id","d":"dismiss"}`, hide the card (no keystroke).
- HID `'1'` = USB keycode `0x1E` (modifier 0). Reuse the existing `ble_keyboard_press`/
  `ble_keyboard_release`.
- Keep the rotation-freeze and the green confirmation flash (now framed as *keystroke
  sent* feedback). The yellow "Terminal" flash is optional; keep it as dismiss feedback.
- New: handle a `{"ev":"clear-ask"}` payload from the daemon → hide the card if showing
  (so a prompt resolved in the terminal also clears the device card).

## Limitations (documented, accepted)

- **Focus:** the HID keystroke lands in the OS active window. If the terminal isn't
  focused, `1` goes elsewhere. The card can hint "enfoca el terminal".
- **Keystroke racing:** typing in the terminal exactly when the prompt appears can
  collide with the HID key (a known Claude Code quirk).
- **macOS only / ~95%:** `2` is broken (unused); Windows prompt bugs are out of scope.

## Error handling

- Device disconnected → hook's `device-ready` gate fails → `ask` only (terminal prompt,
  no card). Normal flow, never blocked.
- Daemon down → no card; terminal prompt still shows (hook returns ask).
- Stale card (prompt resolved in terminal) → cleared by activity/done or the 60 s timeout.
- HID send while disconnected → `ble_keyboard_press` already guards on connection.

## Testing

- **Hook:** test it returns `ask` immediately (no blocking) and writes a `.req` only for
  action tools with a fresh `device-ready`; returns `ask` with no `.req` otherwise.
- **Daemon:** broker clears the card on a TX decision, on an activity/done event, and on
  the 60 s timeout; no `.res` is written.
- **Firmware:** physical QA on the C6 with a real Claude Code permission prompt in a
  focused terminal — Continuar types `1` and the tool runs; the green flash shows; the
  card clears; Terminal hides the card and you answer in the terminal.

## Out of scope

- Rejecting from the device (Terminal only defers; could add HID `3` later).
- "Allow for session" (`2` is broken).
- Windows/Linux HID specifics.
