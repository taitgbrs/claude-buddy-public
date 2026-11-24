#!/bin/sh
# Claude Code hook: tell the desk buddy what Claude is doing.
# usage: claude-buddy.sh working|done|input|idle   (hook JSON arrives on stdin)
# done/input also carry the context window size, read from the transcript: "done 204/1000" (k tokens used/max).
# "working" is only sent when it changes, so the per-tool-call hook doesn't spam ntfy; done/input always go,
# with retries. Each send is logged to ~/.claude/claude-buddy.log (last 200 lines).
TOPIC="${CLAUDE_BUDDY_TOPIC:-claude-buddy-CHANGE-ME}"
case "$TOPIC" in *CHANGE-ME) exit 0;; esac  # no topic set yet: send nothing
msg="$1"
if [ "$1" = done ] || [ "$1" = input ]; then
  ctx=$(python3 -c "
import json, os, sys
path = json.load(sys.stdin)['transcript_path']
for line in reversed(open(path).readlines()):
    if '\"usage\"' not in line:
        continue
    entry = json.loads(line)
    if entry.get('isSidechain'):
        continue
    u = entry['message']['usage']
    used = u['input_tokens'] + u.get('cache_read_input_tokens', 0) + u.get('cache_creation_input_tokens', 0)
    print('%d/%d' % (round(used / 1000), int(os.environ.get('CLAUDE_BUDDY_CTX_MAX', '1000000')) // 1000))
    break
" 2>/dev/null)
  [ -n "$ctx" ] && msg="$1 $ctx"
fi
f="${TMPDIR:-/tmp}/claude-buddy-state"
# Skip repeats of "working" (PostToolUse fires on every tool call). done/input always go: they are the ones you need.
if [ "$1" = working ] || [ "$1" = idle ]; then
  [ "$(cat "$f" 2>/dev/null)" = "$msg" ] && exit 0
fi
echo "$msg" > "$f"
curl -s -m 4 --retry 2 --retry-delay 1 --retry-all-errors -o /dev/null -w '%{http_code}' -d "$msg" "https://ntfy.sh/$TOPIC" > "$f.http" 2>/dev/null
rc=$?
# One line per send, last 200 kept, so a missed "done" can be traced: time, message, curl exit code, HTTP status.
log="$HOME/.claude/claude-buddy.log"
echo "$(date '+%F %T') $msg curl=$rc http=$(cat "$f.http" 2>/dev/null)" >> "$log"
tail -n 200 "$log" > "$log.tmp" 2>/dev/null && mv "$log.tmp" "$log"
exit 0
