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
        self._queue = [i for i in self._queue if i in present]
        for i in sorted(present):
            if i not in self._queue:
                self._queue.append(i)
        if self._head_sent and self._head_sent not in present:
            self._head_sent = None  # orphaned

    def current_id(self) -> str | None:
        return self._queue[0] if self._queue else None

    def scan(self) -> dict | None:
        """Return the BLE payload for the head request if not yet sent, else None."""
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
        """Drop a decided request (the device self-hides; we just advance)."""
        (self.appdir / f"{rid}.req").unlink(missing_ok=True)
        if rid in self._queue:
            self._queue.remove(rid)
        if self._head_sent == rid:
            self._head_sent = None

    def clear_current(self) -> str | None:
        """Drop the current head (terminal-answered / timeout). Returns its id or None."""
        head = self.current_id()
        if head is None:
            return None
        (self.appdir / f"{head}.req").unlink(missing_ok=True)
        self._queue.remove(head)
        if self._head_sent == head:
            self._head_sent = None
        return head
