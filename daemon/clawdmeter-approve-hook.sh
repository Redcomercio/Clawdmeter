#!/usr/bin/env bash
# Clawdmeter PreToolUse hook. Approves a tool from the device via swipe.
# - Only engages when the device is connected (fresh device-ready flag);
#   otherwise returns "ask" instantly so normal terminal prompts are unaffected.
# - Writes a request file the daemon picks up, polls for a decision file.
# - approve -> "allow"; dismiss/timeout/no-device -> "ask" (never "deny").
# Always exits 0.

cfg="${CLAWDMETER_CONFIG_DIR:-$HOME/.config/claude-usage-monitor}"
ready="$cfg/device-ready"
appdir="$cfg/approve"
fresh_secs=10

emit_ask() { printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"ask"}}\n'; exit 0; }

# Gate: device must be connected (flag present and fresh).
[ -f "$ready" ] || emit_ask
now=$(date +%s)
mtime=$(stat -f %m "$ready" 2>/dev/null || stat -c %Y "$ready" 2>/dev/null || echo 0)
[ $(( now - mtime )) -le "$fresh_secs" ] || emit_ask

payload="$(cat)"
if command -v jq >/dev/null 2>&1; then
    sid=$(printf '%s' "$payload"  | jq -r '.session_id // ""')
    cwd=$(printf '%s' "$payload"  | jq -r '.cwd // ""')
    tool=$(printf '%s' "$payload" | jq -r '.tool_name // ""')
    cmd=$(printf '%s' "$payload"  | jq -r '(.tool_input.command // .tool_input.file_path // "") | tostring' | cut -c1-80)
else
    sid=$(printf '%s' "$payload"  | grep -o '"session_id"[^,]*' | head -1 | sed 's/.*:"//;s/"//')
    cwd=$(printf '%s' "$payload"  | grep -o '"cwd"[^,]*'        | head -1 | sed 's/.*:"//;s/"//')
    tool=$(printf '%s' "$payload" | grep -o '"tool_name"[^,]*'  | head -1 | sed 's/.*:"//;s/"//')
    cmd=""
fi

# Only engage the device for action tools that genuinely gate on permission.
# Everything else (AskUserQuestion, Read, Grep, Glob, TodoWrite, …) returns ask
# instantly so it never blocks the terminal on a device button press.
case "$tool" in
    Bash|Edit|Write|MultiEdit|NotebookEdit|mcp__*) ;;  # → engage device
    *) emit_ask ;;                                     # → terminal, instant
esac

proj=$(basename "$cwd" 2>/dev/null); [ -z "$proj" ] && proj="?"
# macOS `date` has no %N (nanoseconds); use epoch seconds + this hook's PID,
# unique per invocation, to avoid id collisions within the same second.
id="${sid}-$(date +%s)-$$"

mkdir -p "$appdir" 2>/dev/null
printf '{"id":"%s","sid":"%s","proj":"%s","tool":"%s","cmd":"%s"}\n' \
    "$id" "$sid" "$proj" "$tool" "${cmd//\"/\'}" > "$appdir/$id.req" 2>/dev/null

# Non-blocking: the terminal prompt shows now; the device card just mirrors it and
# (on approve) types the answer via HID. The hook never decides — always "ask".
emit_ask
