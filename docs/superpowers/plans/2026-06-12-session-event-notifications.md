# Session Event Notifications Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clawdmeter shows a real-time banner when a Claude Code session is waiting for approval, and a "done" banner when a session finishes.

**Architecture:** Claude Code native hooks append normalized JSON lines to `~/.config/claude-usage-monitor/events.jsonl`. The macOS daemon tails the file, maintains per-session state, and pushes small event payloads over the existing BLE RX characteristic. The firmware parses these payloads and renders an LVGL banner on `lv_layer_top()` so it floats over any screen.

**Tech Stack:** POSIX shell (hook), Python 3 + asyncio + bleak (daemon), C++ / Arduino / LVGL 9 / ArduinoJson (firmware).

**Spec:** `docs/superpowers/specs/2026-06-12-session-event-notifications-design.md`

---

## File Structure

- `daemon/clawdmeter-hook.sh` (create) — one shell script invoked by all three hooks; reads hook JSON on stdin, appends one normalized line to the event file.
- `daemon/claude_usage_daemon.py` (modify) — add an `EventTracker` class (state machine) and an asyncio `watch_events()` task; wire event payloads into the existing `Session.write_payload`.
- `daemon/tests/test_event_tracker.py` (create) — unit tests for `EventTracker` and the JSONL line parser.
- `daemon/tests/test_hook_script.py` (create) — test the shell script's stdin→line mapping.
- `firmware/src/data.h` (modify) — add `SessionEvent` struct.
- `firmware/src/main.cpp` (modify) — route event payloads (key `"ev"`) to a new parser and forward to UI; existing usage path unchanged.
- `firmware/src/ui.h` (modify) — declare `ui_show_event(const SessionEvent*)` and `ui_tick_anim` banner timeout.
- `firmware/src/ui.cpp` (modify) — banner overlay on `lv_layer_top()`, tap-to-dismiss, 30 s auto-dismiss for "done".
- `README.md` (modify) — document hook installation snippet.

---

## Task 1: Hook script

**Files:**
- Create: `daemon/clawdmeter-hook.sh`
- Test: `daemon/tests/test_hook_script.py`

- [ ] **Step 1: Write the failing test**

```python
# daemon/tests/test_hook_script.py
import json
import subprocess
import tempfile
from pathlib import Path

HOOK = Path(__file__).resolve().parents[1] / "clawdmeter-hook.sh"


def run_hook(event_name, stdin_obj, event_file):
    return subprocess.run(
        ["bash", str(HOOK), event_name],
        input=json.dumps(stdin_obj),
        capture_output=True,
        text=True,
        env={"CLAWDMETER_EVENT_FILE": str(event_file), "PATH": "/usr/bin:/bin"},
    )


def read_lines(event_file):
    return [json.loads(l) for l in Path(event_file).read_text().splitlines() if l.strip()]


def test_notification_maps_to_approval(tmp_path):
    ef = tmp_path / "events.jsonl"
    run_hook("Notification", {"session_id": "abc", "cwd": "/Users/me/dev/Clawdmeter"}, ef)
    lines = read_lines(ef)
    assert len(lines) == 1
    assert lines[0]["ev"] == "approval"
    assert lines[0]["sid"] == "abc"
    assert lines[0]["proj"] == "Clawdmeter"
    assert isinstance(lines[0]["ts"], int)


def test_stop_maps_to_done(tmp_path):
    ef = tmp_path / "events.jsonl"
    run_hook("Stop", {"session_id": "xyz", "cwd": "/tmp/proj"}, ef)
    assert read_lines(ef)[0]["ev"] == "done"


def test_posttooluse_maps_to_activity(tmp_path):
    ef = tmp_path / "events.jsonl"
    run_hook("PostToolUse", {"session_id": "xyz", "cwd": "/tmp/proj"}, ef)
    assert read_lines(ef)[0]["ev"] == "activity"


def test_exit_zero_even_with_garbage_stdin(tmp_path):
    ef = tmp_path / "events.jsonl"
    r = subprocess.run(
        ["bash", str(HOOK), "Stop"],
        input="not json",
        capture_output=True, text=True,
        env={"CLAWDMETER_EVENT_FILE": str(ef), "PATH": "/usr/bin:/bin"},
    )
    assert r.returncode == 0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd daemon && python3 -m pytest tests/test_hook_script.py -v`
Expected: FAIL — script does not exist.

- [ ] **Step 3: Write the hook script**

```bash
# daemon/clawdmeter-hook.sh
#!/usr/bin/env bash
# Clawdmeter Claude Code hook. Invoked with the hook event name as $1.
# Reads the hook JSON payload on stdin and appends one normalized event
# line to the event file. ALWAYS exits 0 — a failing hook must never
# disturb a Claude Code session.
#
# Install: register this for Notification, Stop and PostToolUse in
# ~/.claude/settings.json (see README).

event_name="$1"
event_file="${CLAWDMETER_EVENT_FILE:-$HOME/.config/claude-usage-monitor/events.jsonl}"

case "$event_name" in
    Notification) ev="approval" ;;
    Stop)         ev="done" ;;
    PostToolUse)  ev="activity" ;;
    *)            exit 0 ;;
esac

payload="$(cat)"

# Extract session_id and cwd. Prefer jq; fall back to grep so the hook
# works on machines without jq installed.
if command -v jq >/dev/null 2>&1; then
    sid="$(printf '%s' "$payload" | jq -r '.session_id // ""' 2>/dev/null)"
    cwd="$(printf '%s' "$payload" | jq -r '.cwd // ""' 2>/dev/null)"
else
    sid="$(printf '%s' "$payload" | grep -o '"session_id"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*:[[:space:]]*"//;s/"$//')"
    cwd="$(printf '%s' "$payload" | grep -o '"cwd"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*:[[:space:]]*"//;s/"$//')"
fi

proj="$(basename "$cwd" 2>/dev/null)"
[ -z "$proj" ] && proj="?"
ts="$(date +%s)"

mkdir -p "$(dirname "$event_file")" 2>/dev/null
printf '{"ts":%s,"sid":"%s","proj":"%s","ev":"%s"}\n' \
    "$ts" "$sid" "$proj" "$ev" >> "$event_file" 2>/dev/null

exit 0
```

- [ ] **Step 4: Make executable and run test to verify it passes**

Run: `chmod +x daemon/clawdmeter-hook.sh && cd daemon && python3 -m pytest tests/test_hook_script.py -v`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add daemon/clawdmeter-hook.sh daemon/tests/test_hook_script.py
git commit -m "feat(daemon): add Claude Code hook script for session events"
```

---

## Task 2: EventTracker state machine

**Files:**
- Modify: `daemon/claude_usage_daemon.py` (add `EventTracker` class near the `Session` class)
- Test: `daemon/tests/test_event_tracker.py`

The tracker is pure logic: feed it parsed event dicts, ask it for the current
BLE payload. No I/O, so it is fully unit-testable. Time is injected so the
10-minute eviction is testable without sleeping.

- [ ] **Step 1: Write the failing test**

```python
# daemon/tests/test_event_tracker.py
from claude_usage_daemon import EventTracker


def test_approval_sets_pending_and_payload(tmp_path):
    t = EventTracker()
    payload = t.feed({"sid": "a", "proj": "Clawdmeter", "ev": "approval", "ts": 100}, now=100)
    assert payload == {"ev": "approval", "proj": "Clawdmeter", "n": 1}


def test_activity_clears_pending_approval():
    t = EventTracker()
    t.feed({"sid": "a", "proj": "P", "ev": "approval", "ts": 100}, now=100)
    payload = t.feed({"sid": "a", "proj": "P", "ev": "activity", "ts": 101}, now=101)
    assert payload == {"ev": "clear"}


def test_done_clears_pending_and_announces_done():
    t = EventTracker()
    t.feed({"sid": "a", "proj": "P", "ev": "approval", "ts": 100}, now=100)
    payload = t.feed({"sid": "a", "proj": "P", "ev": "done", "ts": 102}, now=102)
    assert payload == {"ev": "done", "proj": "P"}


def test_two_pending_reports_count_two():
    t = EventTracker()
    t.feed({"sid": "a", "proj": "P", "ev": "approval", "ts": 100}, now=100)
    payload = t.feed({"sid": "b", "proj": "Q", "ev": "approval", "ts": 101}, now=101)
    assert payload == {"ev": "approval", "proj": "Q", "n": 2}


def test_done_with_others_still_pending_keeps_amber():
    t = EventTracker()
    t.feed({"sid": "a", "proj": "P", "ev": "approval", "ts": 100}, now=100)
    t.feed({"sid": "b", "proj": "Q", "ev": "approval", "ts": 101}, now=101)
    # session b finishes; a is still pending -> stay amber with n=1
    payload = t.feed({"sid": "b", "proj": "Q", "ev": "done", "ts": 102}, now=102)
    assert payload == {"ev": "approval", "proj": "P", "n": 1}


def test_activity_without_pending_returns_none():
    t = EventTracker()
    assert t.feed({"sid": "a", "proj": "P", "ev": "activity", "ts": 100}, now=100) is None


def test_eviction_after_ten_minutes():
    t = EventTracker()
    t.feed({"sid": "a", "proj": "P", "ev": "approval", "ts": 0}, now=0)
    # 11 minutes later a new unrelated event prunes the stale pending session
    payload = t.feed({"sid": "b", "proj": "Q", "ev": "done", "ts": 660}, now=660)
    assert payload == {"ev": "done", "proj": "Q"}  # a was evicted, no longer pending
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd daemon && python3 -m pytest tests/test_event_tracker.py -v`
Expected: FAIL — `ImportError: cannot import name 'EventTracker'`.

- [ ] **Step 3: Implement EventTracker**

Add to `daemon/claude_usage_daemon.py`, immediately above `class Session:`:

```python
EVICT_SECONDS = 600  # forget a session 10 min after its last event


class EventTracker:
    """Pure state machine over normalized session events.

    Feed it event dicts ({"sid","proj","ev","ts"}); it returns the BLE
    payload to send (or None when nothing should change on the device).
    """

    def __init__(self) -> None:
        # sid -> {"proj": str, "pending": bool, "ts": int}
        self._sessions: dict[str, dict] = {}

    def _evict(self, now: float) -> None:
        stale = [sid for sid, s in self._sessions.items()
                 if now - s["ts"] > EVICT_SECONDS]
        for sid in stale:
            del self._sessions[sid]

    def _pending_count(self) -> int:
        return sum(1 for s in self._sessions.values() if s["pending"])

    def _amber_payload(self) -> dict:
        # Most recently touched pending session names the banner.
        pend = [s for s in self._sessions.values() if s["pending"]]
        latest = max(pend, key=lambda s: s["ts"])
        return {"ev": "approval", "proj": latest["proj"], "n": len(pend)}

    def feed(self, event: dict, now: float) -> dict | None:
        self._evict(now)
        sid = event.get("sid", "")
        proj = event.get("proj", "?")
        ev = event.get("ev", "")
        s = self._sessions.setdefault(sid, {"proj": proj, "pending": False, "ts": now})
        s["proj"] = proj
        s["ts"] = now

        if ev == "approval":
            s["pending"] = True
            return self._amber_payload()

        if ev == "activity":
            if s["pending"]:
                s["pending"] = False
                return self._amber_payload() if self._pending_count() else {"ev": "clear"}
            return None

        if ev == "done":
            s["pending"] = False
            if self._pending_count():
                return self._amber_payload()
            return {"ev": "done", "proj": proj}

        return None
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd daemon && python3 -m pytest tests/test_event_tracker.py -v`
Expected: PASS (7 tests).

- [ ] **Step 5: Commit**

```bash
git add daemon/claude_usage_daemon.py daemon/tests/test_event_tracker.py
git commit -m "feat(daemon): add EventTracker session state machine"
```

---

## Task 3: Wire event file watcher into the daemon loop

**Files:**
- Modify: `daemon/claude_usage_daemon.py`

The watcher reads newly appended lines and pushes tracker payloads through the
connected `Session`. It shares the `Session` object with the poll loop. We add
an `EVENT_FILE` constant, truncate it on startup, and run `watch_events()`
concurrently with the poll loop inside `connect_and_run`.

- [ ] **Step 1: Add the EVENT_FILE constant**

After the `SAVED_ADDR_FILE` definition (~line 38), add:

```python
EVENT_FILE = Path.home() / ".config" / "claude-usage-monitor" / "events.jsonl"
EVENT_TICK = 1.0  # seconds between event-file size checks
```

- [ ] **Step 2: Add a parse helper + watcher coroutine**

Add near `EventTracker` (module level):

```python
def parse_event_line(line: str) -> dict | None:
    """Parse one JSONL event line; return None if malformed."""
    line = line.strip()
    if not line:
        return None
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        log(f"Skipping malformed event line: {line[:80]!r}")
        return None
    if not isinstance(obj, dict) or "ev" not in obj:
        return None
    return obj


async def watch_events(session: "Session", tracker: EventTracker,
                       stop_event: asyncio.Event) -> None:
    """Tail EVENT_FILE; push tracker payloads to the device as events arrive."""
    pos = EVENT_FILE.stat().st_size if EVENT_FILE.exists() else 0
    while not stop_event.is_set() and session.client.is_connected:
        try:
            size = EVENT_FILE.stat().st_size if EVENT_FILE.exists() else 0
            if size < pos:
                pos = 0  # file truncated/rotated
            if size > pos:
                with EVENT_FILE.open("r") as f:
                    f.seek(pos)
                    new = f.read()
                    pos = f.tell()
                for line in new.splitlines():
                    obj = parse_event_line(line)
                    if obj is None:
                        continue
                    payload = tracker.feed(obj, now=time.time())
                    if payload is not None:
                        await session.write_payload(payload)
        except (OSError, BleakError) as e:
            log(f"Event watch error: {e}")
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=EVENT_TICK)
        except asyncio.TimeoutError:
            pass
```

- [ ] **Step 3: Truncate the event file on startup**

In `main()`, right after the `log("=== Claude Usage Tracker Daemon...")` line, add:

```python
    # Stale events from before this daemon started are meaningless.
    try:
        EVENT_FILE.parent.mkdir(parents=True, exist_ok=True)
        EVENT_FILE.write_text("")
    except OSError as e:
        log(f"Could not reset event file: {e}")
```

- [ ] **Step 4: Run the watcher alongside the poll loop**

In `connect_and_run`, after `await session.setup_refresh_subscription()`, replace the
existing poll `while` loop so the watcher runs concurrently. Wrap the existing
poll loop body in a nested coroutine and gather both:

```python
    session = Session(client)
    await session.setup_refresh_subscription()
    tracker = EventTracker()

    used = {"ok": False}

    async def poll_loop() -> None:
        last_poll = 0.0
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            if session.refresh_requested.is_set() or (now - last_poll) >= POLL_INTERVAL:
                session.refresh_requested.clear()
                token = read_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
                    if payload is not None and await session.write_payload(payload):
                        last_poll = time.time()
                        used["ok"] = True
            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass

    try:
        await asyncio.gather(
            poll_loop(),
            watch_events(session, tracker, stop_event),
        )
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used["ok"]
```

(Remove the old inline `last_poll`/`used_successfully` loop and its `finally`
block that this replaces.)

- [ ] **Step 5: Verify the daemon still imports and polls**

Run: `cd daemon && python3 -c "import claude_usage_daemon; print('import ok')"`
Expected: `import ok`

Run: `cd daemon && python3 -m pytest tests/test_event_tracker.py -v`
Expected: PASS (still 7 tests; no regression).

- [ ] **Step 6: Commit**

```bash
git add daemon/claude_usage_daemon.py
git commit -m "feat(daemon): tail event file and push session events over BLE"
```

---

## Task 4: Firmware — SessionEvent struct and payload routing

**Files:**
- Modify: `firmware/src/data.h`
- Modify: `firmware/src/main.cpp`

- [ ] **Step 1: Add the SessionEvent struct**

In `firmware/src/data.h`, after the `UsageData` struct:

```c
struct SessionEvent {
    char type[12];   // "approval" | "done" | "clear"
    char proj[24];   // project name, truncated
    uint8_t count;   // pending approvals (>=1 for "approval")
    bool fresh;      // set when a new event arrives, cleared by the UI
};
```

- [ ] **Step 2: Add an event parser in main.cpp**

In `firmware/src/main.cpp`, after `parse_json` (~line 116), add:

```cpp
static SessionEvent session_event = {};

// Parse an event payload (has an "ev" key). Returns true if handled.
static bool parse_event_json(const char* json, SessionEvent* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;
    if (!doc["ev"].is<const char*>()) return false;
    strlcpy(out->type, doc["ev"] | "", sizeof(out->type));
    strlcpy(out->proj, doc["proj"] | "", sizeof(out->proj));
    out->count = doc["n"] | 1;
    out->fresh = true;
    return true;
}
```

- [ ] **Step 3: Route payloads in the BLE handling block**

In `main.cpp`, replace the `if (ble_has_data())` block (~lines 360-375) with a
version that checks for the `"ev"` key first:

```cpp
    if (ble_has_data()) {
        const char* raw = ble_get_data();
        if (strstr(raw, "\"ev\"") != nullptr) {
            if (parse_event_json(raw, &session_event)) {
                ui_show_event(&session_event);
                ble_send_ack();
            } else {
                ble_send_nack();
            }
        } else if (parse_json(raw, &usage)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }
```

- [ ] **Step 4: Compile-check (will fail until Task 5 adds ui_show_event)**

Run: `pio run -d firmware -e waveshare_amoled_216 2>&1 | tail -5`
Expected: FAIL — `'ui_show_event' was not declared`. This confirms wiring is in place; Task 5 resolves it.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/data.h firmware/src/main.cpp
git commit -m "feat(firmware): parse and route BLE session-event payloads"
```

---

## Task 5: Firmware — banner overlay UI

**Files:**
- Modify: `firmware/src/ui.h`
- Modify: `firmware/src/ui.cpp`

The banner lives on `lv_layer_top()` so it floats over every screen on every
board. It is created once (lazily) and shown/hidden on demand. Amber persists;
green ("done") auto-dismisses after 30 s. Tap dismisses either.

- [ ] **Step 1: Declare the UI entry points in ui.h**

In `firmware/src/ui.h`, after `void ui_update(const UsageData* data);`:

```c
void ui_show_event(const SessionEvent* ev);
void ui_banner_tick(void);   // call each loop; handles the 30s auto-dismiss
```

Ensure `ui.h` includes the struct: it already includes `data.h` indirectly via
`ui_update(const UsageData*)`. If `data.h` is not included in `ui.h`, add
`#include "data.h"` near the top.

- [ ] **Step 2: Implement the banner in ui.cpp**

At the top of `firmware/src/ui.cpp` with the other statics, add:

```cpp
// ---- Session-event banner (floats on the top layer over any screen) ----
static lv_obj_t* banner = nullptr;
static lv_obj_t* banner_lbl = nullptr;
static uint32_t banner_hide_at = 0;   // lv_tick when a "done" banner self-hides

static void banner_tap_cb(lv_event_t* e) {
    LV_UNUSED(e);
    if (banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    banner_hide_at = 0;
}

static void banner_ensure(void) {
    if (banner) return;
    const BoardCaps* c = board_caps();
    banner = lv_obj_create(lv_layer_top());
    // Full-width strip near the top, inside the 20px rounded-corner margin.
    lv_obj_set_size(banner, c->W - 40, 56);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(banner, 12, 0);
    lv_obj_set_style_border_width(banner, 0, 0);
    lv_obj_set_style_pad_all(banner, 8, 0);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(banner, banner_tap_cb, LV_EVENT_CLICKED, nullptr);

    banner_lbl = lv_label_create(banner);
    lv_obj_set_style_text_font(banner_lbl, &font_styrene_20, 0);
    lv_obj_set_style_text_color(banner_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(banner_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(banner_lbl, c->W - 56);
    lv_obj_center(banner_lbl);
}

void ui_show_event(const SessionEvent* ev) {
    if (!ev) return;
    banner_ensure();
    char text[48];

    if (strcmp(ev->type, "clear") == 0) {
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        banner_hide_at = 0;
        return;
    }

    if (strcmp(ev->type, "approval") == 0) {
        lv_obj_set_style_bg_color(banner, lv_color_hex(0xB8860B), 0);  // amber
        if (ev->count > 1) {
            snprintf(text, sizeof(text), LV_SYMBOL_WARNING " %s  (%u pendientes)",
                     ev->proj, (unsigned)ev->count);
        } else {
            snprintf(text, sizeof(text), LV_SYMBOL_WARNING " %s  aprobacion", ev->proj);
        }
        lv_label_set_text(banner_lbl, text);
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
        banner_hide_at = 0;  // persists until daemon clears it
        return;
    }

    if (strcmp(ev->type, "done") == 0) {
        lv_obj_set_style_bg_color(banner, lv_color_hex(0x1E7B34), 0);  // green
        snprintf(text, sizeof(text), LV_SYMBOL_OK " %s  listo", ev->proj);
        lv_label_set_text(banner_lbl, text);
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
        banner_hide_at = lv_tick_get() + 30000;  // auto-dismiss in 30s
        return;
    }
}

void ui_banner_tick(void) {
    if (banner_hide_at != 0 && lv_tick_get() >= banner_hide_at) {
        if (banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        banner_hide_at = 0;
    }
}
```

- [ ] **Step 3: Call ui_banner_tick from the main loop**

In `firmware/src/main.cpp`, in the main `loop()` near the other periodic UI
calls (after `check_serial_cmd();`, ~line 358), add:

```cpp
    ui_banner_tick();
```

- [ ] **Step 4: Compile all three environments**

Run: `pio run -d firmware -e waveshare_amoled_216 2>&1 | tail -3`
Expected: `SUCCESS`

Run: `pio run -d firmware -e waveshare_amoled_18 2>&1 | tail -3`
Expected: `SUCCESS`

Run: `pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -3`
Expected: `SUCCESS`

- [ ] **Step 5: Commit**

```bash
git add firmware/src/ui.h firmware/src/ui.cpp firmware/src/main.cpp
git commit -m "feat(firmware): session-event banner overlay on top layer"
```

---

## Task 6: Visual QA of the banner

**Files:** none (verification only)

- [ ] **Step 1: Temporarily force a banner for screenshotting**

In `firmware/src/main.cpp` `setup()`, after `ui_init();`, temporarily add:

```cpp
    static SessionEvent demo = {"approval", "Clawdmeter", 2, true};
    ui_show_event(&demo);
```

- [ ] **Step 2: Flash and screenshot**

Run: `pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem101`
Then: `./screenshot.sh /tmp/banner-amber.png`
Read `/tmp/banner-amber.png` with the Read tool. Verify: amber strip near top,
text `⚠ Clawdmeter  (2 pendientes)`, not clipped by rounded corners, readable
over the splash.

- [ ] **Step 3: Repeat for the "done" banner**

Change the demo line to `{"done", "backend-parkingapp", 1, true}`, rebuild,
flash, `./screenshot.sh /tmp/banner-done.png`, Read it. Verify green strip,
text `✓ backend-parkingapp  listo`.

- [ ] **Step 4: Remove the demo code**

Delete the two temporary `demo` lines from `setup()`. Rebuild to confirm clean:
Run: `pio run -d firmware -e waveshare_amoled_216 2>&1 | tail -3`
Expected: `SUCCESS`

- [ ] **Step 5: Commit (only if any non-demo tweaks were needed)**

```bash
git add firmware/src/ui.cpp
git commit -m "fix(firmware): banner layout tweaks from visual QA"
```

(Skip this commit if no changes beyond removing demo code were required.)

---

## Task 7: Document hook installation

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add a "Session event notifications" section to README**

Append to `README.md` (find the daemon section; add after it):

````markdown
## Session event notifications (macOS)

Clawdmeter can show a banner when a Claude Code session is waiting for your
approval, or when one finishes. This uses Claude Code hooks that append events
to `~/.config/claude-usage-monitor/events.jsonl`, which the daemon tails.

Add to `~/.claude/settings.json` (merge into any existing `hooks` block):

```json
{
  "hooks": {
    "Notification": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "/ABS/PATH/Clawdmeter/daemon/clawdmeter-hook.sh Notification" } ] }
    ],
    "Stop": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "/ABS/PATH/Clawdmeter/daemon/clawdmeter-hook.sh Stop" } ] }
    ],
    "PostToolUse": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "/ABS/PATH/Clawdmeter/daemon/clawdmeter-hook.sh PostToolUse" } ] }
    ]
  }
}
```

Replace `/ABS/PATH/` with the absolute path to your checkout. Restart Claude
Code so it reloads settings. The daemon picks up events within ~1 second.
````

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: document Clawdmeter session-event hook installation"
```

---

## Self-Review Notes

- **Spec coverage:** hooks (Task 1), append-file not FIFO (Task 1 + 3), EventTracker state machine with activity/done/clear rules and 10-min eviction (Task 2), 1s watcher + startup truncate (Task 3), SessionEvent struct + dual-payload routing (Task 4), `lv_layer_top` banner amber/green with tap + 30s dismiss + `board_caps()` sizing (Task 5), visual QA (Task 6), README hook install (Task 7). Windows/Linux explicitly out of scope per spec.
- **Type consistency:** `ui_show_event(const SessionEvent*)`, `ui_banner_tick(void)`, `EventTracker.feed(event, now)` and payload dicts (`{"ev","proj","n"}` / `{"ev":"clear"}` / `{"ev":"done","proj"}`) are consistent across daemon, firmware, and tests.
- **No placeholders:** every code/test/command step is concrete.
