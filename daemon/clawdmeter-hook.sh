#!/usr/bin/env bash
# Clawdmeter Claude Code hook. Invoked with the hook event name as $1.
# Reads the hook JSON payload on stdin and appends one normalized event
# line to the event file. ALWAYS exits 0 — a failing hook must never
# disturb a Claude Code session.
#
# Install: register this for Notification, Stop and PostToolUse in
# ~/.claude/settings.json (see README).

event_name="$1"
event_file="${CLAWDMETER_EVENT_FILE:-$HOME/.config/claude-usage-monitor/events.jsonl}"

case "$event_name" in
    Notification) ev="approval" ;;
    Stop)         ev="done" ;;
    PostToolUse)  ev="activity" ;;
    *)            exit 0 ;;
esac

payload="$(cat)"

# Extract session_id and cwd. Prefer jq; fall back to grep so the hook
# works on machines without jq installed.
if command -v jq >/dev/null 2>&1; then
    sid="$(printf '%s' "$payload" | jq -r '.session_id // ""' 2>/dev/null)"
    cwd="$(printf '%s' "$payload" | jq -r '.cwd // ""' 2>/dev/null)"
else
    sid="$(printf '%s' "$payload" | grep -o '"session_id"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*:[[:space:]]*"//;s/"$//')"
    cwd="$(printf '%s' "$payload" | grep -o '"cwd"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*:[[:space:]]*"//;s/"$//')"
fi

proj="$(basename "$cwd" 2>/dev/null)"
[ -z "$proj" ] && proj="?"
ts="$(date +%s)"

mkdir -p "$(dirname "$event_file")" 2>/dev/null
printf '{"ts":%s,"sid":"%s","proj":"%s","ev":"%s"}\n' \
    "$ts" "$sid" "$proj" "$ev" >> "$event_file" 2>/dev/null

# Extra signal: a Bash `git commit` also emits a `commit` event (for milestones).
if [ "$event_name" = "PostToolUse" ]; then
    if command -v jq >/dev/null 2>&1; then
        tool="$(printf '%s' "$payload" | jq -r '.tool_name // ""' 2>/dev/null)"
        cmd="$(printf '%s' "$payload" | jq -r '.tool_input.command // ""' 2>/dev/null)"
    else
        tool="$(printf '%s' "$payload" | grep -o '"tool_name"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*:[[:space:]]*"//;s/"$//')"
        cmd="$(printf '%s' "$payload" | grep -o '"command"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*:[[:space:]]*"//;s/"$//')"
    fi
    if [ "$tool" = "Bash" ] && printf '%s' "$cmd" | grep -q 'git commit'; then
        printf '{"ts":%s,"sid":"%s","proj":"%s","ev":"commit"}\n' \
            "$ts" "$sid" "$proj" >> "$event_file" 2>/dev/null
    fi
fi

exit 0
