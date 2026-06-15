"""Queue + request/response file plumbing for swipe-to-approve.

Pure of BLE/asyncio: the daemon scans periodically, sends scan()'s result to
the device, and calls decide() when a swipe arrives over BLE.
"""
import json
import re
from pathlib import Path

_DANGER = re.compile(
    r"rm\s+-rf|--force\b|\s-f\b|mkfs|dd\s+if=|:\(\)\s*\{|>\s*/dev/|sudo\s+rm")


def _is_dangerous(cmd: str) -> bool:
    return bool(cmd and _DANGER.search(cmd))


# File-editing tools show a 3-option prompt (Yes / Yes-allow-all / No); other
# tools (Bash, etc.) show a 2-option prompt (Yes / No).
_THREE_OPTION_TOOLS = {"Edit", "Write", "MultiEdit", "NotebookEdit"}


def _option_count(tool: str) -> int:
    return 3 if tool in _THREE_OPTION_TOOLS else 2


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
        tool = req.get("tool", "")
        return {"ev": "ask", "id": head, "proj": req.get("proj", "?"),
                "tool": tool, "cmd": req.get("cmd", ""),
                "pos": 1, "total": len(self._queue),
                "danger": _is_dangerous(req.get("cmd", "")),
                "opts": _option_count(tool)}

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
