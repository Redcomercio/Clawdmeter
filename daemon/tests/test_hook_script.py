import json
import subprocess
import tempfile
from pathlib import Path

HOOK = Path(__file__).resolve().parents[1] / "clawdmeter-hook.sh"


def run_hook(event_name, stdin_obj, event_file):
    return subprocess.run(
        ["bash", str(HOOK), event_name],
        input=json.dumps(stdin_obj),
        capture_output=True,
        text=True,
        env={"CLAWDMETER_EVENT_FILE": str(event_file), "PATH": "/usr/bin:/bin"},
    )


def read_lines(event_file):
    return [json.loads(l) for l in Path(event_file).read_text().splitlines() if l.strip()]


def test_notification_maps_to_approval(tmp_path):
    ef = tmp_path / "events.jsonl"
    run_hook("Notification", {"session_id": "abc", "cwd": "/Users/me/dev/Clawdmeter"}, ef)
    lines = read_lines(ef)
    assert len(lines) == 1
    assert lines[0]["ev"] == "approval"
    assert lines[0]["sid"] == "abc"
    assert lines[0]["proj"] == "Clawdmeter"
    assert isinstance(lines[0]["ts"], int)


def test_stop_maps_to_done(tmp_path):
    ef = tmp_path / "events.jsonl"
    run_hook("Stop", {"session_id": "xyz", "cwd": "/tmp/proj"}, ef)
    assert read_lines(ef)[0]["ev"] == "done"


def test_posttooluse_maps_to_activity(tmp_path):
    ef = tmp_path / "events.jsonl"
    run_hook("PostToolUse", {"session_id": "xyz", "cwd": "/tmp/proj"}, ef)
    assert read_lines(ef)[0]["ev"] == "activity"


def test_exit_zero_even_with_garbage_stdin(tmp_path):
    ef = tmp_path / "events.jsonl"
    r = subprocess.run(
        ["bash", str(HOOK), "Stop"],
        input="not json",
        capture_output=True, text=True,
        env={"CLAWDMETER_EVENT_FILE": str(ef), "PATH": "/usr/bin:/bin"},
    )
    assert r.returncode == 0
