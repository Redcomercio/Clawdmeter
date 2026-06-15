# Notification Center + Touch-Rotation Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A touch notification center that lists all stuck/active items across sessions with per-row delete, plus the touch-rotation fix it depends on.

**Architecture:** Part A fixes touch under rotation (remap the touch point by quadrant in `my_touch_cb`). Part B adds a full-screen overlay on `lv_layer_top()`, opened by a downward swipe, fed by a new `{"ev":"notif","items":[...]}` payload the daemon emits when the approval queue changes; tapping a row's ✕ sends `{"id","d":"clear"}` so the daemon drops that request (no keystroke).

**Tech Stack:** C++ / Arduino / LVGL 9 (firmware C6 480×480), Python 3 + bleak (daemon, `daemon/.venv2`).

**Spec:** `docs/superpowers/specs/2026-06-15-notification-center-design.md`

---

## File Structure

- `firmware/src/main.cpp` (modify) — remap touch by quadrant; detect open/close swipe; route `{"ev":"notif"}`.
- `firmware/src/ui.h` / `ui.cpp` (modify) — notification-center overlay: `ui_notif_show/hide/visible/set_list`.
- `daemon/approval_broker.py` (modify) — `list()` method.
- `daemon/claude_usage_daemon.py` (modify) — send `{"ev":"notif",...}` on queue change; accept `clear` over TX.
- `daemon/tests/test_approval_broker.py` (modify) — `list()` test.

---

## Task 1: Touch-rotation remap

**Files:**
- Modify: `firmware/src/main.cpp` (`my_touch_cb`)

Remap the raw touch point by the current rotation quadrant so touch matches the
pixel-remapped display. Hardware-tuned in QA.

- [ ] **Step 1: Add the remap before LVGL gets the point**

In `firmware/src/main.cpp` `my_touch_cb`, replace the final block:

```cpp
    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
```

with:

```cpp
    if (pressed) {
        // The display is rotated by CPU pixel-remap at flush, but touch comes in
        // raw panel coordinates. Remap the point by the current quadrant so it
        // lands on the rotated content. Square panels (480x480); the AMOLED-1.8
        // has rotation disabled so quadrant is always 0 (identity).
        const int32_t S = board_caps().width;
        uint16_t rx = x, ry = y;
        switch (imu_hal_rotation_quadrant()) {
        case 1: rx = y;         ry = S - 1 - x; break;
        case 2: rx = S - 1 - x; ry = S - 1 - y; break;
        case 3: rx = S - 1 - y; ry = x;         break;
        default: break;  // 0
        }
        data->point.x = rx;
        data->point.y = ry;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
```

- [ ] **Step 2: Build**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter && /usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 3: Flash + QA per rotation**

Flash (`-t upload --upload-port $(ls /dev/cu.usbmodem*|head -1)`). With the device
flat (quadrant 0) tap a known target (e.g. trigger a milestone badge and tap it —
should replay) and confirm it hits. Then hold the device at 90°/180°/270° and tap a
known on-screen element; confirm the correct element responds. If a quadrant is
inverted, flip its axis/sign in the `switch` and re-flash. Record the working mapping.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/main.cpp
git commit -m "fix(firmware): remap touch by rotation quadrant (touch was misaligned rotated)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2: Daemon — broker list() + notif payload + clear over TX

**Files:**
- Modify: `daemon/approval_broker.py`, `daemon/claude_usage_daemon.py`
- Test: `daemon/tests/test_approval_broker.py`

- [ ] **Step 1: Write the failing test for list()**

Append to `daemon/tests/test_approval_broker.py`:

```python
def test_list_returns_queue_items(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a", proj="A", tool="Bash")
    write_req(appdir, "b", proj="B", tool="Edit")
    b.scan()  # populate the queue order
    items = b.list()
    assert items == [{"id": "a", "proj": "A", "tool": "Bash"},
                     {"id": "b", "proj": "B", "tool": "Edit"}]


def test_list_drops_after_clear(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a"); b.scan()
    b.decide("a", "clear")
    assert b.list() == []
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_approval_broker.py -q`
Expected: FAIL — `list` not defined.

- [ ] **Step 3: Implement list()**

In `daemon/approval_broker.py`, add to `ApprovalBroker` (after `clear_current`):

```python
    def list(self) -> list[dict]:
        """Current queue as rows for the notification center (in order)."""
        self._refresh_queue()
        rows = []
        for rid in self._queue:
            try:
                req = json.loads((self.appdir / f"{rid}.req").read_text())
            except (OSError, json.JSONDecodeError):
                continue
            rows.append({"id": rid, "proj": req.get("proj", "?"),
                         "tool": req.get("tool", "")})
        return rows
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/pytest tests/test_approval_broker.py -q`
Expected: PASS.

- [ ] **Step 5: Accept `clear` over TX**

In `daemon/claude_usage_daemon.py` `Session._on_tx`, broaden the accepted decisions:

Find:
```python
        if rid and d in ("approve", "dismiss") and getattr(self, "_broker", None):
```
Replace with:
```python
        if rid and d in ("approve", "dismiss", "clear") and getattr(self, "_broker", None):
```

- [ ] **Step 6: Send the notif list when the queue changes**

In `run_broker` (in `connect_and_run`), track the last-sent list and emit it on
change (and on connect, since the first computed list differs from the initial
sentinel). Replace the `run_broker` loop body's `try:` block with:

```python
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
            # Notification-center list: send whenever it changes (and on connect).
            items = broker.list()
            if items != last_list["v"]:
                last_list["v"] = items
                await session.write_payload({"ev": "notif", "items": items})
        except (OSError, BleakError) as e:
            log(f"Broker error: {e}")
```

And initialize the tracker at the top of `run_broker` (next to `sent_at`):

```python
    last_list = {"v": None}
```

- [ ] **Step 7: Verify import + tests**

Run: `cd /Users/cristoballama/development/redcomercio-repos/Clawdmeter/daemon && .venv2/bin/python -c "import claude_usage_daemon; print('import ok')" && .venv2/bin/pytest tests/test_approval_broker.py tests/test_approve_hook.py -q`
Expected: import ok; all pass.

- [ ] **Step 8: Commit**

```bash
git add daemon/approval_broker.py daemon/claude_usage_daemon.py daemon/tests/test_approval_broker.py
git commit -m "feat(daemon): broker list() + notif payload on queue change + accept clear

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3: Firmware — notification-center overlay + list routing

**Files:**
- Modify: `firmware/src/ui.h`, `firmware/src/ui.cpp`, `firmware/src/main.cpp`

A full-screen overlay on `lv_layer_top()` listing rows (proj · tool) each with an ✕
that sends a clear; a Cerrar button; an empty state. Fed by `{"ev":"notif",...}`.

- [ ] **Step 1: Declare the API in ui.h**

In `firmware/src/ui.h`, after the milestone declarations:

```c
#define NOTIF_MAX 8
struct NotifItem { char id[40]; char proj[24]; char tool[16]; };
void ui_notif_set_list(const NotifItem* items, uint8_t count);
void ui_notif_show(void);
void ui_notif_hide(void);
bool ui_notif_visible(void);
```

- [ ] **Step 2: Implement the overlay in ui.cpp**

Add with the other overlays in `firmware/src/ui.cpp` (it uses `ble_send_decision`,
already included):

```cpp
// ---- Notification center: full-screen list of stuck/active items ----
static lv_obj_t* notif_panel = nullptr;
static lv_obj_t* notif_list = nullptr;   // scrollable container of rows
static lv_obj_t* notif_empty = nullptr;
static NotifItem notif_items[NOTIF_MAX];
static uint8_t   notif_count = 0;

static void notif_x_cb(lv_event_t* e) {
    const char* id = (const char*)lv_event_get_user_data(e);
    if (id && id[0]) ble_send_decision(id, "clear");  // daemon drops it; no keystroke
    // The row hides immediately; the daemon's next notif list confirms removal.
    lv_obj_t* row = (lv_obj_t*)lv_obj_get_parent(lv_event_get_target_obj(e));
    if (row) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
}

static void notif_close_cb(lv_event_t* e) { LV_UNUSED(e); ui_notif_hide(); }

static void notif_ensure(void) {
    if (notif_panel) return;
    const BoardCaps& c = board_caps();
    notif_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(notif_panel, c.width, c.height);
    lv_obj_align(notif_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(notif_panel, 0, 0);
    lv_obj_set_style_border_width(notif_panel, 0, 0);
    lv_obj_set_style_bg_color(notif_panel, lv_color_hex(0x14141c), 0);
    lv_obj_set_style_pad_all(notif_panel, 12, 0);
    lv_obj_clear_flag(notif_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(notif_panel);
    lv_obj_set_style_text_font(title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Notificaciones");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    notif_empty = lv_label_create(notif_panel);
    lv_obj_set_style_text_font(notif_empty, &font_styrene_20, 0);
    lv_obj_set_style_text_color(notif_empty, lv_color_hex(0x8a8a92), 0);
    lv_label_set_text(notif_empty, "Sin notificaciones");
    lv_obj_center(notif_empty);

    notif_list = lv_obj_create(notif_panel);
    lv_obj_set_size(notif_list, c.width - 24, c.height - 130);
    lv_obj_align(notif_list, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(notif_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(notif_list, 0, 0);
    lv_obj_set_flex_flow(notif_list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* close = lv_obj_create(notif_panel);
    lv_obj_set_size(close, c.width - 80, 52);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(close, 12, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x3a3a44), 0);
    lv_obj_add_event_cb(close, notif_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* close_lbl = lv_label_create(close);
    lv_obj_set_style_text_font(close_lbl, &font_styrene_20, 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), 0);
    lv_label_set_text(close_lbl, "Cerrar");
    lv_obj_center(close_lbl);

    lv_obj_add_flag(notif_panel, LV_OBJ_FLAG_HIDDEN);
}

static void notif_render(void) {
    notif_ensure();
    lv_obj_clean(notif_list);                       // drop old rows
    if (notif_count == 0) lv_obj_clear_flag(notif_empty, LV_OBJ_FLAG_HIDDEN);
    else                  lv_obj_add_flag(notif_empty, LV_OBJ_FLAG_HIDDEN);
    const BoardCaps& c = board_caps();
    for (uint8_t i = 0; i < notif_count; i++) {
        lv_obj_t* row = lv_obj_create(notif_list);
        lv_obj_set_size(row, c.width - 32, 60);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x202028), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* txt = lv_label_create(row);
        lv_obj_set_style_text_font(txt, &font_styrene_20, 0);
        lv_obj_set_style_text_color(txt, lv_color_white(), 0);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_DOT);
        lv_obj_set_width(txt, c.width - 120);
        char line[44];
        snprintf(line, sizeof(line), "%s · %s", notif_items[i].proj, notif_items[i].tool);
        lv_label_set_text(txt, line);
        lv_obj_align(txt, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t* x = lv_obj_create(row);
        lv_obj_set_size(x, 44, 44);
        lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(x, 0, 0);
        lv_obj_set_style_bg_color(x, lv_color_hex(0xB23b3b), 0);
        lv_obj_align(x, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_event_cb(x, notif_x_cb, LV_EVENT_CLICKED, (void*)notif_items[i].id);
        lv_obj_t* xl = lv_label_create(x);
        lv_obj_set_style_text_font(xl, &font_styrene_20, 0);
        lv_obj_set_style_text_color(xl, lv_color_white(), 0);
        lv_label_set_text(xl, LV_SYMBOL_CLOSE);
        lv_obj_center(xl);
    }
}

void ui_notif_set_list(const NotifItem* items, uint8_t count) {
    if (count > NOTIF_MAX) count = NOTIF_MAX;
    notif_count = count;
    for (uint8_t i = 0; i < count; i++) notif_items[i] = items[i];
    if (ui_notif_visible()) notif_render();
}

void ui_notif_show(void) {
    notif_ensure();
    notif_render();
    lv_obj_clear_flag(notif_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(notif_panel);
}

void ui_notif_hide(void) {
    if (notif_panel) lv_obj_add_flag(notif_panel, LV_OBJ_FLAG_HIDDEN);
}

bool ui_notif_visible(void) {
    return notif_panel && !lv_obj_has_flag(notif_panel, LV_OBJ_FLAG_HIDDEN);
}
```

- [ ] **Step 3: Route the notif payload in main.cpp**

In `firmware/src/main.cpp`, after `parse_milestone_json`, add:

```cpp
static NotifItem notif_buf[NOTIF_MAX];

// Parse an {"ev":"notif","items":[...]} payload and push it to the UI.
static void handle_notif_json(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    JsonArray arr = doc["items"].as<JsonArray>();
    uint8_t n = 0;
    for (JsonObject it : arr) {
        if (n >= NOTIF_MAX) break;
        strlcpy(notif_buf[n].id,   it["id"]   | "", sizeof(notif_buf[n].id));
        strlcpy(notif_buf[n].proj, it["proj"] | "", sizeof(notif_buf[n].proj));
        strlcpy(notif_buf[n].tool, it["tool"] | "", sizeof(notif_buf[n].tool));
        n++;
    }
    ui_notif_set_list(notif_buf, n);
}
```

In the BLE routing chain, add a `notif` branch BEFORE the generic `"ev"` branch
(and before `milestone`/`ask`, since `"notif"` is distinctive):

```cpp
        } else if (strstr(raw, "\"notif\"") != nullptr) {
            handle_notif_json(raw);
            ble_send_ack();
```

- [ ] **Step 4: Build all three**

Run each (`/usr/local/bin/pio run -d firmware -e <env>`): `waveshare_amoled_216_c6`,
`waveshare_amoled_216`, `waveshare_amoled_18`. Expected: SUCCESS for all.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/ui.h firmware/src/ui.cpp firmware/src/main.cpp
git commit -m "feat(firmware): notification-center overlay (rows + per-row clear + close)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4: Firmware — open/close swipe gesture

**Files:**
- Modify: `firmware/src/main.cpp` (`my_touch_cb`)

Detect a downward swipe from the top to open the center, and an upward swipe to
close it — using the already-remapped touch point, so it works at any rotation.

- [ ] **Step 1: Add swipe detection in my_touch_cb**

In `my_touch_cb`, after computing the remapped `rx, ry` (Task 1) and before/around
setting `data`, track the press start and detect the gesture on release. Replace the
`if (pressed) { ... } else { ... }` tail with:

```cpp
    static int32_t sw_start_y = -1;
    static int32_t sw_start_x = 0;
    if (pressed) {
        const int32_t S = board_caps().width;
        uint16_t rx = x, ry = y;
        switch (imu_hal_rotation_quadrant()) {
        case 1: rx = y;         ry = S - 1 - x; break;
        case 2: rx = S - 1 - x; ry = S - 1 - y; break;
        case 3: rx = S - 1 - y; ry = x;         break;
        default: break;
        }
        if (sw_start_y < 0) { sw_start_y = ry; sw_start_x = rx; }
        data->point.x = rx;
        data->point.y = ry;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        if (sw_start_y >= 0) {
            int32_t dy = (int32_t)data->point.y - sw_start_y;  // last point vs start
            const int32_t S = board_caps().width;
            if (sw_start_y < S / 5 && dy > S / 3 && !ui_notif_visible())
                ui_notif_show();                 // swipe down from top → open
            else if (ui_notif_visible() && dy < -(S / 3))
                ui_notif_hide();                 // swipe up → close
            sw_start_y = -1;
        }
        data->state = LV_INDEV_STATE_RELEASED;
    }
```

(Replaces the Task 1 tail — Task 1's remap is folded in here; keep the earlier touch
policy block above unchanged. `data->point` retains the last pressed point on release
in LVGL, used here as the swipe end.)

- [ ] **Step 2: Build**

Run: `/usr/local/bin/pio run -d firmware -e waveshare_amoled_216_c6 2>&1 | tail -3`
Expected: SUCCESS

- [ ] **Step 3: Commit**

```bash
git add firmware/src/main.cpp
git commit -m "feat(firmware): swipe-down opens the notification center, swipe-up closes

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 5: End-to-end QA + docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Flash + restart daemon**

Flash the C6 (`-t upload --upload-port $(ls /dev/cu.usbmodem*|head -1)`); restart the
daemon (`launchctl kickstart -k gui/$(id -u)/com.user.claude-usage-daemon`).

- [ ] **Step 2: Verify**

With 2+ pending approvals queued (trigger tool prompts in two sessions, or leave a
couple unanswered), swipe down from the top → the center lists both rows
(`proj · tool`). Tap a row's ✕ → it disappears and the daemon drops that request
(check `~/.config/claude-usage-monitor/approve/` shrinks). Cerrar/swipe-up closes.
Verify it works with the device rotated (touch remap from Task 1). Empty state shows
"Sin notificaciones" when nothing is pending.

- [ ] **Step 3: Document in README**

Add to the "Approve from the device" area:

```markdown
### Notification center

Swipe down from the top to open a list of everything currently pending/stuck across
your sessions (project · tool). Tap a row's ✕ to clear it from the device (the daemon
drops that request — no keystroke is sent). Cerrar or swipe up to close. Useful for
clearing prompts left stuck (e.g. after closing a terminal) when running several
sessions at once.
```

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: document the notification center

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-Review Notes

- **Spec coverage:** touch remap per quadrant (Task 1); broker `list()` + `notif`
  payload on change + `clear` over TX (Task 2); overlay with rows + ✕ clear + Cerrar +
  empty state, `notif` routing (Task 3); swipe open/close (Task 4); E2E + docs (Task 5).
  Delete = clear-only (no keystroke), coexists with the single approval card — matches spec.
- **Contract consistency:** `{"ev":"notif","items":[{"id","proj","tool"}]}` (daemon→device),
  `{"id","d":"clear"}` (device→daemon, now accepted in `_on_tx`). `NotifItem`/`ui_notif_*`
  consistent across ui.h, ui.cpp, main.cpp. `broker.list()` shape matches the payload.
- **No placeholders:** every step has concrete code/commands.
- **Note:** Task 1's touch-remap tail and Task 4's swipe tail edit the same block —
  Task 4 supersedes Task 1's `if (pressed)` tail (the remap is folded in). Apply Task 1
  first (QA the mapping), then Task 4 layers the swipe on top.
