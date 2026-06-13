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
    payload = t.feed({"sid": "b", "proj": "Q", "ev": "done", "ts": 102}, now=102)
    assert payload == {"ev": "approval", "proj": "P", "n": 1}


def test_activity_without_pending_returns_none():
    t = EventTracker()
    assert t.feed({"sid": "a", "proj": "P", "ev": "activity", "ts": 100}, now=100) is None


def test_eviction_after_ten_minutes():
    t = EventTracker()
    t.feed({"sid": "a", "proj": "P", "ev": "approval", "ts": 0}, now=0)
    payload = t.feed({"sid": "b", "proj": "Q", "ev": "done", "ts": 660}, now=660)
    assert payload == {"ev": "done", "proj": "Q"}


def test_current_state_amber_when_pending():
    t = EventTracker()
    t.feed({"sid": "a", "proj": "P", "ev": "approval", "ts": 100}, now=100)
    assert t.current_state() == {"ev": "approval", "proj": "P", "n": 1}


def test_current_state_clear_when_nothing_pending():
    t = EventTracker()
    assert t.current_state() == {"ev": "clear"}
    t.feed({"sid": "a", "proj": "P", "ev": "approval", "ts": 100}, now=100)
    t.feed({"sid": "a", "proj": "P", "ev": "done", "ts": 101}, now=101)
    assert t.current_state() == {"ev": "clear"}
