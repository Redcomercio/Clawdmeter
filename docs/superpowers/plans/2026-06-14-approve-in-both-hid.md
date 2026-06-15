# Approve in Both (HID) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the tool-permission prompt show in the terminal AND mirror on the device, where the device's approve button types `1` into the focused terminal over BLE-HID.

**Architecture:** The PreToolUse hook becomes non-blocking — it writes a request and returns `ask` immediately, so the terminal prompt always shows. The daemon mirrors the request as a card on the device and clears it on a button decision, an activity/done event, or a 60 s timeout. The firmware's approve button types HID `1` and self-hides; the daemon can also hide it via a new `clear-ask` payload.

**Tech Stack:** POSIX shell (hook), Python 3 + asyncio/bleak (daemon, `daemon/.venv2`), C++ / Arduino / LVGL 9 (firmware C6).

**Spec:** `docs/superpowers/specs/2026-06-14-approve-in-both-hid-design.md`

---

## File Structure

- `daemon/clawdmeter-approve-hook.sh` (modify) — drop the `.res` polling; write `.req` and return `ask` immediately.
- `daemon/tests/test_approve_hook.py` (modify) — replace the blocking tests with non-blocking ones.
- `daemon/approval_broker.py` (modify) — `decide()` no longer writes `.res`; add `clear_current()`.
- `daemon/claude_usage_daemon.py` (modify) — send `clear-ask` on TX decision / activity-done / 60 s timeout.
- `daemon/tests/test_approval_broker.py` (modify) — update for the no-`.res` behavior + `clear_current`.
- `firmware/src/ui.cpp` (modify) — approve types HID `1`; handle `clear-ask`; keep the green flash.
- `firmware/src/ui.h` (modify) — declare `ui_hide_approval()`.
- `firmware/src/main.cpp` (modify) — route `{"ev":"clear-ask"}`.

---

## Task 1: Non-blocking hook

**Files:**
- Modify: `daemon/clawdmeter-approve-hook.sh`
- Modify: `daemon/tests/test_approve_hook.py`

The hook must write the request and return `ask` immediately — no `.res` polling, no
timeout loop. It always returns `ask` (the decision now happens in the terminal).

- [ ] **Step 1: Replace the tests with non-blocking expectations**

Replace the entire contents of `daemon/tests/test_approve_hook.py` with:

```python
import json, subprocess, time
from pathlib import Path

HOOK = Path(__file__).resolve().parents[1] / "clawdmeter-approve-hook.sh"


def run_hook(stdin_obj, cfg_dir, env_extra=None):
    env = {"CLAWDMETER_CONFIG_DIR": str(cfg_dir), "PATH": "/usr/bin:/bin"}
    if env_extra:
        env.update(env_extra)
    return subprocess.run(["bash", str(HOOK)], input=json.dumps(stdin_obj),
                          capture_output=True, text=True, env=env)


def reqs(cfg_dir):
    d = cfg_dir / "approve"
    return list(d.glob("*.req")) if d.exists() else []


def test_no_device_ready_returns_ask_no_req(tmp_path):
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert reqs(tmp_path) == []


def test_action_tool_writes_req_and_returns_ask_immediately(tmp_path):
    (tmp_path / "device-ready").write_text("")
    start = time.time()
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "git push"}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert time.time() - start < 3                 # non-blocking
    rs = reqs(tmp_path)
    assert len(rs) == 1
    body = json.loads(rs[0].read_text())
    assert body["tool"] == "Bash" and body["proj"] == "proj"


def test_non_action_tool_returns_ask_no_req(tmp_path):
    (tmp_path / "device-ready").write_text("")
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "AskUserQuestion",
                  "tool_input": {}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert reqs(tmp_path) == []


def test_stale_device_ready_returns_ask_no_req(tmp_path):
    ready = tmp_path / "device-ready"; ready.write_text("")
    import os
    old = time.time() - 30
    os.utime(ready, (old, old))
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert reqs(tmp_path) == []
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_approve_hook.py -v`
Expected: FAIL — the current hook blocks/polls and never writes a `.req` without a `.res`.

- [ ] **Step 3: Rewrite the hook non-blocking**

Replace the section of `daemon/clawdmeter-approve-hook.sh` from the line
`id="${sid}-$(date +%s)-$$"` to the end of the file with:

```bash
id="${sid}-$(date +%s)-$$"

mkdir -p "$appdir" 2>/dev/null
printf '{"id":"%s","sid":"%s","proj":"%s","tool":"%s","cmd":"%s"}\n' \
    "$id" "$sid" "$proj" "$tool" "${cmd//\"/\'}" > "$appdir/$id.req" 2>/dev/null

# Non-blocking: the terminal prompt shows now; the device card just mirrors it and
# (on approve) types the answer via HID. The hook never decides — always "ask".
emit_ask
```

Also remove the now-unused `emit_allow` definition and the `timeout=...` variable
near the top (lines defining `timeout` and `emit_allow`). Keep `emit_ask`, the
`device-ready` gate, the payload extraction, and the action-tool allowlist guard.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_approve_hook.py -v`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add daemon/clawdmeter-approve-hook.sh daemon/tests/test_approve_hook.py
git commit -m "feat(daemon): non-blocking approve hook (write req, ask immediately)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2: Broker clear + daemon clear-ask

**Files:**
- Modify: `daemon/approval_broker.py`
- Modify: `daemon/claude_usage_daemon.py`
- Modify: `daemon/tests/test_approval_broker.py`

`decide()` stops writing `.res` (the hook no longer reads it); add `clear_current()`
for terminal-answered/timeout clears. The daemon sends `{"ev":"clear-ask"}` when it
clears a card it had shown (so a card the device didn't dismiss itself goes away).

- [ ] **Step 1: Update the broker tests**

In `daemon/tests/test_approval_broker.py`, replace `test_decision_writes_res_and_advances`
with:

```python
def test_decide_drops_request_no_res(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a"); write_req(appdir, "b")
    b.scan()  # head = a
    b.decide("a", "approve")
    assert not (appdir / "a.res").exists()       # no .res written anymore
    assert not (appdir / "a.req").exists()        # request dropped
    nxt = b.scan()
    assert nxt["id"] == "b"


def test_clear_current_drops_head(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a")
    b.scan()
    cleared = b.clear_current()
    assert cleared == "a"
    assert not (appdir / "a.req").exists()
    assert b.current_id() is None
    assert b.clear_current() is None              # nothing left
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_approval_broker.py -v`
Expected: FAIL — `decide` still writes `.res`; `clear_current` undefined.

- [ ] **Step 3: Update the broker**

In `daemon/approval_broker.py`, replace `decide()` with a no-`.res` version and add
`clear_current()`:

```python
    def decide(self, rid: str, decision: str) -> None:
        """Drop a decided request (the device self-hides; we just advance)."""
        (self.appdir / f"{rid}.req").unlink(missing_ok=True)
        if rid in self._queue:
            self._queue.remove(rid)
        if self._head_sent == rid:
            self._head_sent = None

    def clear_current(self) -> str | None:
        """Drop the current head (terminal-answered/timeout). Returns its id or None."""
        head = self.current_id()
        if head is None:
            return None
        (self.appdir / f"{head}.req").unlink(missing_ok=True)
        self._queue.remove(head)
        if self._head_sent == head:
            self._head_sent = None
        return head
```

- [ ] **Step 4: Run broker tests**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_approval_broker.py -v`
Expected: PASS.

- [ ] **Step 5: Wire clear-ask into the daemon**

In `daemon/claude_usage_daemon.py`:

(a) In `run_broker`, track when the head was sent and time it out at 60 s, sending
`clear-ask`. Replace the body of `run_broker`'s loop so it looks like:

```python
async def run_broker(session: "Session", broker: ApprovalBroker,
                     stop_event: asyncio.Event) -> None:
    """Scan the approve dir; send the head request; expire stale cards (60s)."""
    sent_at = {"t": 0.0, "id": None}
    while not stop_event.is_set() and session.client.is_connected:
        try:
            payload = broker.scan()
            if payload is not None:
                await session.write_payload(payload)
                sent_at = {"t": time.time(), "id": payload["id"]}
            elif sent_at["id"] and broker.current_id() == sent_at["id"] \
                    and time.time() - sent_at["t"] > 60:
                if broker.clear_current():
                    await session.write_payload({"ev": "clear-ask"})
                sent_at = {"t": 0.0, "id": None}
            elif broker.current_id() is None:
                sent_at = {"t": 0.0, "id": None}
        except (OSError, BleakError) as e:
            log(f"Broker error: {e}")
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=APPROVE_TICK)
        except asyncio.TimeoutError:
            pass
```

(b) Clear the card when the session resolves the prompt in the terminal — in
`watch_events`, after feeding the tracker/engine for an event, add: if the event is
`activity` or `done`, clear any current card. Add inside the `for line ...` loop,
after the existing per-event handling:

```python
                    if obj.get("ev") in ("activity", "done"):
                        if session._broker and session._broker.clear_current():
                            await session.write_payload({"ev": "clear-ask"})
```

(c) The TX decision path already calls `broker.decide()` in `Session._on_tx`; that
now just drops the request (device self-hid). No change needed there beyond Task 2's
`decide()`.

- [ ] **Step 6: Verify import + tests**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/python -c "import claude_usage_daemon; print('import ok')"`
Expected: `import ok`

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_approval_broker.py tests/test_approve_hook.py -q`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add daemon/approval_broker.py daemon/claude_usage_daemon.py daemon/tests/test_approval_broker.py
git commit -m "feat(daemon): clear-ask on decision/activity/timeout; broker no longer writes .res

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3: Firmware — type HID '1' on approve + handle clear-ask

**Files:**
- Modify: `firmware/src/ui.h`, `firmware/src/ui.cpp`, `firmware/src/main.cpp`

- [ ] **Step 1: Declare ui_hide_approval in ui.h**

In `firmware/src/ui.h`, after the approval declarations:

```c
void ui_hide_approval(void);   // daemon-initiated clear (clear-ask payload)
```

- [ ] **Step 2: Type HID '1' on approve in card_finish**

In `firmware/src/ui.cpp`, `card_finish()` currently calls `ble_send_decision(card_id, decision)`
then `confirm_flash(decision)`. Change the approve path to also type `1` into the
focused terminal. Replace the body of `card_finish` with:

```cpp
static void card_finish(const char* decision) {
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = 0;
    card_visible = false;
    imu_hal_lock_rotation(false);
    if (strcmp(decision, "approve") == 0) {
        // Type "1" (approve once) into whatever terminal has OS focus.
        ble_keyboard_press(0x1E, 0);   // HID usage 0x1E = '1'
        ble_keyboard_release();
    }
    if (card_id[0]) {
        ble_send_decision(card_id, decision);   // tells the daemon to advance
        card_id[0] = '\0';
    }
    splash_unpin_anim();
    confirm_flash(decision);   // green "Aprobado" = keystroke-sent feedback
}
```

- [ ] **Step 3: Implement ui_hide_approval**

In `firmware/src/ui.cpp`, add near `ui_approval_tick`:

```cpp
// Daemon-initiated clear (prompt resolved in the terminal, or timed out).
void ui_hide_approval(void) {
    if (!card_visible) return;
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = 0;
    card_visible = false;
    card_id[0] = '\0';
    imu_hal_lock_rotation(false);
    splash_unpin_anim();
}
```

- [ ] **Step 4: Route clear-ask in main.cpp**

In `firmware/src/main.cpp`'s BLE routing chain, add a branch before the generic
`"ev"` branch (alongside `ask`/`milestone`):

```cpp
        } else if (strstr(raw, "\"clear-ask\"") != nullptr) {
            ui_hide_approval();
            ble_send_ack();
```

- [ ] **Step 5: Compile all three environments**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter && /usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -3`
Expected: SUCCESS
Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_216 2>&1 | tail -3`
Expected: SUCCESS
Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_18 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 6: Commit**

```bash
git add firmware/src/ui.h firmware/src/ui.cpp firmware/src/main.cpp
git commit -m "feat(firmware): approve types HID '1' to terminal + clear-ask handling

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4: End-to-end QA (real prompt) + docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Flash + reactivate the pipeline**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter && /usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 -t upload --upload-port /dev/cu.usbmodem1101`
Ensure the daemon is running (`launchctl kickstart -k gui/$(id -u)/com.user.claude-usage-daemon`)
and the `PreToolUse` hook is registered with the scoped matcher.

- [ ] **Step 2: Verify in a fresh, focused terminal session**

In a NEW Claude Code session in a focused terminal, trigger a tool permission (e.g.,
ask it to run a Bash command). Verify: the permission prompt appears in the terminal
AND the card appears on the device. Pressing **Continuar** types `1` → the prompt is
answered "Yes" and the tool runs; the green "Aprobado" flash shows; the card clears.
Pressing **Terminal** hides the card and you answer in the terminal (the card also
clears via the activity/done event). Confirm the terminal had focus.

- [ ] **Step 3: Document in README**

Update the "Approve from the device" section: the prompt now shows in the terminal
*and* on the device; pressing Continuar types the approval into the focused terminal
over BLE-HID (the terminal must be the active window); Terminal just dismisses the
device card. Note the ~95% reliability and focus requirement.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: approve-in-both — device types the answer via BLE-HID

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-Review Notes

- **Spec coverage:** non-blocking hook (Task 1); broker no-`.res` + `clear_current` +
  daemon `clear-ask` on decision/activity-done/timeout (Task 2); firmware HID `1` on
  approve + green flash + `clear-ask` handling + Terminal dismiss (Task 3); E2E + docs
  (Task 4). Focus/racing/95% limitations documented in spec + README.
- **Contract consistency:** `{"ev":"ask",...}` (daemon→device, unchanged), `{"id","d":"approve"|"dismiss"}`
  (device→daemon TX, now only advances the queue), `{"ev":"clear-ask"}` (daemon→device,
  new). `ble_keyboard_press(0x1E,0)`/`ble_keyboard_release()` reuse the existing HID.
  `ui_hide_approval()`/routing consistent across header/impl/main.
- **No placeholders:** every step has concrete code/commands.
- **Note:** if the host registers the HID `1` unreliably back-to-back, QA may add a
  brief delay between press and release; flagged in Task 4.
```
