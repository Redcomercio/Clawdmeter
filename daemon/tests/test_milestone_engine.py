from milestone_engine import MilestoneEngine


def ev(kind):
    return {"ev": kind, "sid": "s", "proj": "P"}


def test_first_active_day_sets_streak_one():
    e = MilestoneEngine()
    e.feed_event(ev("activity"), "2026-06-14")
    assert e.state["streak_count"] == 1


def test_consecutive_days_increment_streak_and_unlock_3():
    e = MilestoneEngine()
    e.feed_event(ev("activity"), "2026-06-12")
    e.feed_event(ev("activity"), "2026-06-13")
    out = e.feed_event(ev("activity"), "2026-06-14")
    assert e.state["streak_count"] == 3
    assert any(m["id"] == "streak3" for m in out)


def test_gap_resets_streak():
    e = MilestoneEngine()
    e.feed_event(ev("activity"), "2026-06-12")
    e.feed_event(ev("activity"), "2026-06-14")  # skipped the 13th
    assert e.state["streak_count"] == 1


def test_same_day_does_not_increment():
    e = MilestoneEngine()
    e.feed_event(ev("activity"), "2026-06-14")
    e.feed_event(ev("done"), "2026-06-14")
    assert e.state["streak_count"] == 1


def test_done_counts_tasks_and_unlocks_daily_10():
    e = MilestoneEngine()
    out = []
    for _i in range(10):
        out += e.feed_event(ev("done"), "2026-06-14")
    assert e.state["tasks_today"] == 10
    assert e.state["tasks_total"] == 10
    assert any(m["id"] == "tasks_day10" for m in out)


def test_tasks_today_resets_on_new_day():
    e = MilestoneEngine()
    e.feed_event(ev("done"), "2026-06-14")
    e.feed_event(ev("done"), "2026-06-15")
    assert e.state["tasks_today"] == 1
    assert e.state["tasks_total"] == 2


def test_commit_counts_and_unlocks_first():
    e = MilestoneEngine()
    out = e.feed_event(ev("commit"), "2026-06-14")
    assert e.state["commits_total"] == 1
    assert any(m["id"] == "commit1" for m in out)


def test_usage_in_zone_unlocks_once():
    e = MilestoneEngine()
    out1 = e.feed_usage(30.0, "2026-06-14")
    out2 = e.feed_usage(33.0, "2026-06-14")
    assert any(m["id"] == "in_zone" for m in out1)
    assert not any(m["id"] == "in_zone" for m in out2)


def test_usage_marathon_unlocks_once():
    e = MilestoneEngine()
    out1 = e.feed_usage(85.0, "2026-06-14")
    out2 = e.feed_usage(90.0, "2026-06-15")
    assert any(m["id"] == "marathon" for m in out1)
    assert not any(m["id"] == "marathon" for m in out2)


def test_unlock_fires_once_across_restart():
    e = MilestoneEngine()
    for _i in range(10):
        e.feed_event(ev("done"), "2026-06-14")
    saved = e.state
    e2 = MilestoneEngine(state=saved)          # simulate restart
    out = e2.feed_event(ev("done"), "2026-06-14")
    assert not any(m["id"] == "tasks_day10" for m in out)
