#!/usr/bin/env bash
# Replays a session recording on the connected device and summarizes its AirScreenStats.
#
#   scripts/replay.sh RECORDING [LOG]
#
# RECORDING is a local file (copied into the app's files dir first) or a bare file name already in
# /data/user/0/dev.airscreen/files. The device log of the run goes to LOG (default: replay-<name>.log).
# Needs a debug build installed. Uses $ANDROID_SERIAL like adb does.
set -euo pipefail

REC=$1
NAME=$(basename "$REC")
LOG=${2:-replay-${NAME%.bin}.log}
APP=dev.airscreen
DEVICE_PATH=/data/user/0/$APP/files/$NAME
SUMMARY="$(dirname "$0")/replay-summary.py"

if [[ -f "$REC" ]]; then
  adb push "$REC" "/data/local/tmp/$NAME" >/dev/null
  adb shell "cat /data/local/tmp/$NAME | run-as $APP sh -c 'cat > files/$NAME' && rm /data/local/tmp/$NAME"
fi

SINCE=$(adb shell date +"'%m-%d %H:%M:%S.000'")
adb shell am start -S -n "$APP/.MainActivity" --es replay "$DEVICE_PATH" >/dev/null
adb logcat -v time -T "$SINCE" -s AirScreen:I AirScreenStats:I UxPlay:I >"$LOG" &
LOGCAT=$!
trap 'kill $LOGCAT 2>/dev/null' EXIT
until grep -q -e "replay finished" -e "cannot read recording" "$LOG"; do sleep 1; done
sleep 1
python3 "$SUMMARY" "$LOG"
echo "log: $LOG"
