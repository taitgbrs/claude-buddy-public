#!/bin/sh
# Installs the Claude Buddy hooks into ~/.claude/settings.json (merges, keeps existing settings).
# Run on any computer where Claude Code runs. Re-running is safe.
set -e
cd "$(dirname "$0")"
if grep -q '^TOPIC=.*CHANGE-ME' claude-buddy.sh && [ -z "$CLAUDE_BUDDY_TOPIC" ]; then
  echo "Set your own topic in claude-buddy.sh first (see README)"; exit 1
fi
mkdir -p ~/.claude
cp claude-buddy.sh ~/.claude/claude-buddy.sh
chmod +x ~/.claude/claude-buddy.sh
python3 - <<'EOF'
import json, os
p = os.path.expanduser("~/.claude/settings.json")
s = json.load(open(p)) if os.path.exists(p) else {}
hooks = s.setdefault("hooks", {})
for event, word in [("UserPromptSubmit", "working"), ("PostToolUse", "working"),
                    ("Stop", "done"), ("Notification", "input")]:
    cmd = f"~/.claude/claude-buddy.sh {word}"
    groups = hooks.setdefault(event, [])
    if any(h.get("command") == cmd for g in groups for h in g.get("hooks", [])):
        continue
    groups.append({"hooks": [{"type": "command", "command": cmd, "async": True}]})
json.dump(s, open(p, "w"), indent=2)
print("hooks installed in", p)
EOF
