# Swipe-to-Approve Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Approve Claude Code tool-permission requests from the Clawdmeter device with a Tinder-style swipe; left-swipe/timeout/disconnect defers to the normal terminal prompt (never denies).

**Architecture:** A blocking `PreToolUse` hook writes a request file and polls for a decision, but only when a `device-ready` flag is fresh (device connected). The macOS daemon's `ApprovalBroker` queues requests, sends the head over BLE RX (`{"ev":"ask",…}`), receives the swipe decision over BLE TX, and writes the decision file the hook is waiting on. The firmware shows a draggable swipe card and notifies the decision over the TX characteristic.

**Tech Stack:** POSIX shell (hook), Python 3 + asyncio + bleak (daemon), C++ / Arduino / LVGL 9 (firmware).

**Spec:** `docs/superpowers/specs/2026-06-12-swipe-to-approve-design.md`

---

## File Structure

- `daemon/clawdmeter-approve-hook.sh` (create) — PreToolUse hook: device-ready gate, write request, poll for decision, emit allow/ask.
- `daemon/approval_broker.py` (create) — `ApprovalBroker`: pure queue + req/res file plumbing, no BLE/asyncio.
- `daemon/claude_usage_daemon.py` (modify) — `device-ready` flag, TX subscription, run the broker inside `connect_and_run`.
- `daemon/tests/test_approve_hook.py` (create) — hook behavior tests.
- `daemon/tests/test_approval_broker.py` (create) — broker logic tests.
- `firmware/src/data.h` (modify) — `ApprovalRequest` struct.
- `firmware/src/ble.h` / `ble.cpp` (modify) — `ble_send_decision()` over TX.
- `firmware/src/main.cpp` (modify) — route `{"ev":"ask",…}` to the card.
- `firmware/src/ui.h` / `ui.cpp` (modify) — swipe card overlay + drag gesture + decision callback.
- `README.md` (modify) — document the PreToolUse hook install.

Shared directory: `~/.config/claude-usage-monitor/approve/` holds `<id>.req` / `<id>.res`.

---

## Task 1: PreToolUse hook script

**Files:**
- Create: `daemon/clawdmeter-approve-hook.sh`
- Test: `daemon/tests/test_approve_hook.py`

- [ ] **Step 1: Write the failing test**

```python
# daemon/tests/test_approve_hook.py
import json, subprocess, threading, time
from pathlib import Path

HOOK = Path(__file__).resolve().parents[1] / "clawdmeter-approve-hook.sh"


def run_hook(stdin_obj, cfg_dir, env_extra=None):
    env = {"CLAWDMETER_CONFIG_DIR": str(cfg_dir), "PATH": "/usr/bin:/bin"}
    if env_extra:
        env.update(env_extra)
    return subprocess.run(["bash", str(HOOK)], input=json.dumps(stdin_obj),
                          capture_output=True, text=True, env=env)


def test_no_device_ready_returns_ask(tmp_path):
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path)
    out = json.loads(r.stdout)
    assert out["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert r.returncode == 0


def test_stale_device_ready_returns_ask(tmp_path):
    ready = tmp_path / "device-ready"
    ready.write_text("")
    # backdate 30s so it's stale (>10s)
    import os
    old = time.time() - 30
    os.utime(ready, (old, old))
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"


def test_approve_decision_returns_allow(tmp_path):
    (tmp_path / "device-ready").write_text("")
    (tmp_path / "approve").mkdir()

    def responder():
        # wait for the req file, then drop an approve res
        for _ in range(100):
            reqs = list((tmp_path / "approve").glob("*.req"))
            if reqs:
                rid = reqs[0].stem
                (tmp_path / "approve" / f"{rid}.res").write_text('{"d":"approve"}')
                return
            time.sleep(0.05)

    t = threading.Thread(target=responder); t.start()
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "git push"}}, tmp_path)
    t.join()
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "allow"


def test_dismiss_decision_returns_ask(tmp_path):
    (tmp_path / "device-ready").write_text("")
    (tmp_path / "approve").mkdir()

    def responder():
        for _ in range(100):
            reqs = list((tmp_path / "approve").glob("*.req"))
            if reqs:
                (tmp_path / "approve" / f"{reqs[0].stem}.res").write_text('{"d":"dismiss"}')
                return
            time.sleep(0.05)

    t = threading.Thread(target=responder); t.start()
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "rm x"}}, tmp_path)
    t.join()
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"


def test_timeout_returns_ask(tmp_path):
    (tmp_path / "device-ready").write_text("")
    (tmp_path / "approve").mkdir()
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path,
                 env_extra={"CLAWDMETER_APPROVE_TIMEOUT": "1"})
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_approve_hook.py -v`
Expected: FAIL — script does not exist.

- [ ] **Step 3: Write the hook script**

```bash
# daemon/clawdmeter-approve-hook.sh
#!/usr/bin/env bash
# Clawdmeter PreToolUse hook. Approves a tool from the device via swipe.
# - Only engages when the device is connected (fresh device-ready flag);
#   otherwise returns "ask" instantly so normal terminal prompts are unaffected.
# - Writes a request file the daemon picks up, polls for a decision file.
# - approve -> "allow"; dismiss/timeout/no-device -> "ask" (never "deny").
# Always exits 0.

cfg="${CLAWDMETER_CONFIG_DIR:-$HOME/.config/claude-usage-monitor}"
ready="$cfg/device-ready"
appdir="$cfg/approve"
fresh_secs=10
timeout="${CLAWDMETER_APPROVE_TIMEOUT:-30}"

emit_ask()   { printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"ask"}}\n'; exit 0; }
emit_allow() { printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"allow"}}\n'; exit 0; }

# Gate: device must be connected (flag present and fresh).
[ -f "$ready" ] || emit_ask
now=$(date +%s)
mtime=$(stat -f %m "$ready" 2>/dev/null || stat -c %Y "$ready" 2>/dev/null || echo 0)
[ $(( now - mtime )) -le "$fresh_secs" ] || emit_ask

payload="$(cat)"
if command -v jq >/dev/null 2>&1; then
    sid=$(printf '%s' "$payload"  | jq -r '.session_id // ""')
    cwd=$(printf '%s' "$payload"  | jq -r '.cwd // ""')
    tool=$(printf '%s' "$payload" | jq -r '.tool_name // ""')
    cmd=$(printf '%s' "$payload"  | jq -r '(.tool_input.command // .tool_input.file_path // "") | tostring' | cut -c1-80)
else
    sid=$(printf '%s' "$payload"  | grep -o '"session_id"[^,]*' | head -1 | sed 's/.*:"//;s/"//')
    cwd=$(printf '%s' "$payload"  | grep -o '"cwd"[^,]*'        | head -1 | sed 's/.*:"//;s/"//')
    tool=$(printf '%s' "$payload" | grep -o '"tool_name"[^,]*'  | head -1 | sed 's/.*:"//;s/"//')
    cmd=""
fi
proj=$(basename "$cwd" 2>/dev/null); [ -z "$proj" ] && proj="?"
id="${sid}-$(date +%s%N)"

mkdir -p "$appdir" 2>/dev/null
req="$appdir/$id.req"
res="$appdir/$id.res"
printf '{"id":"%s","sid":"%s","proj":"%s","tool":"%s","cmd":"%s"}\n' \
    "$id" "$sid" "$proj" "$tool" "${cmd//\"/\'}" > "$req"

# Poll for the decision; clean up on every exit path.
trap 'rm -f "$req" "$res"' EXIT
start=$now
while :; do
    if [ -f "$res" ]; then
        d=$(cat "$res")
        case "$d" in
            *'"approve"'*) emit_allow ;;
            *) emit_ask ;;
        esac
    fi
    [ $(( $(date +%s) - start )) -ge "$timeout" ] && emit_ask
    sleep 0.3
done
```

- [ ] **Step 4: Make executable and run tests**

Run: `chmod +x daemon/clawdmeter-approve-hook.sh && cd daemon && .venv2/bin/pytest tests/test_approve_hook.py -v`
Expected: PASS (5 tests). (`test_timeout` runs ~1s due to the override.)

- [ ] **Step 5: Commit**

```bash
git add daemon/clawdmeter-approve-hook.sh daemon/tests/test_approve_hook.py
git commit -m "feat(daemon): PreToolUse swipe-approve hook (device-gated, ask fallback)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2: ApprovalBroker (queue + req/res plumbing)

**Files:**
- Create: `daemon/approval_broker.py`
- Test: `daemon/tests/test_approval_broker.py`

The broker is pure logic + filesystem: it scans for `<id>.req` files, queues
them, exposes the head request to send to the device, and on a decision writes
`<id>.res` and advances. No BLE/asyncio — the daemon drives it by calling methods.

- [ ] **Step 1: Write the failing test**

```python
# daemon/tests/test_approval_broker.py
import json
from pathlib import Path
from approval_broker import ApprovalBroker


def write_req(appdir, rid, proj="P", tool="Bash", cmd="ls"):
    (appdir / f"{rid}.req").write_text(json.dumps(
        {"id": rid, "sid": "s", "proj": proj, "tool": tool, "cmd": cmd}))


def test_scan_picks_up_request_and_builds_ask(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a")
    sendable = b.scan()
    assert sendable == {"ev": "ask", "id": "a", "proj": "P", "tool": "Bash",
                        "cmd": "ls", "pos": 1, "total": 1}


def test_only_head_is_sent_with_queue_positions(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a", proj="A")
    write_req(appdir, "b", proj="B")
    first = b.scan()
    assert first["id"] == "a" and first["total"] == 2 and first["pos"] == 1
    # scanning again without resolving keeps the same head (no duplicate send)
    assert b.scan() is None


def test_decision_writes_res_and_advances(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a"); write_req(appdir, "b")
    b.scan()  # head = a
    b.decide("a", "approve")
    assert json.loads((appdir / "a.res").read_text()) == {"d": "approve"}
    # next scan promotes b
    nxt = b.scan()
    assert nxt["id"] == "b" and nxt["pos"] == 1 and nxt["total"] == 1


def test_orphan_request_dropped_when_req_file_vanishes(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a")
    b.scan()
    (appdir / "a.req").unlink()  # hook gave up
    # scan notices the head's req is gone and clears it
    assert b.scan() is None
    assert b.current_id() is None


def test_decision_for_unknown_id_is_ignored(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    b.decide("ghost", "approve")  # no crash, no file
    assert not (appdir / "ghost.res").exists()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd daemon && .venv2/bin/pytest tests/test_approval_broker.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'approval_broker'`.

- [ ] **Step 3: Implement the broker**

```python
# daemon/approval_broker.py
"""Queue + request/response file plumbing for swipe-to-approve.

Pure of BLE/asyncio: the daemon scans periodically, sends scan()'s result to
the device, and calls decide() when a swipe arrives over BLE.
"""
import json
from pathlib import Path


class ApprovalBroker:
    def __init__(self, appdir: Path) -> None:
        self.appdir = Path(appdir)
        self._queue: list[str] = []   # ids in arrival order
        self._head_sent: str | None = None

    def _refresh_queue(self) -> None:
        self.appdir.mkdir(parents=True, exist_ok=True)
        present = {p.stem for p in self.appdir.glob("*.req")}
        # keep existing order, drop vanished, append new (sorted for determinism)
        self._queue = [i for i in self._queue if i in present]
        for i in sorted(present):
            if i not in self._queue:
                self._queue.append(i)
        if self._head_sent and self._head_sent not in present:
            self._head_sent = None  # orphaned

    def current_id(self) -> str | None:
        return self._queue[0] if self._queue else None

    def scan(self) -> dict | None:
        """Return the BLE payload for the head request if it hasn't been sent
        yet, else None. Call every tick."""
        self._refresh_queue()
        head = self.current_id()
        if head is None or head == self._head_sent:
            return None
        try:
            req = json.loads((self.appdir / f"{head}.req").read_text())
        except (OSError, json.JSONDecodeError):
            return None
        self._head_sent = head
        return {"ev": "ask", "id": head, "proj": req.get("proj", "?"),
                "tool": req.get("tool", ""), "cmd": req.get("cmd", ""),
                "pos": 1, "total": len(self._queue)}

    def decide(self, rid: str, decision: str) -> None:
        """Record a swipe decision: write <id>.res and drop it from the queue."""
        if not (self.appdir / f"{rid}.req").exists():
            return
        (self.appdir / f"{rid}.res").write_text(json.dumps({"d": decision}))
        if rid in self._queue:
            self._queue.remove(rid)
        if self._head_sent == rid:
            self._head_sent = None
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd daemon && .venv2/bin/pytest tests/test_approval_broker.py -v`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add daemon/approval_broker.py daemon/tests/test_approval_broker.py
git commit -m "feat(daemon): ApprovalBroker — request queue + decision plumbing

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3: Daemon — device-ready flag, TX subscription, run broker

**Files:**
- Modify: `daemon/claude_usage_daemon.py`

Context: `Session` has `.client` and async `write_payload(dict)`. `connect_and_run`
already runs `poll_loop()` and `watch_events()` via `asyncio.wait`, and takes
`(target, stop_event, tracker, watch_pos)`. `RX_CHAR_UUID` and `REQ_CHAR_UUID`
are defined near line 26; `EVENT_FILE` near line 38.

- [ ] **Step 1: Add constants**

After the `REQ_CHAR_UUID` definition (~line 27), add:

```python
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"  # device -> host notify
```

After `EVENT_FILE`/`EVENT_TICK` (~line 39), add:

```python
DEVICE_READY_FILE = Path.home() / ".config" / "claude-usage-monitor" / "device-ready"
APPROVE_DIR = Path.home() / ".config" / "claude-usage-monitor" / "approve"
APPROVE_TICK = 0.3  # seconds between approve-dir scans
```

- [ ] **Step 2: Add the broker-driver coroutine and TX decision handler**

Add an import near the top (with the other imports):

```python
from approval_broker import ApprovalBroker
```

Add at module level (near `watch_events`):

```python
def touch_device_ready() -> None:
    try:
        DEVICE_READY_FILE.parent.mkdir(parents=True, exist_ok=True)
        DEVICE_READY_FILE.touch()
    except OSError as e:
        log(f"device-ready touch failed: {e}")


def clear_device_ready() -> None:
    DEVICE_READY_FILE.unlink(missing_ok=True)


async def run_broker(session: "Session", broker: ApprovalBroker,
                     stop_event: asyncio.Event) -> None:
    """Scan the approve dir; send the head request to the device over RX."""
    while not stop_event.is_set() and session.client.is_connected:
        try:
            payload = broker.scan()
            if payload is not None:
                await session.write_payload(payload)
        except (OSError, BleakError) as e:
            log(f"Broker error: {e}")
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=APPROVE_TICK)
        except asyncio.TimeoutError:
            pass
```

- [ ] **Step 3: Subscribe to TX and route decisions in Session**

In `class Session.__init__`, store a broker ref placeholder; add a TX handler.
Replace the `Session` class's `setup_refresh_subscription` area by adding:

```python
    def attach_broker(self, broker) -> None:
        self._broker = broker

    def _on_tx(self, _char, data: bytearray) -> None:
        try:
            msg = json.loads(bytes(data).decode())
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        rid, d = msg.get("id"), msg.get("d")
        if rid and d in ("approve", "dismiss") and getattr(self, "_broker", None):
            log(f"Swipe decision {d} for {rid}")
            self._broker.decide(rid, d)

    async def setup_tx_subscription(self) -> None:
        try:
            await self.client.start_notify(TX_CHAR_UUID, self._on_tx)
        except (BleakError, ValueError) as e:
            log(f"TX subscription unavailable: {e}")
```

- [ ] **Step 4: Wire broker + device-ready into connect_and_run**

In `connect_and_run`, right after `await session.setup_refresh_subscription()`,
add:

```python
    broker = ApprovalBroker(APPROVE_DIR)
    session.attach_broker(broker)
    await session.setup_tx_subscription()
    touch_device_ready()
```

Add `touch_device_ready()` inside `poll_loop()` right after a successful write
(so the flag refreshes every poll tick). Find the line `used["ok"] = True` and
add immediately after it:

```python
                        touch_device_ready()
```

Add `run_broker` to the gathered tasks. Find:

```python
    poll_task = asyncio.create_task(poll_loop())
    watch_task = asyncio.create_task(
        watch_events(session, tracker, watch_pos, stop_event))
    try:
        done, pending = await asyncio.wait(
            {poll_task, watch_task}, return_when=asyncio.FIRST_COMPLETED)
```

Replace with:

```python
    poll_task = asyncio.create_task(poll_loop())
    watch_task = asyncio.create_task(
        watch_events(session, tracker, watch_pos, stop_event))
    broker_task = asyncio.create_task(run_broker(session, broker, stop_event))
    try:
        done, pending = await asyncio.wait(
            {poll_task, watch_task, broker_task}, return_when=asyncio.FIRST_COMPLETED)
```

In the `finally:` block of `connect_and_run` (where `client.disconnect()` runs),
add before the disconnect:

```python
        clear_device_ready()
```

- [ ] **Step 5: Verify import and no regressions**

Run: `cd daemon && .venv2/bin/python -c "import claude_usage_daemon; print('import ok')"`
Expected: `import ok`

Run: `cd daemon && .venv2/bin/pytest tests/test_event_tracker.py tests/test_hook_script.py tests/test_approval_broker.py tests/test_approve_hook.py -q`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add daemon/claude_usage_daemon.py
git commit -m "feat(daemon): device-ready flag, TX decision subscription, broker loop

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4: Firmware — ApprovalRequest struct, TX decision sender, RX routing

**Files:**
- Modify: `firmware/src/data.h`, `firmware/src/ble.h`, `firmware/src/ble.cpp`, `firmware/src/main.cpp`

Note: this task ends with an expected compile FAILURE referencing `ui_show_approval`
(added in Task 5). That confirms wiring is in place.

- [ ] **Step 1: Add the ApprovalRequest struct**

In `firmware/src/data.h`, after `SessionEvent`:

```c
struct ApprovalRequest {
    char id[40];     // request id (session-epoch); echoed back on decision
    char proj[24];
    char tool[16];
    char cmd[64];
    uint8_t pos;     // 1-based position in the queue
    uint8_t total;   // queue length
    bool fresh;
};
```

- [ ] **Step 2: Declare and implement ble_send_decision**

In `firmware/src/ble.h`, after `void ble_send_nack(void);`:

```c
// Notify a swipe decision for an approval request over the TX characteristic.
// decision is "approve" or "dismiss".
void ble_send_decision(const char* id, const char* decision);
```

In `firmware/src/ble.cpp`, after `ble_send_nack()` (~line 262), add:

```cpp
void ble_send_decision(const char* id, const char* decision) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"d\":\"%s\"}", id, decision);
        tx_char->setValue(buf);
        tx_char->notify();
    }
}
```

- [ ] **Step 3: Route the ask payload in main.cpp**

In `firmware/src/main.cpp`, after `parse_event_json` (~line 131), add a parser:

```cpp
static ApprovalRequest approval_req = {};

// Parse an {"ev":"ask",...} payload into an ApprovalRequest.
static bool parse_approval_json(const char* json, ApprovalRequest* out) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    strlcpy(out->id,   doc["id"]   | "", sizeof(out->id));
    strlcpy(out->proj, doc["proj"] | "", sizeof(out->proj));
    strlcpy(out->tool, doc["tool"] | "", sizeof(out->tool));
    strlcpy(out->cmd,  doc["cmd"]  | "", sizeof(out->cmd));
    out->pos   = doc["pos"]   | 1;
    out->total = doc["total"] | 1;
    out->fresh = true;
    return true;
}
```

In the BLE routing block, the `"ev"` branch currently calls
`parse_event_json`/`ui_show_event`. Change it to distinguish `ask` from the
others. Replace:

```cpp
        if (strstr(raw, "\"ev\"") != nullptr) {
            if (parse_event_json(raw, &session_event)) {
                ui_show_event(&session_event);
                ble_send_ack();
            } else {
                ble_send_nack();
            }
        } else if (parse_json(raw, &usage)) {
```

with:

```cpp
        if (strstr(raw, "\"ask\"") != nullptr) {
            if (parse_approval_json(raw, &approval_req)) {
                ui_show_approval(&approval_req);
                ble_send_ack();
            } else {
                ble_send_nack();
            }
        } else if (strstr(raw, "\"ev\"") != nullptr) {
            if (parse_event_json(raw, &session_event)) {
                ui_show_event(&session_event);
                ble_send_ack();
            } else {
                ble_send_nack();
            }
        } else if (parse_json(raw, &usage)) {
```

- [ ] **Step 4: Compile-check (EXPECTED FAIL until Task 5)**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter && /usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -6`
Expected: FAIL — `'ui_show_approval' was not declared`. Confirms wiring; Task 5 resolves it.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/data.h firmware/src/ble.h firmware/src/ble.cpp firmware/src/main.cpp
git commit -m "feat(firmware): approval request struct, TX decision sender, ask routing

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 5: Firmware — swipe card UI with drag gesture

**Files:**
- Modify: `firmware/src/ui.h`, `firmware/src/ui.cpp`

Context: `ui.cpp` has the banner/cloud overlays on `lv_layer_top()`, `board_caps()`
(returns `const BoardCaps&` with `.width`/`.height`), Styrene fonts declared, and
`splash_pin_anim("expression surprise")`/`splash_unpin_anim()` from the prior
feature. LVGL 9. The card drag uses `LV_EVENT_PRESSING` (translate) +
`LV_EVENT_RELEASED` (commit/spring-back).

- [ ] **Step 1: Declare the UI entry point in ui.h**

In `firmware/src/ui.h`, after `void ui_show_event(const SessionEvent* ev);`:

```c
void ui_show_approval(const ApprovalRequest* req);
void ui_approval_tick(void);   // call each loop; handles the 30s auto-dismiss
```

- [ ] **Step 2: Implement the card in ui.cpp**

Add with the other overlay statics (near the banner/cloud block):

```cpp
// ---- Swipe-to-approve card (overlays any screen; commits a BLE decision) ----
static lv_obj_t* card = nullptr;
static lv_obj_t* card_proj = nullptr;
static lv_obj_t* card_tool = nullptr;
static lv_obj_t* card_cmd  = nullptr;
static lv_obj_t* card_pos  = nullptr;
static char      card_id[40] = {0};
static int32_t   card_drag_start_x = 0;
static int32_t   card_drag_dx = 0;
static uint32_t  card_hide_at = 0;   // lv_tick when the 30s timeout fires

static void card_finish(const char* decision) {
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = 0;
    if (card_id[0]) {
        ble_send_decision(card_id, decision);
        card_id[0] = '\0';
    }
    splash_unpin_anim();
}

static void card_press_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    const BoardCaps& c = board_caps();
    int32_t thresh = c.width * 4 / 10;   // ~40% of width commits

    if (code == LV_EVENT_PRESSED) {
        card_drag_start_x = p.x;
        card_drag_dx = 0;
    } else if (code == LV_EVENT_PRESSING) {
        card_drag_dx = p.x - card_drag_start_x;
        lv_obj_align(card, LV_ALIGN_CENTER, card_drag_dx, 0);
        // tint: green when dragging right, dim when left
        if (card_drag_dx > 0)
            lv_obj_set_style_bg_color(card, lv_color_hex(0x1E7B34), 0);
        else if (card_drag_dx < 0)
            lv_obj_set_style_bg_color(card, lv_color_hex(0x3a3a44), 0);
        else
            lv_obj_set_style_bg_color(card, lv_color_hex(0x202028), 0);
    } else if (code == LV_EVENT_RELEASED) {
        if (card_drag_dx >= thresh)       card_finish("approve");
        else if (card_drag_dx <= -thresh) card_finish("dismiss");
        else {  // spring back
            lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x202028), 0);
        }
    }
}

static void card_ensure(void) {
    if (card) return;
    const BoardCaps& c = board_caps();
    card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(card, c.width - 60, 200);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x202028), 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(card, card_press_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(card, card_press_cb, LV_EVENT_RELEASED, nullptr);

    card_proj = lv_label_create(card);
    lv_obj_set_style_text_font(card_proj, &font_styrene_28, 0);
    lv_obj_set_style_text_color(card_proj, lv_color_white(), 0);
    lv_obj_align(card_proj, LV_ALIGN_TOP_LEFT, 0, 0);

    card_tool = lv_label_create(card);
    lv_obj_set_style_text_font(card_tool, &font_styrene_20, 0);
    lv_obj_set_style_text_color(card_tool, lv_color_hex(0xB8860B), 0);
    lv_obj_align(card_tool, LV_ALIGN_TOP_LEFT, 0, 44);

    card_cmd = lv_label_create(card);
    lv_obj_set_style_text_font(card_cmd, &font_styrene_16, 0);
    lv_obj_set_style_text_color(card_cmd, lv_color_hex(0xcfcfd6), 0);
    lv_label_set_long_mode(card_cmd, LV_LABEL_LONG_DOT);
    lv_obj_set_width(card_cmd, c.width - 84);
    lv_obj_align(card_cmd, LV_ALIGN_TOP_LEFT, 0, 74);

    lv_obj_t* hints = lv_label_create(card);
    lv_obj_set_style_text_font(hints, &font_styrene_16, 0);
    lv_obj_set_style_text_color(hints, lv_color_hex(0x8a8a92), 0);
    lv_label_set_text(hints, LV_SYMBOL_LEFT " terminal      aprobar " LV_SYMBOL_OK);
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, 0);

    card_pos = lv_label_create(card);
    lv_obj_set_style_text_font(card_pos, &font_styrene_14, 0);
    lv_obj_set_style_text_color(card_pos, lv_color_hex(0x8a8a92), 0);
    lv_obj_align(card_pos, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_approval(const ApprovalRequest* req) {
    if (!req) return;
    card_ensure();
    strlcpy(card_id, req->id, sizeof(card_id));
    lv_label_set_text(card_proj, req->proj);
    lv_label_set_text(card_tool, req->tool);
    lv_label_set_text(card_cmd, req->cmd);
    char pos[16];
    snprintf(pos, sizeof(pos), "%u de %u", (unsigned)req->pos, (unsigned)req->total);
    lv_label_set_text(card_pos, pos);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x202028), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_hide_at = lv_tick_get() + 30000;  // auto-dismiss in 30s
    splash_pin_anim("expression surprise");
}

void ui_approval_tick(void) {
    if (card_hide_at != 0 && lv_tick_get() >= card_hide_at) {
        card_finish("dismiss");
    }
}
```

If `ble.h` is not already included in `ui.cpp`, add `#include "ble.h"` near the
top (it is needed for `ble_send_decision`).

- [ ] **Step 3: Call ui_approval_tick from the main loop**

In `firmware/src/main.cpp`, next to `ui_banner_tick();` (added in the prior
feature), add:

```cpp
    ui_approval_tick();
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
git commit -m "feat(firmware): swipe-to-approve card with drag gesture

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 6: Physical QA on the C6

**Files:** none (verification only)

- [ ] **Step 1: Temporarily force a card for QA**

In `firmware/src/main.cpp` `setup()`, after `ui_show_screen(SCREEN_SPLASH);`, add:

```cpp
    // TEMP QA: demo approval card — REMOVE before commit
    static ApprovalRequest demo = {"demo-1", "libredte-server", "Bash", "git push origin main", 1, 2, true};
    ui_show_approval(&demo);
```

- [ ] **Step 2: Flash and verify the gesture**

Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 -t upload --upload-port /dev/cu.usbmodem1101`
On the device, verify: card shows project/tool/command + `1 de 2` + hints; dragging
right tints green and commits past ~40%; dragging left dims and commits; a partial
drag springs back; after 30 s with no action the card auto-dismisses. Clawdio is
surprised while the card is up.

- [ ] **Step 3: Remove the demo code**

Delete the two temporary `demo` lines. Rebuild to confirm clean:
Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 4: Commit (only if QA required non-demo tweaks)**

```bash
git add firmware/src/ui.cpp
git commit -m "fix(firmware): swipe card tweaks from QA"
```

(Skip if only demo removal was needed.)

---

## Task 7: End-to-end test + hook install docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Register the PreToolUse hook and test live**

Add to `~/.claude/settings.json` `hooks` block a `PreToolUse` entry (alongside the
existing Notification/Stop/PostToolUse):

```json
    "PreToolUse": [
      { "matcher": "", "hooks": [
        { "type": "command", "timeout": 35,
          "command": "/Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon/clawdmeter-approve-hook.sh" } ] }
    ]
```

Restart the daemon (`daemon/.venv2/bin/python daemon/claude_usage_daemon.py`),
restart Claude Code so it reloads hooks, then in a fresh session trigger a tool
permission. Verify the swipe card appears and a right swipe runs the tool, a left
swipe falls back to the terminal prompt.

- [ ] **Step 2: Document in README**

Append to the README's session-notifications section:

````markdown
### Approve from the device (swipe-to-approve)

With the daemon connected, a `PreToolUse` hook lets you approve tool permissions
by swiping on the device. Add to `~/.claude/settings.json` (merge into `hooks`):

```json
{
  "hooks": {
    "PreToolUse": [
      { "matcher": "", "hooks": [
        { "type": "command", "timeout": 35,
          "command": "/ABS/PATH/Clawdmeter/daemon/clawdmeter-approve-hook.sh" } ] }
    ]
  }
}
```

Swipe right = approve; swipe left, timeout, or a disconnected device = defer to
the normal terminal prompt (it never denies). The hook only waits while the
device is connected; otherwise it returns instantly.
````

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: document swipe-to-approve hook install

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-Review Notes

- **Spec coverage:** device-ready gate + 10s freshness (Task 1, Task 3); request/decision files + per-id correlation + orphan drop + queue (Task 2); PreToolUse allow/ask, never deny (Task 1); TX subscription + RX `ask` payload (Task 3, Task 4); `ApprovalRequest` + swipe card with drag/spring-back/queue/timeout + Clawdio surprise (Task 4, Task 5); physical QA (Task 6); hook install docs + e2e (Task 7). Notify-only interventions stay on the existing Notification/Stop path (unchanged). Deny and AskUserQuestion selection explicitly out of scope.
- **Type/contract consistency:** payloads `{"ev":"ask","id","proj","tool","cmd","pos","total"}` (daemon→device) and `{"id","d":"approve"|"dismiss"}` (device→daemon) match across broker, daemon, firmware parser, and `ble_send_decision`. `ApprovalBroker.scan()/decide()/current_id()` names consistent. `ui_show_approval`/`ui_approval_tick` consistent between ui.h, ui.cpp, main.cpp.
- **No placeholders:** every code/test/command step is concrete.
- **Note:** swipe-direction mapping under display rotation is verified in Task 6 QA; if a rotated orientation inverts the gesture, that's a follow-up tweak, not a blocker.
