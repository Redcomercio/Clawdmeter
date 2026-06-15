import json, subprocess, time
from pathlib import Path

HOOK = Path(__file__).resolve().parents[1] / "clawdmeter-approve-hook.sh"


def run_hook(stdin_obj, cfg_dir, env_extra=None):
    env = {"CLAWDMETER_CONFIG_DIR": str(cfg_dir), "PATH": "/usr/bin:/bin"}
    if env_extra:
        env.update(env_extra)
    return subprocess.run(["bash", str(HOOK)], input=json.dumps(stdin_obj),
                          capture_output=True, text=True, env=env)


def reqs(cfg_dir):
    d = cfg_dir / "approve"
    return list(d.glob("*.req")) if d.exists() else []


def test_no_device_ready_returns_ask_no_req(tmp_path):
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert reqs(tmp_path) == []


def test_action_tool_writes_req_and_returns_ask_immediately(tmp_path):
    (tmp_path / "device-ready").write_text("")
    start = time.time()
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "git push"}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert time.time() - start < 3                 # non-blocking
    rs = reqs(tmp_path)
    assert len(rs) == 1
    body = json.loads(rs[0].read_text())
    assert body["tool"] == "Bash" and body["proj"] == "proj"


def test_non_action_tool_returns_ask_no_req(tmp_path):
    (tmp_path / "device-ready").write_text("")
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "AskUserQuestion",
                  "tool_input": {}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert reqs(tmp_path) == []


def test_stale_device_ready_returns_ask_no_req(tmp_path):
    ready = tmp_path / "device-ready"; ready.write_text("")
    import os
    old = time.time() - 30
    os.utime(ready, (old, old))
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert reqs(tmp_path) == []
