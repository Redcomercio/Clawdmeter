"""Pure milestone logic for Clawdio. No I/O — the daemon persists `state`.

Feed normalized events (feed_event) and usage percentages (feed_usage) with the
current local date ('YYYY-MM-DD'); each call returns the milestones newly unlocked
on that call: [{"id","label","anim"}, ...]. Each milestone fires at most once
(tracked in state['unlocked'])."""
from datetime import date

# threshold -> (id, label). Thresholds checked in feed_event/feed_usage.
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
