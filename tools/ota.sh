#!/bin/sh
# Build the firmware and send it to every stick on the topic over WiFi.
# The image is signed with the key in src/ota_key.h; sticks reject anything else.
# ntfy keeps the file for 3 hours, so a stick that's off longer than that misses the update (reflash or resend).
set -e
cd "$(dirname "$0")/.."
KEY=$(sed -n 's/^#define OTA_KEY "\(.*\)"$/\1/p' src/ota_key.h 2>/dev/null || true)
[ -n "$KEY" ] || { echo "src/ota_key.h is missing; OTA needs the same key the sticks were flashed with"; exit 1; }
TOPIC="${CLAUDE_BUDDY_TOPIC:-claude-buddy-CHANGE-ME}"
case "$TOPIC" in *CHANGE-ME) echo "set your own topic first (see README)"; exit 1;; esac
${PIO:-pio} run
BIN=.pio/build/m5stick-c/firmware.bin
MAC=$(openssl dgst -sha256 -hmac "$KEY" -r "$BIN" | cut -d' ' -f1)
curl -sSf -T "$BIN" -H "Filename: buddy.bin" -H "Message: ota $MAC" "https://ntfy.sh/$TOPIC" >/dev/null
echo "sent firmware $MAC to $TOPIC"
