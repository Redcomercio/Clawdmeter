#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from approval_broker import ApprovalBroker
from milestone_engine import MilestoneEngine

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"  # device -> host notify

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
EVENT_FILE = Path.home() / ".config" / "claude-usage-monitor" / "events.jsonl"
EVENT_TICK = 1.0  # seconds between event-file size checks

DEVICE_READY_FILE = Path.home() / ".config" / "claude-usage-monitor" / "device-ready"
APPROVE_DIR = Path.home() / ".config" / "claude-usage-monitor" / "approve"
APPROVE_TICK = 0.3  # seconds between approve-dir scans
CLAWDIO_STATE_FILE = Path.home() / ".config" / "claude-usage-monitor" / "clawdio-state.json"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Claude Controller', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    payload = {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }
    return payload


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

    def current_state(self) -> dict | None:
        """The banner payload reflecting current pending sessions, or None.

        Used to re-assert device state on reconnect. Returns the amber
        payload if any session is pending, else a clear so a stale banner
        on the device is dismissed."""
        if self._pending_count():
            return self._amber_payload()
        return {"ev": "clear"}

    def pending_list(self) -> list[dict]:
        """Rows for the notification center: one per pending session."""
        return [{"id": sid, "proj": s["proj"], "tool": "aprobación"}
                for sid, s in self._sessions.items() if s["pending"]]

    def clear(self, sid: str) -> bool:
        """Clear one session's pending flag (notification-center delete).
        Returns True if it was pending."""
        s = self._sessions.get(sid)
        if s and s["pending"]:
            s["pending"] = False
            return True
        return False

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
                       watch_pos: dict, stop_event: asyncio.Event, engine) -> None:
    """Tail EVENT_FILE; push tracker payloads to the device as events arrive.

    Also refreshes the device-ready flag every tick (~1s) so the PreToolUse hook
    (which treats it as fresh for 10s) reliably sees the device as connected —
    the 60s usage poll alone would let it go stale between polls."""
    last_notif = None
    while not stop_event.is_set() and session.client.is_connected:
        touch_device_ready()
        try:
            size = EVENT_FILE.stat().st_size if EVENT_FILE.exists() else 0
            if size < watch_pos["pos"]:
                watch_pos["pos"] = 0  # file truncated/rotated
            if size > watch_pos["pos"]:
                with EVENT_FILE.open("r") as f:
                    f.seek(watch_pos["pos"])
                    new = f.read()
                    watch_pos["pos"] = f.tell()
                for line in new.splitlines():
                    obj = parse_event_line(line)
                    if obj is None:
                        continue
                    payload = tracker.feed(obj, now=time.time())
                    if payload is not None:
                        await session.write_payload(payload)
                    # Milestone progress (streaks/tasks/commits) from this event.
                    for m in engine.feed_event(obj, _local_today()):
                        save_clawdio_state(engine.state)
                        await session.write_payload(
                            {"ev": "milestone", "id": m["id"],
                             "label": m["label"], "anim": m["anim"]})
                    # A prompt resolved in the terminal (tool ran / session moved
                    # on) clears any mirror card still showing on the device.
                    if obj.get("ev") in ("activity", "done"):
                        brk = getattr(session, "_broker", None)
                        if brk and brk.clear_current():
                            await session.write_payload({"ev": "clear-ask"})
            # Notification center: send the pending list whenever it changes
            # (also covers connect: None -> []).
            nl = tracker.pending_list()
            if nl != last_notif:
                last_notif = nl
                await session.write_payload({"ev": "notif", "items": nl})
            # A device-initiated notif clear has no event to drive the banner —
            # re-assert it here.
            if getattr(session, "_notif_dirty", False):
                session._notif_dirty = False
                state = tracker.current_state()
                if state is not None:
                    await session.write_payload(state)
        except (OSError, BleakError) as e:
            log(f"Event watch error: {e}")
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=EVENT_TICK)
        except asyncio.TimeoutError:
            pass


def touch_device_ready() -> None:
    try:
        DEVICE_READY_FILE.parent.mkdir(parents=True, exist_ok=True)
        DEVICE_READY_FILE.touch()
    except OSError as e:
        log(f"device-ready touch failed: {e}")


def clear_device_ready() -> None:
    DEVICE_READY_FILE.unlink(missing_ok=True)


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


async def run_broker(session: "Session", broker: ApprovalBroker,
                     stop_event: asyncio.Event) -> None:
    """Scan the approve dir; send the head request; expire stale cards at 60s."""
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


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    def attach_broker(self, broker) -> None:
        self._broker = broker

    def attach_tracker(self, tracker) -> None:
        self._tracker = tracker

    def _on_tx(self, _char, data: bytearray) -> None:
        try:
            msg = json.loads(bytes(data).decode())
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        rid, d = msg.get("id"), msg.get("d")
        if not rid:
            return
        if d == "notifclear" and getattr(self, "_tracker", None):
            # Notification-center delete: clear that session's pending banner.
            log(f"Notif clear for {rid}")
            if self._tracker.clear(rid):
                self._notif_dirty = True   # watch loop re-asserts banner + list
        elif d in ("approve", "dismiss", "clear") and getattr(self, "_broker", None):
            log(f"Swipe decision {d} for {rid}")
            self._broker.decide(rid, d)

    async def setup_tx_subscription(self) -> None:
        try:
            await self.client.start_notify(TX_CHAR_UUID, self._on_tx)
        except (BleakError, ValueError) as e:
            log(f"TX subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(target, stop_event: asyncio.Event,
                          tracker: EventTracker, watch_pos: dict, engine) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()
    broker = ApprovalBroker(APPROVE_DIR)
    session.attach_broker(broker)
    session.attach_tracker(tracker)
    await session.setup_tx_subscription()
    touch_device_ready()

    # Re-assert the current banner state so a reconnected device is in sync.
    state = tracker.current_state()
    if state is not None:
        await session.write_payload(state)

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
                        touch_device_ready()
                        try:
                            pct = float(payload.get("s", 0))
                        except (TypeError, ValueError):
                            pct = 0.0
                        for m in engine.feed_usage(pct, _local_today()):
                            save_clawdio_state(engine.state)
                            await session.write_payload(
                                {"ev": "milestone", "id": m["id"],
                                 "label": m["label"], "anim": m["anim"]})
            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass

    poll_task = asyncio.create_task(poll_loop())
    watch_task = asyncio.create_task(
        watch_events(session, tracker, watch_pos, stop_event, engine))
    broker_task = asyncio.create_task(run_broker(session, broker, stop_event))
    try:
        done, pending = await asyncio.wait(
            {poll_task, watch_task, broker_task}, return_when=asyncio.FIRST_COMPLETED)
        for t in pending:
            t.cancel()
        for t in pending:
            try:
                await t
            except asyncio.CancelledError:
                pass
        # Surface any real exception from the finished task(s).
        for t in done:
            exc = t.exception()
            if exc is not None:
                log(f"Task error: {exc}")
    finally:
        clear_device_ready()
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used["ok"]


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    # Stale events from before this daemon started are meaningless.
    try:
        EVENT_FILE.parent.mkdir(parents=True, exist_ok=True)
        EVENT_FILE.write_text("")
    except OSError as e:
        log(f"Could not reset event file: {e}")

    tracker = EventTracker()
    watch_pos = {"pos": 0}  # persists file read position across reconnects
    engine = MilestoneEngine(state=load_clawdio_state())

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event, tracker, watch_pos, engine)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
