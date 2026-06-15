import json, subprocess
from pathlib import Path

HOOK = Path(__file__).resolve().parents[1] / "clawdmeter-hook.sh"


def run(stdin_obj, event_file):
    subprocess.run(["bash", str(HOOK), "PostToolUse"], input=json.dumps(stdin_obj),
                   capture_output=True, text=True,
                   env={"CLAWDMETER_EVENT_FILE": str(event_file), "PATH": "/usr/bin:/bin"})


def evs(event_file):
    return [json.loads(l) for l in Path(event_file).read_text().splitlines() if l.strip()]


def test_git_commit_emits_commit_event(tmp_path):
    ef = tmp_path / "events.jsonl"
    run({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
         "tool_input": {"command": "git commit -m 'wip'"}}, ef)
    kinds = [e["ev"] for e in evs(ef)]
    assert "activity" in kinds
    assert "commit" in kinds


def test_non_commit_bash_no_commit_event(tmp_path):
    ef = tmp_path / "events.jsonl"
    run({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
         "tool_input": {"command": "git status"}}, ef)
    kinds = [e["ev"] for e in evs(ef)]
    assert "activity" in kinds
    assert "commit" not in kinds


def test_non_bash_tool_no_commit_event(tmp_path):
    ef = tmp_path / "events.jsonl"
    run({"session_id": "s", "cwd": "/x/proj", "tool_name": "Read",
         "tool_input": {"file_path": "git commit"}}, ef)
    assert "commit" not in [e["ev"] for e in evs(ef)]
