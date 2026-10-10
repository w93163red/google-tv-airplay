#!/usr/bin/env bash
# Exposes the AirScreen receiver running in the Android emulator to the LAN.
# The emulator sits behind NAT, so iOS can neither see its mDNS records nor reach its ports.
# This script forwards the fixed receiver ports from the Mac's LAN address into the emulator
# and re-registers the receiver's _airplay/_raop records on the Mac, pointing at the Mac.
#
# UDP relays lock onto the first client address, so run "start" again before each new session.
#
# usage: scripts/emu-forward.sh start|stop|status
set -euo pipefail

SERIAL=${SERIAL:-emulator-5554}
IFACE=${IFACE:-en0}
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
STATE=${TMPDIR:-/tmp}/airscreen-emu-forward
# macOS's own AirPlay receiver owns *:7000, so RTSP is published on 17000.
# The other ports must keep their numbers because the receiver hands them to the client in SETUP.
RTSP_HOST_PORT=17000
TCP_PORTS="7000 7100"
UDP_PORTS="6000 6001 7011"

host_port() { [ "$1" = 7000 ] && echo $RTSP_HOST_PORT || echo "$1"; }

stop() {
  if [ -d "$STATE" ]; then
    for pidfile in "$STATE"/*.pid; do
      [ -e "$pidfile" ] && kill "$(cat "$pidfile")" 2>/dev/null || true
    done
    rm -rf "$STATE"
  fi
  for p in $TCP_PORTS; do "$ADB" -s "$SERIAL" emu redir del tcp:$((p + 20000)) >/dev/null 2>&1 || true; done
  for p in $UDP_PORTS; do "$ADB" -s "$SERIAL" emu redir del udp:$((p + 20000)) >/dev/null 2>&1 || true; done
}

spawn() {
  local name=$1; shift
  nohup "$@" >"$STATE/$name.log" 2>&1 &
  echo $! >"$STATE/$name.pid"
}

txt_args() {
  tr ';' '\n' <<<"$1" | sed '/^$/d'
}

start() {
  stop
  mkdir -p "$STATE"
  local ip
  ip=$(ipconfig getifaddr "$IFACE")
  [ -n "$ip" ] || { echo "no IPv4 address on $IFACE" >&2; exit 1; }

  local airplay_line raop_line
  airplay_line=$("$ADB" -s "$SERIAL" logcat -d -s AirScreen:I | grep -o 'TXT airplay .*' | tail -1 || true)
  raop_line=$("$ADB" -s "$SERIAL" logcat -d -s AirScreen:I | grep -o 'TXT raop .*' | tail -1 || true)
  [ -n "$airplay_line" ] && [ -n "$raop_line" ] || { echo "receiver TXT lines not in logcat; launch the app first" >&2; exit 1; }

  # Loopback hop: emulator redir (127.0.0.1:2xxxx -> guest port), then socat (LAN ip:port -> loopback).
  for p in $TCP_PORTS; do
    "$ADB" -s "$SERIAL" emu redir add tcp:$((p + 20000)):$p >/dev/null
    spawn "tcp$p" socat TCP4-LISTEN:"$(host_port $p)",bind="$ip",reuseaddr,fork TCP4:127.0.0.1:$((p + 20000))
  done
  for p in $UDP_PORTS; do
    "$ADB" -s "$SERIAL" emu redir add udp:$((p + 20000)):$p >/dev/null
    spawn "udp$p" socat UDP4-LISTEN:"$p",bind="$ip",reuseaddr UDP4:127.0.0.1:$((p + 20000))
  done

  # TXT values never contain spaces, service names may: the TXT blob is the last token.
  local raop_txt raop_name airplay_txt name
  airplay_txt=${airplay_line#TXT airplay }
  raop_txt=${raop_line##* }
  raop_name=${raop_line#TXT raop }
  raop_name=${raop_name% *}
  name=${raop_name#*@}

  local txt=()
  while IFS= read -r kv; do txt+=("$kv"); done < <(txt_args "$airplay_txt")
  spawn mdns-airplay dns-sd -P "$name" _airplay._tcp local "$RTSP_HOST_PORT" airscreen-emu.local "$ip" "${txt[@]}"
  txt=()
  while IFS= read -r kv; do txt+=("$kv"); done < <(txt_args "$raop_txt")
  spawn mdns-raop dns-sd -P "$raop_name" _raop._tcp local "$RTSP_HOST_PORT" airscreen-emu.local "$ip" "${txt[@]}"

  echo "forwarding $ip -> $SERIAL, advertised as \"$name\""
}

status() {
  [ -d "$STATE" ] || { echo "not running"; exit 0; }
  for pidfile in "$STATE"/*.pid; do
    local n; n=$(basename "$pidfile" .pid)
    if kill -0 "$(cat "$pidfile")" 2>/dev/null; then echo "$n up"; else echo "$n DOWN"; fi
  done
  "$ADB" -s "$SERIAL" emu redir list
}

case "${1:-}" in
  start) start ;;
  stop) stop ;;
  status) status ;;
  *) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
