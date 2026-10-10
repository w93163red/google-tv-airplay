#!/usr/bin/env bash
# Checks the receiver's lifetime against activity launches on the connected device:
#  1. launching AirScreen again while it runs keeps one activity and the same listening socket on
#     port 7000, and starts no second receiver. The first launch names only the component, as
#     replay.sh does; the launcher's intent differs from it, which in standard launch mode put a
#     second instance on top. NEW_TASK|MULTIPLE_TASK follows;
#  2. Home stops the receiver and closes port 7000;
#  3. reopening from the launcher starts it again.
# Leaves AirScreen open in the foreground. Uses $ANDROID_SERIAL like adb does.
set -euo pipefail

APP=dev.airscreen
LAUNCHER=(-a android.intent.action.MAIN -c android.intent.category.LEANBACK_LAUNCHER -n "$APP/.MainActivity")
fail=0

listening() {  # inodes of sockets listening on TCP 7000
  adb shell cat /proc/net/tcp /proc/net/tcp6 | awk '$2 ~ /:1B58$/ && $4 == "0A" {print $10}' | sort | xargs
}
instances() {
  adb shell dumpsys activity activities | grep -c "^ *\* ActivityRecord{.* $APP/.MainActivity" || true
}
since() {
  adb shell date +"'%m-%d %H:%M:%S.000'"
}
log_since() {
  adb logcat -d -v time -T "$1" -s AirScreen:I UxPlay:E
}
check() {  # check NAME ACTUAL EXPECTED
  if [[ "$2" == "$3" ]]; then echo "PASS $1: $2"; else echo "FAIL $1: got '$2', want '$3'"; fail=1; fi
}

adb shell am start -S -W -n "$APP/.MainActivity" >/dev/null
sleep 2
first=$(listening)
[[ -n "$first" ]] || { echo "FAIL receiver did not start"; exit 1; }
t=$(since)
adb shell am start -W "${LAUNCHER[@]}" >/dev/null
adb shell am start -W -f 0x18000000 -n "$APP/.MainActivity" >/dev/null
sleep 2
log=$(log_since "$t")
check "activities after relaunch" "$(instances)" 1
check "port 7000 sockets after relaunch" "$(listening)" "$first"
check "receiver starts during relaunch" "$(grep -c 'raop httpd listening' <<<"$log" || true)" 0
check "start failures during relaunch" "$(grep -c 'raop_start_httpd failed' <<<"$log" || true)" 0

adb shell input keyevent KEYCODE_HOME
sleep 2
check "port 7000 sockets after Home" "$(listening)" ""

t=$(since)
adb shell am start -W "${LAUNCHER[@]}" >/dev/null
sleep 2
check "receiver starts on reopen" "$(grep -c 'raop httpd listening' <<<"$(log_since "$t")" || true)" 1
check "port 7000 listening after reopen" "$([[ -n "$(listening)" ]] && echo yes || echo no)" yes
exit $fail
