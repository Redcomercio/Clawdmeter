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
    assert b.scan() is None


def test_decision_writes_res_and_advances(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a"); write_req(appdir, "b")
    b.scan()  # head = a
    b.decide("a", "approve")
    assert json.loads((appdir / "a.res").read_text()) == {"d": "approve"}
    nxt = b.scan()
    assert nxt["id"] == "b" and nxt["pos"] == 1 and nxt["total"] == 1


def test_orphan_request_dropped_when_req_file_vanishes(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    write_req(appdir, "a")
    b.scan()
    (appdir / "a.req").unlink()  # hook gave up
    assert b.scan() is None
    assert b.current_id() is None


def test_decision_for_unknown_id_is_ignored(tmp_path):
    appdir = tmp_path / "approve"; appdir.mkdir()
    b = ApprovalBroker(appdir)
    b.decide("ghost", "approve")
    assert not (appdir / "ghost.res").exists()
