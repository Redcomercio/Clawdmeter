# Clawdio Milestones & Reactions — Phase A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give Clawdio event reactions and milestone celebrations — a positive-feedback loop with no decay/death/penalties.

**Architecture:** The daemon is the brain: it counts/persists milestones in `clawdio-state.json` and sends a `{"ev":"milestone",...}` over BLE when one unlocks. The firmware reacts to events with short timed animations and, on a milestone, plays a festive burst → holds a proud state (30–60 min) → shows a tappable trophy badge that replays the burst. Phase A only (no trophy gallery / no new screen).

**Tech Stack:** POSIX shell (hook), Python 3 + asyncio/bleak (daemon, run via `daemon/.venv2`), C++ / Arduino / LVGL 9 (firmware C6 AMOLED-2.16).

**Spec:** `docs/superpowers/specs/2026-06-14-clawdio-milestones-reactions-design.md`

---

## File Structure

- `daemon/clawdmeter-hook.sh` (modify) — also emit a `commit` event when a Bash command contains `git commit`.
- `daemon/milestone_engine.py` (create) — `MilestoneEngine`: pure logic over events + usage + date; returns newly-unlocked milestones; owns the state dict.
- `daemon/claude_usage_daemon.py` (modify) — load/persist `clawdio-state.json`, feed the engine from events + usage, send `milestone` payloads; add a `danger` flag to the approval `ask` payload.
- `daemon/tests/test_milestone_engine.py` (create) — engine unit tests.
- `daemon/tests/test_commit_hook.py` (create) — hook commit-detection test.
- `firmware/src/splash.{h,cpp}` (modify) — `splash_pin_anim_for(name, ms)` timed pin.
- `firmware/src/data.h` (modify) — `Milestone` struct.
- `firmware/src/ui.{h,cpp}` (modify) — reactions (done→festejo, approve→contento), milestone burst + proud state + tappable badge + replay.
- `firmware/src/main.cpp` (modify) — route `{"ev":"milestone",...}` payloads.

---

## Task 1: Commit detection in the events hook

**Files:**
- Modify: `daemon/clawdmeter-hook.sh`
- Test: `daemon/tests/test_commit_hook.py`

The hook currently maps `PostToolUse → activity`. Extend it so a Bash `git commit`
command ALSO appends a `commit` event line (the activity line stays).

- [ ] **Step 1: Write the failing test**

```python
# daemon/tests/test_commit_hook.py
import json, subprocess
from pathlib import Path

HOOK = Path(__file__).resolve().parents[1] / "clawdmeter-hook.sh"


def run(stdin_obj, event_file):
    subprocess.run(["bash", str(HOOK), "PostToolUse"], input=json.dumps(stdin_obj),
                   capture_output=True, text=True,
                   env={"CLAWDMETER_EVENT_FILE": str(event_file), "PATH": "/usr/bin:/bin"})


def evs(event_file):
    return [json.loads(l) for l in Path(event_file).read_text().splitlines() if l.strip()]


def test_git_commit_emits_commit_event(tmp_path):
    ef = tmp_path / "events.jsonl"
    run({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
         "tool_input": {"command": "git commit -m 'wip'"}}, ef)
    kinds = [e["ev"] for e in evs(ef)]
    assert "activity" in kinds
    assert "commit" in kinds


def test_non_commit_bash_no_commit_event(tmp_path):
    ef = tmp_path / "events.jsonl"
    run({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
         "tool_input": {"command": "git status"}}, ef)
    kinds = [e["ev"] for e in evs(ef)]
    assert "activity" in kinds
    assert "commit" not in kinds


def test_non_bash_tool_no_commit_event(tmp_path):
    ef = tmp_path / "events.jsonl"
    run({"session_id": "s", "cwd": "/x/proj", "tool_name": "Read",
         "tool_input": {"file_path": "git commit"}}, ef)
    assert "commit" not in [e["ev"] for e in evs(ef)]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_commit_hook.py -v`
Expected: FAIL — no `commit` event emitted.

- [ ] **Step 3: Implement commit detection**

In `daemon/clawdmeter-hook.sh`, after the existing block that writes the normalized
line (the `printf ... >> "$event_file"` near the end, before `exit 0`), add:

```bash
# Extra signal: a Bash `git commit` also emits a `commit` event (for milestones).
if [ "$event_name" = "PostToolUse" ]; then
    tool="$(printf '%s' "$payload" | { command -v jq >/dev/null 2>&1 \
        && jq -r '.tool_name // ""' 2>/dev/null \
        || grep -o '"tool_name"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*:[[:space:]]*"//;s/"$//'; })"
    cmd="$(printf '%s' "$payload" | { command -v jq >/dev/null 2>&1 \
        && jq -r '.tool_input.command // ""' 2>/dev/null \
        || grep -o '"command"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*:[[:space:]]*"//;s/"$//'; })"
    if [ "$tool" = "Bash" ] && printf '%s' "$cmd" | grep -q 'git commit'; then
        printf '{"ts":%s,"sid":"%s","proj":"%s","ev":"commit"}\n' \
            "$ts" "$sid" "$proj" >> "$event_file" 2>/dev/null
    fi
fi
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_commit_hook.py -v`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add daemon/clawdmeter-hook.sh daemon/tests/test_commit_hook.py
git commit -m "feat(daemon): hook emits a commit event on 'git commit'

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2: MilestoneEngine (pure logic)

**Files:**
- Create: `daemon/milestone_engine.py`
- Test: `daemon/tests/test_milestone_engine.py`

Pure logic: feed it events + usage + the current date (`YYYY-MM-DD`); it updates
its state dict and returns newly-unlocked milestones (each `{"id","label","anim"}`).
No I/O — the daemon persists `engine.state`.

- [ ] **Step 1: Write the failing test**

```python
# daemon/tests/test_milestone_engine.py
from milestone_engine import MilestoneEngine


def ev(kind):
    return {"ev": kind, "sid": "s", "proj": "P"}


def test_first_active_day_sets_streak_one(_=None):
    e = MilestoneEngine()
    e.feed_event(ev("activity"), "2026-06-14")
    assert e.state["streak_count"] == 1


def test_consecutive_days_increment_streak_and_unlock_3(_=None):
    e = MilestoneEngine()
    e.feed_event(ev("activity"), "2026-06-12")
    e.feed_event(ev("activity"), "2026-06-13")
    out = e.feed_event(ev("activity"), "2026-06-14")
    assert e.state["streak_count"] == 3
    assert any(m["id"] == "streak3" for m in out)


def test_gap_resets_streak(_=None):
    e = MilestoneEngine()
    e.feed_event(ev("activity"), "2026-06-12")
    e.feed_event(ev("activity"), "2026-06-14")  # skipped the 13th
    assert e.state["streak_count"] == 1


def test_same_day_does_not_increment(_=None):
    e = MilestoneEngine()
    e.feed_event(ev("activity"), "2026-06-14")
    e.feed_event(ev("done"), "2026-06-14")
    assert e.state["streak_count"] == 1


def test_done_counts_tasks_and_unlocks_daily_10(_=None):
    e = MilestoneEngine()
    out = []
    for _i in range(10):
        out += e.feed_event(ev("done"), "2026-06-14")
    assert e.state["tasks_today"] == 10
    assert e.state["tasks_total"] == 10
    assert any(m["id"] == "tasks_day10" for m in out)


def test_tasks_today_resets_on_new_day(_=None):
    e = MilestoneEngine()
    e.feed_event(ev("done"), "2026-06-14")
    e.feed_event(ev("done"), "2026-06-15")
    assert e.state["tasks_today"] == 1
    assert e.state["tasks_total"] == 2


def test_commit_counts_and_unlocks_first(_=None):
    e = MilestoneEngine()
    out = e.feed_event(ev("commit"), "2026-06-14")
    assert e.state["commits_total"] == 1
    assert any(m["id"] == "commit1" for m in out)


def test_usage_in_zone_unlocks_once(_=None):
    e = MilestoneEngine()
    out1 = e.feed_usage(30.0, "2026-06-14")
    out2 = e.feed_usage(33.0, "2026-06-14")
    assert any(m["id"] == "in_zone" for m in out1)
    assert not any(m["id"] == "in_zone" for m in out2)


def test_usage_marathon_unlocks_once(_=None):
    e = MilestoneEngine()
    out1 = e.feed_usage(85.0, "2026-06-14")
    out2 = e.feed_usage(90.0, "2026-06-15")
    assert any(m["id"] == "marathon" for m in out1)
    assert not any(m["id"] == "marathon" for m in out2)


def test_unlock_fires_once_across_restart(_=None):
    e = MilestoneEngine()
    for _i in range(10):
        e.feed_event(ev("done"), "2026-06-14")
    saved = e.state
    e2 = MilestoneEngine(state=saved)          # simulate restart
    out = e2.feed_event(ev("done"), "2026-06-14")
    assert not any(m["id"] == "tasks_day10" for m in out)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_milestone_engine.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'milestone_engine'`.

- [ ] **Step 3: Implement the engine**

```python
# daemon/milestone_engine.py
"""Pure milestone logic for Clawdio. No I/O — the daemon persists `state`.

Feed normalized events (feed_event) and usage percentages (feed_usage) with the
current local date ('YYYY-MM-DD'); each call returns the milestones newly unlocked
on that call: [{"id","label","anim"}, ...]. Each milestone fires at most once
(tracked in state['unlocked'])."""
from datetime import date

# id -> (label, animation). Thresholds are encoded in _check below.
STREAKS = [(3, "streak3", "🔥 3 días seguidos"),
           (7, "streak7", "🔥 7 días seguidos"),
           (14, "streak14", "🔥 14 días seguidos"),
           (30, "streak30", "🔥 30 días seguidos")]
TASKS_DAY = [(10, "tasks_day10", "✅ 10 tareas hoy"),
             (25, "tasks_day25", "✅ 25 tareas hoy")]
TASKS_TOTAL = [(100, "tasks100", "🏅 100 tareas"),
               (500, "tasks500", "🏅 500 tareas"),
               (1000, "tasks1000", "🏅 1000 tareas")]
COMMITS = [(1, "commit1", "💾 Primer commit"),
           (10, "commit10", "💾 10 commits"),
           (50, "commit50", "💾 50 commits"),
           (100, "commit100", "💾 100 commits")]

FESTIVE_ANIM = "dance bounce"


def _default_state() -> dict:
    return {
        "last_active_date": None, "streak_count": 0, "best_streak": 0,
        "tasks_today": 0, "tasks_today_date": None, "tasks_total": 0,
        "commits_total": 0, "usage_in_zone_seen": False,
        "usage_marathon_dates": [], "unlocked": [],
    }


class MilestoneEngine:
    def __init__(self, state: dict | None = None) -> None:
        self.state = _default_state()
        if state:
            self.state.update(state)

    def _unlock(self, mid: str, label: str) -> dict | None:
        if mid in self.state["unlocked"]:
            return None
        self.state["unlocked"].append(mid)
        return {"id": mid, "label": label, "anim": FESTIVE_ANIM}

    def _check_thresholds(self, table, value) -> list[dict]:
        out = []
        for threshold, mid, label in table:
            if value >= threshold:
                m = self._unlock(mid, label)
                if m:
                    out.append(m)
        return out

    def feed_event(self, ev: dict, today: str) -> list[dict]:
        out = []
        # --- streak (any event marks an active day) ---
        last = self.state["last_active_date"]
        if last != today:
            if last is not None:
                delta = (date.fromisoformat(today) - date.fromisoformat(last)).days
                self.state["streak_count"] = (
                    self.state["streak_count"] + 1 if delta == 1 else 1)
            else:
                self.state["streak_count"] = 1
            self.state["last_active_date"] = today
            self.state["best_streak"] = max(
                self.state["best_streak"], self.state["streak_count"])
        out += self._check_thresholds(STREAKS, self.state["streak_count"])

        kind = ev.get("ev")
        if kind == "done":
            if self.state["tasks_today_date"] != today:
                self.state["tasks_today"] = 0
                self.state["tasks_today_date"] = today
            self.state["tasks_today"] += 1
            self.state["tasks_total"] += 1
            out += self._check_thresholds(TASKS_DAY, self.state["tasks_today"])
            out += self._check_thresholds(TASKS_TOTAL, self.state["tasks_total"])
        elif kind == "commit":
            self.state["commits_total"] += 1
            out += self._check_thresholds(COMMITS, self.state["commits_total"])
        return out

    def feed_usage(self, session_pct: float, today: str) -> list[dict]:
        out = []
        if 12.0 <= session_pct < 45.0 and not self.state["usage_in_zone_seen"]:
            self.state["usage_in_zone_seen"] = True
            m = self._unlock("in_zone", "📊 En su salsa")
            if m:
                out.append(m)
        if session_pct >= 80.0:
            if today not in self.state["usage_marathon_dates"]:
                self.state["usage_marathon_dates"].append(today)
            m = self._unlock("marathon", "📊 Maratón superado")
            if m:
                out.append(m)
        return out
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_milestone_engine.py -v`
Expected: PASS (10 tests).

- [ ] **Step 5: Commit**

```bash
git add daemon/milestone_engine.py daemon/tests/test_milestone_engine.py
git commit -m "feat(daemon): MilestoneEngine — streaks/tasks/commits/usage, unlock-once

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3: Daemon integration — persist state, feed engine, send milestones, danger flag

**Files:**
- Modify: `daemon/claude_usage_daemon.py`

Context: `watch_events()` parses each event line and feeds `tracker`; `poll_loop()`
gets a usage `payload` (dict with key `"s"` = session %). `connect_and_run` takes
`(target, stop_event, tracker, watch_pos)`; `main()` creates `tracker`/`watch_pos`
before the reconnect loop. `ApprovalBroker.scan()` builds the `ask` payload.

- [ ] **Step 1: Add constants + state load/save helpers**

After the `APPROVE_DIR` constant (~line 45), add:

```python
CLAWDIO_STATE_FILE = Path.home() / ".config" / "claude-usage-monitor" / "clawdio-state.json"
```

Add the import near the others:

```python
from milestone_engine import MilestoneEngine
```

Add module-level helpers near `touch_device_ready`:

```python
def load_clawdio_state() -> dict | None:
    try:
        return json.loads(CLAWDIO_STATE_FILE.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def save_clawdio_state(state: dict) -> None:
    try:
        CLAWDIO_STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        tmp = CLAWDIO_STATE_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(state))
        tmp.replace(CLAWDIO_STATE_FILE)   # atomic
    except OSError as e:
        log(f"Could not save clawdio state: {e}")


def _local_today() -> str:
    return time.strftime("%Y-%m-%d")
```

- [ ] **Step 2: Create the engine in main() and thread it through**

In `main()`, where `tracker = EventTracker()` and `watch_pos = {...}` are created,
add:

```python
    engine = MilestoneEngine(state=load_clawdio_state())
```

Change the `connect_and_run(...)` call in `main()` to pass it:

```python
        ok = await connect_and_run(target, stop_event, tracker, watch_pos, engine)
```

Change `connect_and_run`'s signature to accept it:

```python
async def connect_and_run(target, stop_event: asyncio.Event,
                          tracker, watch_pos, engine) -> bool:
```

And pass it into the watch task — change the `watch_events(...)` task creation:

```python
    watch_task = asyncio.create_task(
        watch_events(session, tracker, watch_pos, stop_event, engine))
```

- [ ] **Step 3: Feed events into the engine in watch_events**

Change `watch_events`'s signature and feed the engine per event. Replace the
function's signature line and the per-event handling:

```python
async def watch_events(session: "Session", tracker: EventTracker,
                       watch_pos: dict, stop_event: asyncio.Event, engine) -> None:
```

Inside the `for line in new.splitlines():` loop, after the existing
`payload = tracker.feed(obj, now=time.time())` / `await session.write_payload(payload)`
block, add:

```python
                    for m in engine.feed_event(obj, _local_today()):
                        save_clawdio_state(engine.state)
                        await session.write_payload(
                            {"ev": "milestone", "id": m["id"],
                             "label": m["label"], "anim": m["anim"]})
```

- [ ] **Step 4: Feed usage into the engine in poll_loop**

In `poll_loop()` (inside `connect_and_run`), after a successful usage write — find
the block where `payload` was sent and `used["ok"] = True` set — add right after it:

```python
                        try:
                            pct = float(payload.get("s", 0))
                        except (TypeError, ValueError):
                            pct = 0.0
                        for m in engine.feed_usage(pct, _local_today()):
                            save_clawdio_state(engine.state)
                            await session.write_payload(
                                {"ev": "milestone", "id": m["id"],
                                 "label": m["label"], "anim": m["anim"]})
```

- [ ] **Step 5: Add the danger flag to the approval ask payload**

In `daemon/approval_broker.py`, add a module-level helper and set the flag in
`scan()`'s returned dict. Add near the top:

```python
import re

_DANGER = re.compile(
    r"rm\s+-rf|--force\b|\s-f\b|mkfs|dd\s+if=|:\(\)\s*\{|>\s*/dev/|sudo\s+rm")


def _is_dangerous(cmd: str) -> bool:
    return bool(cmd and _DANGER.search(cmd))
```

In `scan()`, where the return dict is built, add the flag:

```python
        return {"ev": "ask", "id": head, "proj": req.get("proj", "?"),
                "tool": req.get("tool", ""), "cmd": req.get("cmd", ""),
                "pos": 1, "total": len(self._queue),
                "danger": _is_dangerous(req.get("cmd", ""))}
```

- [ ] **Step 6: Verify import + no regressions**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/python -c "import claude_usage_daemon; print('import ok')"`
Expected: `import ok`

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_milestone_engine.py tests/test_commit_hook.py tests/test_event_tracker.py tests/test_approval_broker.py tests/test_approve_hook.py tests/test_hook_script.py -q`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add daemon/claude_usage_daemon.py daemon/approval_broker.py
git commit -m "feat(daemon): feed MilestoneEngine, persist state, send milestones, danger flag

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4: Firmware — timed pin, Milestone struct, RX routing

**Files:**
- Modify: `firmware/src/splash.h`, `firmware/src/splash.cpp`, `firmware/src/data.h`, `firmware/src/main.cpp`

Note: this task's final compile FAILS referencing `ui_show_milestone` (added in
Task 6). That expected failure is the success signal.

- [ ] **Step 1: Add splash_pin_anim_for (timed pin) declaration**

In `firmware/src/splash.h`, after `void splash_unpin_anim(void);`:

```c
// Pin an animation for `ms` milliseconds, then auto-release back to rotation.
// Used for momentary reactions / celebration bursts. Call splash_tick() each loop.
void splash_pin_anim_for(const char* name, uint32_t ms);
```

- [ ] **Step 2: Implement the timed pin in splash.cpp**

In `firmware/src/splash.cpp`, add a static near `pinned_anim`:

```cpp
static uint32_t pin_until_ms = 0;   // 0 = indefinite/none; else auto-unpin time
```

Add the function near `splash_pin_anim`:

```cpp
void splash_pin_anim_for(const char* name, uint32_t ms) {
    splash_pin_anim(name);
    pin_until_ms = millis() + ms;
}
```

In `splash_unpin_anim()`, also clear the timer — change it to:

```cpp
void splash_unpin_anim(void) {
    if (pinned_anim < 0) return;
    pinned_anim = -1;
    pin_until_ms = 0;
    if (active) splash_pick_for_current_rate();  // resume normal rotation
}
```

In `splash_tick()`, at the very top (before the existing auto-rotate block), add:

```cpp
    if (pin_until_ms != 0 && millis() >= pin_until_ms) {
        pin_until_ms = 0;
        splash_unpin_anim();
    }
```

- [ ] **Step 3: Change the rotation interval to 20s → keep (reactions are short)**

No change needed here — reactions use the timed pin; the 20s base rotation is
unchanged in Phase A. (Expression-duration tuning is out of scope for this plan.)

- [ ] **Step 4: Add the Milestone struct**

In `firmware/src/data.h`, after `ApprovalRequest`:

```c
struct Milestone {
    char label[40];   // e.g. "🔥 7 días seguidos"
    char anim[20];    // festive animation name
    bool fresh;
};
```

- [ ] **Step 5: Route the milestone payload in main.cpp**

In `firmware/src/main.cpp`, after `parse_approval_json` add a parser:

```cpp
static Milestone milestone = {};

// Parse an {"ev":"milestone",...} payload.
static bool parse_milestone_json(const char* json, Milestone* out) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    strlcpy(out->label, doc["label"] | "", sizeof(out->label));
    strlcpy(out->anim,  doc["anim"]  | "dance bounce", sizeof(out->anim));
    out->fresh = true;
    return true;
}
```

In the BLE routing block, add a `milestone` branch BEFORE the generic `"ev"`
branch (the milestone payload also contains `"ev":"milestone"`, so check the
distinguishing token first). Find the `if (strstr(raw, "\"ask\"") ...)` chain and
insert after the `ask` branch:

```cpp
        } else if (strstr(raw, "\"milestone\"") != nullptr) {
            if (parse_milestone_json(raw, &milestone)) {
                ui_show_milestone(&milestone);
                ble_send_ack();
            } else {
                ble_send_nack();
            }
```

(Place it so the order is: `ask` → `milestone` → `ev` → usage.)

- [ ] **Step 6: Compile-check (EXPECTED FAIL until Task 6)**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter && /usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -6`
Expected: FAIL — `'ui_show_milestone' was not declared`. Confirms wiring; Task 6 adds it.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/splash.h firmware/src/splash.cpp firmware/src/data.h firmware/src/main.cpp
git commit -m "feat(firmware): timed splash pin + Milestone struct + milestone routing

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 5: Firmware — event reactions

**Files:**
- Modify: `firmware/src/ui.cpp`

Use the timed pin for momentary reactions. Clawdio's danger reaction reuses the
existing surprise pin on the approval card; activity→focused is already mood-driven.

- [ ] **Step 1: Add the festejo reaction on `done`**

In `firmware/src/ui.cpp`, in `ui_show_event()`'s `"done"` branch (it sets the green
banner and `approval_on = false`), add a festive reaction. Find:

```cpp
        approval_on = false;  // nothing pending when a done payload arrives
        cloud_update_visibility();
        return;
    }
```

and insert before `cloud_update_visibility();`:

```cpp
        splash_pin_anim_for("dance bounce", 2500);  // festejo: tarea terminada
```

- [ ] **Step 2: Add the contento reaction on approve**

In `card_finish()`, in the `approve` path, add a happy wink before the confirm
flash. Find the `card_finish` body where `confirm_flash(decision)` is called at the
end, and change the tail so approve also winks:

```cpp
    splash_unpin_anim();
    if (strcmp(decision, "approve") == 0)
        splash_pin_anim_for("expression wink", 1500);  // contento
    confirm_flash(decision);        // visual confirmation of what was pressed
}
```

- [ ] **Step 3: Make danger show scared on the card**

In `ui_show_approval()`, it already calls `splash_pin_anim("expression surprise")`.
The struct has no danger field yet; danger is conveyed by the daemon and is visually
the same (surprise). No code change needed in Phase A — `expression surprise`
already reads as alarmed. (Documented as intentional in the spec.)

- [ ] **Step 4: Compile-check (still expects the Task 6 symbol)**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter && /usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -4`
Expected: FAIL only on `'ui_show_milestone' was not declared` (reactions compile clean).

- [ ] **Step 5: Commit**

```bash
git add firmware/src/ui.cpp
git commit -m "feat(firmware): event reactions — festejo on done, contento on approve

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 6: Firmware — milestone celebration (burst + proud state + tappable badge)

**Files:**
- Modify: `firmware/src/ui.h`, `firmware/src/ui.cpp`

Burst (~5s festive anim + toast) → proud sustained state (random 30–60 min) holding
a calm-happy animation + a tappable trophy badge in a corner. Tapping the badge
replays the burst.

- [ ] **Step 1: Declare the UI entry points in ui.h**

In `firmware/src/ui.h`, after the approval declarations:

```c
void ui_show_milestone(const Milestone* m);
void ui_milestone_tick(void);   // call each loop; handles burst/proud timers
```

- [ ] **Step 2: Implement the celebration in ui.cpp**

Add with the other overlay code (near the banner/cloud block):

```cpp
// ---- Milestone celebration: burst + proud state + tappable replay badge ----
static lv_obj_t* mile_toast = nullptr;       // festive toast (burst)
static lv_obj_t* mile_toast_lbl = nullptr;
static lv_obj_t* mile_badge = nullptr;       // persistent corner trophy
static lv_obj_t* mile_badge_lbl = nullptr;
static char      mile_label[40] = {0};
static char      mile_anim[20] = {0};
static uint32_t  mile_toast_hide_at = 0;     // burst toast end
static uint32_t  mile_proud_until = 0;       // proud state end

static void mile_ensure(void) {
    if (mile_toast) return;
    const BoardCaps& c = board_caps();
    // Festive toast — wide strip near the top.
    mile_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mile_toast, c.width - 40, 64);
    lv_obj_align(mile_toast, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(mile_toast, 14, 0);
    lv_obj_set_style_border_width(mile_toast, 0, 0);
    lv_obj_set_style_bg_color(mile_toast, lv_color_hex(0x7a5cff), 0);  // festive purple
    lv_obj_clear_flag(mile_toast, LV_OBJ_FLAG_SCROLLABLE);
    mile_toast_lbl = lv_label_create(mile_toast);
    lv_obj_set_style_text_font(mile_toast_lbl, &font_styrene_24, 0);
    lv_obj_set_style_text_color(mile_toast_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(mile_toast_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(mile_toast_lbl, c.width - 72);
    lv_obj_center(mile_toast_lbl);
    lv_obj_add_flag(mile_toast, LV_OBJ_FLAG_HIDDEN);

    // Persistent tappable badge — small, bottom-left corner.
    mile_badge = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mile_badge, 64, 64);
    lv_obj_align(mile_badge, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_radius(mile_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mile_badge, 0, 0);
    lv_obj_set_style_bg_color(mile_badge, lv_color_hex(0x7a5cff), 0);
    lv_obj_clear_flag(mile_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mile_badge, LV_OBJ_FLAG_CLICKABLE);
    mile_badge_lbl = lv_label_create(mile_badge);
    lv_obj_set_style_text_font(mile_badge_lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(mile_badge_lbl, lv_color_white(), 0);
    lv_label_set_text(mile_badge_lbl, LV_SYMBOL_OK);  // trophy stand-in
    lv_obj_center(mile_badge_lbl);
    lv_obj_add_flag(mile_badge, LV_OBJ_FLAG_HIDDEN);
}

static void mile_burst(void) {
    splash_pin_anim_for(mile_anim, 5000);                 // festive dance ~5s
    lv_label_set_text(mile_toast_lbl, mile_label);
    lv_obj_clear_flag(mile_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mile_toast);
    mile_toast_hide_at = lv_tick_get() + 5000;
    // start/refresh the proud window: random 30–60 min
    mile_proud_until = lv_tick_get() + (uint32_t)random(1800000, 3600001);
}

static void mile_badge_tap_cb(lv_event_t* e) {
    LV_UNUSED(e);
    if (mile_label[0]) mile_burst();   // replay celebration on demand
}

void ui_show_milestone(const Milestone* m) {
    if (!m) return;
    mile_ensure();
    strlcpy(mile_label, m->label, sizeof(mile_label));
    strlcpy(mile_anim, m->anim[0] ? m->anim : "dance bounce", sizeof(mile_anim));
    lv_obj_add_event_cb(mile_badge, mile_badge_tap_cb, LV_EVENT_CLICKED, nullptr);
    lv_label_set_text(mile_badge_lbl, LV_SYMBOL_OK);
    lv_obj_clear_flag(mile_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mile_badge);
    mile_burst();
}

void ui_milestone_tick(void) {
    uint32_t now = lv_tick_get();
    if (mile_toast_hide_at != 0 && now >= mile_toast_hide_at) {
        if (mile_toast) lv_obj_add_flag(mile_toast, LV_OBJ_FLAG_HIDDEN);
        mile_toast_hide_at = 0;
        // After the burst, hold a calm-happy proud expression for the window.
        if (mile_proud_until > now) splash_pin_anim_for("dance sway", mile_proud_until - now);
    }
    if (mile_proud_until != 0 && now >= mile_proud_until) {
        mile_proud_until = 0;
        if (mile_badge) lv_obj_add_flag(mile_badge, LV_OBJ_FLAG_HIDDEN);  // badge decays
    }
}
```

Note: `lv_obj_add_event_cb` for the badge is attached in `ui_show_milestone` each
time — to avoid stacking duplicate callbacks, attach it once. Move the
`lv_obj_add_event_cb(mile_badge, mile_badge_tap_cb, ...)` line into `mile_ensure()`
(right after the badge is created) and remove it from `ui_show_milestone`.

- [ ] **Step 3: Call ui_milestone_tick from the main loop**

In `firmware/src/main.cpp`, next to `ui_banner_tick(); ui_approval_tick();`, add:

```cpp
    ui_milestone_tick();
```

- [ ] **Step 4: Compile all three environments**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter && /usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -3`
Expected: SUCCESS
Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_216 2>&1 | tail -3`
Expected: SUCCESS
Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_18 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 5: Commit**

```bash
git add firmware/src/ui.h firmware/src/ui.cpp firmware/src/main.cpp
git commit -m "feat(firmware): milestone celebration — burst + proud state + replay badge

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 7: Physical QA + docs

**Files:**
- Modify: `firmware/src/main.cpp` (temporary demo, reverted), `README.md`

- [ ] **Step 1: Temporarily force a milestone for QA**

In `firmware/src/main.cpp` `setup()`, after `ui_show_screen(SCREEN_SPLASH);`:

```cpp
    // TEMP QA: demo milestone — REMOVE before commit
    static Milestone demo_m = {"🔥 7 días seguidos", "dance bounce", true};
    ui_show_milestone(&demo_m);
```

- [ ] **Step 2: Flash and verify**

Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 -t upload --upload-port /dev/cu.usbmodem1101`
On the device verify: festive burst (~5s dance + purple toast with the label),
then the toast hides and Clawdio holds a calm sway with a round badge bottom-left.
Tap the badge → the burst replays. (The 30–60 min proud window is long; confirm the
badge persists and replay works — don't wait the full window.)

- [ ] **Step 3: Remove the demo + rebuild**

Delete the two demo lines. Rebuild:
Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 4: Document in README**

Append to the README's session-notifications area:

````markdown
## Clawdio milestones & reactions (macOS)

Clawdio reacts to your work and celebrates milestones — no decay, death, or
penalties. The daemon counts milestones (day streaks, completed tasks, usage,
git commits) in `~/.config/claude-usage-monitor/clawdio-state.json` and sends a
celebration to the device when one unlocks: a festive burst + toast, then Clawdio
holds a proud state for 30–60 min with a trophy badge in the corner. **Tap the
badge to replay the celebration.** Reactions: a small dance when a task finishes,
a happy wink when you approve from the device, and surprise on a risky command.

This reuses the existing notification hooks (`Notification`/`Stop`/`PostToolUse`);
commit milestones need no extra setup beyond those hooks being installed.
````

- [ ] **Step 5: Commit**

```bash
git add README.md
git commit -m "docs: document Clawdio milestones & reactions (Phase A)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-Review Notes

- **Spec coverage (Phase A):** commit plumbing (Task 1); milestone engine streaks/
  tasks/usage/commits + unlock-once (Task 2); persistence + feed + send + danger
  flag (Task 3); timed pin + struct + routing (Task 4); reactions done→festejo,
  approve→contento, danger→surprise, activity→mood (Task 5); burst + proud 30–60min
  + tappable replay badge (Task 6); QA + docs (Task 7). Trophy gallery + navigation
  are Phase B (out of scope here).
- **Contract consistency:** `{"ev":"milestone","id","label","anim"}` matches across
  engine, daemon send, and firmware `parse_milestone_json`. `danger` flag added to
  the `ask` payload. `splash_pin_anim_for(name, ms)`, `ui_show_milestone(const Milestone*)`,
  `ui_milestone_tick()` consistent across headers/impl/main.
- **No placeholders:** every step has concrete code/commands.
- **Note:** the proud-state animation (`dance sway`) and toast color are tunable in
  QA; `random(min,max)` is Arduino's PRNG (seeded enough for a cosmetic duration).
