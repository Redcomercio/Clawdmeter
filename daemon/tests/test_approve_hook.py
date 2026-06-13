import json, subprocess, threading, time
from pathlib import Path

HOOK = Path(__file__).resolve().parents[1] / "clawdmeter-approve-hook.sh"


def run_hook(stdin_obj, cfg_dir, env_extra=None):
    env = {"CLAWDMETER_CONFIG_DIR": str(cfg_dir), "PATH": "/usr/bin:/bin"}
    if env_extra:
        env.update(env_extra)
    return subprocess.run(["bash", str(HOOK)], input=json.dumps(stdin_obj),
                          capture_output=True, text=True, env=env)


def test_no_device_ready_returns_ask(tmp_path):
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path)
    out = json.loads(r.stdout)
    assert out["hookSpecificOutput"]["permissionDecision"] == "ask"
    assert r.returncode == 0


def test_stale_device_ready_returns_ask(tmp_path):
    ready = tmp_path / "device-ready"
    ready.write_text("")
    import os
    old = time.time() - 30
    os.utime(ready, (old, old))
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path)
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"


def test_approve_decision_returns_allow(tmp_path):
    (tmp_path / "device-ready").write_text("")
    (tmp_path / "approve").mkdir()

    def responder():
        for _ in range(100):
            reqs = list((tmp_path / "approve").glob("*.req"))
            if reqs:
                rid = reqs[0].stem
                (tmp_path / "approve" / f"{rid}.res").write_text('{"d":"approve"}')
                return
            time.sleep(0.05)

    t = threading.Thread(target=responder); t.start()
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "git push"}}, tmp_path)
    t.join()
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "allow"


def test_dismiss_decision_returns_ask(tmp_path):
    (tmp_path / "device-ready").write_text("")
    (tmp_path / "approve").mkdir()

    def responder():
        for _ in range(100):
            reqs = list((tmp_path / "approve").glob("*.req"))
            if reqs:
                (tmp_path / "approve" / f"{reqs[0].stem}.res").write_text('{"d":"dismiss"}')
                return
            time.sleep(0.05)

    t = threading.Thread(target=responder); t.start()
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "rm x"}}, tmp_path)
    t.join()
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"


def test_timeout_returns_ask(tmp_path):
    (tmp_path / "device-ready").write_text("")
    (tmp_path / "approve").mkdir()
    r = run_hook({"session_id": "s", "cwd": "/x/proj", "tool_name": "Bash",
                  "tool_input": {"command": "ls"}}, tmp_path,
                 env_extra={"CLAWDMETER_APPROVE_TIMEOUT": "1"})
    assert json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] == "ask"
