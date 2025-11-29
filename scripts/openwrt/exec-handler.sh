#!/bin/sh
# exec-handler.sh (OpenWrt link handler)
# Usage: exec-handler.sh <path> [args...]
# Contract: /exec with JSON {"path":"/sys/<cap>/<command>","args":[...]}
#
# Implemented:
#   /sys/help|shutdown|restart|ping|luci-on|luci-off
#   /sys/link/help|status
#   /sys/link/wifi/help|get|set|params|start|stop|status
#   /sys/link/wfb_ng/help|get|set|params|start|stop|restart|status
#
# Notes:
# - Cheap argv parsing; handler remains JSON-agnostic.
# - Uses fw_printenv/fw_setenv for link parameters.
# - Start/stop/status are best-effort stubs; rc=3 if unsupported.

PATH=/usr/sbin:/usr/bin:/sbin:/bin

die(){ echo "$*" 1>&2; exit 2; }
have(){ command -v "$1" >/dev/null 2>&1; }

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" 2>/dev/null && pwd)
[ -n "$SCRIPT_DIR" ] || die "failed to resolve script directory"

emit_msg(){
  name="$1"
  file="$SCRIPT_DIR/$name"
  [ -r "$file" ] || die "help payload missing: $name"
  cat "$file"
}

# ======================= LINK (wifi_mode-driven) =======================
# Active link type is held in wifi_mode (fw_printenv -n wifi_mode).
# Values: wfb_ng | ap | sta. Routing: wfb_ng → wfb; ap/sta → wifi.

wifi_mode_get(){
  if have fw_printenv; then fw_printenv -n wifi_mode 2>/dev/null; else echo "sta"; fi
}

# env helpers
link_param_get(){ key="$1"; if have fw_printenv; then fw_printenv -n "$key" 2>/dev/null; else die "fw_printenv not available"; fi }
link_param_set(){ key="$1"; val="$2"; if have fw_setenv; then fw_setenv "$key" "$val" >/dev/null 2>&1 || die "failed to set $key"; echo "ok"; else die "fw_setenv not available"; fi }

# WiFi help (with updated controls and freeform)
wifi_help_json(){ emit_msg "wifi_help.msg"; }

wifi_start(){
  if [ -x /etc/init.d/S50wifi ]; then /etc/init.d/S50wifi start >/dev/null 2>&1 && { echo "wifi started"; return 0; } fi
  if have wifi; then wifi up >/dev/null 2>&1 && { echo "wifi started"; return 0; } fi
  echo "wifi start unsupported on this device" 1>&2; return 3
}

wifi_stop(){
  if [ -x /etc/init.d/S50wifi ]; then /etc/init.d/S50wifi stop >/dev/null 2>&1 && { echo "wifi stopped"; return 0; } fi
  if have wifi; then wifi down >/dev/null 2>&1 && { echo "wifi stopped"; return 0; } fi
  echo "wifi stop unsupported on this device" 1>&2; return 3
}

wifi_status(){
  if have wifi; then
    wifi status 2>/dev/null || { echo "wifi status failed" 1>&2; return 3; }
    return 0
  fi
  echo "wifi status unsupported on this device" 1>&2
  return 3
}

wifi_get(){ name="$1"; [ -n "$name" ] || die "missing name"; link_param_get "$name"; }
wifi_set_one(){ pair="$1"; key="${pair%%=*}"; val="${pair#*=}"; [ -n "$key" ] || die "missing key"; [ "$key" != "$val" ] || die "missing value"; link_param_set "$key" "$val" >/dev/null || exit $?; }
wifi_params(){ ok=1; for kv in "$@"; do case "$kv" in --*) continue;; esac; out="$(wifi_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }; done; [ $ok -eq 1 ] || exit 2; echo "ok"; }

# WFB-NG stubs
wfb_help_json(){ emit_msg "wfb_help.msg"; }

wfb_start(){
  if [ -x /etc/init.d/wfb-ng ]; then /etc/init.d/wfb-ng start >/dev/null 2>&1 && { echo "wfb_ng started"; return 0; } fi
  if [ -x /etc/init.d/S95wfb_ng ]; then /etc/init.d/S95wfb_ng start >/dev/null 2>&1 && { echo "wfb_ng started"; return 0; } fi
  if [ -x /etc/init.d/S95wfb-ng ]; then /etc/init.d/S95wfb-ng start >/dev/null 2>&1 && { echo "wfb_ng started"; return 0; } fi
  echo "wfb_ng start unsupported on this device" 1>&2; return 3
}

wfb_stop(){
  if [ -x /etc/init.d/wfb-ng ]; then /etc/init.d/wfb-ng stop >/dev/null 2>&1 && { echo "wfb_ng stopped"; return 0; } fi
  if [ -x /etc/init.d/S95wfb_ng ]; then /etc/init.d/S95wfb_ng stop >/dev/null 2>&1 && { echo "wfb_ng stopped"; return 0; } fi
  if [ -x /etc/init.d/S95wfb-ng ]; then /etc/init.d/S95wfb-ng stop >/dev/null 2>&1 && { echo "wfb_ng stopped"; return 0; } fi
  echo "wfb_ng stop unsupported on this device" 1>&2; return 3
}

wfb_restart(){
  if [ -x /etc/init.d/wfb-ng ]; then /etc/init.d/wfb-ng restart >/dev/null 2>&1 && { echo "wfb_ng restarted"; return 0; } fi
  if [ -x /etc/init.d/S95wfb_ng ]; then /etc/init.d/S95wfb_ng restart >/dev/null 2>&1 && { echo "wfb_ng restarted"; return 0; } fi
  if [ -x /etc/init.d/S95wfb-ng ]; then /etc/init.d/S95wfb-ng restart >/dev/null 2>&1 && { echo "wfb_ng restarted"; return 0; } fi
  echo "wfb_ng restart unsupported on this device" 1>&2; return 3
}

wfb_status(){
  if have pidof; then
    pids=$(pidof forker 2>/dev/null) || true
    if [ -z "$pids" ]; then
      echo "wfb_ng status unavailable: forker not running" 1>&2
      return 3
    fi
    if kill -SIGUSR1 $pids >/dev/null 2>&1; then
      echo "wfb_ng status signal sent"
      return 0
    fi
  fi
  echo "wfb_ng status unsupported on this device" 1>&2
  return 3
}

wfb_get(){ name="$1"; [ -n "$name" ] || die "missing name"; link_param_get "$name"; }
wfb_set_one(){ pair="$1"; key="${pair%%=*}"; val="${pair#*=}"; [ -n "$key" ] || die "missing key"; [ "$key" != "$val" ] || die "missing value"; link_param_set "$key" "$val" >/dev/null || exit $?; }
wfb_params(){ ok=1; for kv in "$@"; do case "$kv" in --*) continue;; esac; out="$(wfb_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }; done; [ $ok -eq 1 ] || exit 2; echo "ok"; }

# Overall link control using wifi_mode
link_help_json(){ emit_msg "link_help.msg"; }

link_route_status(){
  mode="$(wifi_mode_get)"
  case "$mode" in
    wfb_ng) wfb_status ;;
    ap|sta|*) wifi_status ;;
  esac
}

# system helpers
sys_help_json(){ emit_msg "sys_help.msg"; }

sys_shutdown_cmd(){ shutdown now; }

sys_restart_cmd(){ reboot now; }

sys_luci_on(){
  if have luci-on; then luci-on "$@"; else die "luci-on not available"; fi
}

sys_luci_off(){
  if have luci-off; then luci-off "$@"; else die "luci-off not available"; fi
}

# ======================= DISPATCH =======================
case "$1" in
  /sys/help)               sys_help_json ;;
  /sys/shutdown)           shift; sys_shutdown_cmd "$@" ;;
  /sys/restart)            shift; sys_restart_cmd "$@" ;;
  /sys/ping)               shift; ping -c 1 -W 1 "$1" 2>&1 ;;
  /sys/luci-on)            shift; sys_luci_on "$@" ;;
  /sys/luci-off)           shift; sys_luci_off "$@" ;;

  # link overall (wifi_mode-driven)
  /sys/link/help)         link_help_json ;;
  /sys/link/status)       shift; link_route_status "$@" ;;

  # wifi
  /sys/link/wifi/help)     wifi_help_json ;;
  /sys/link/wifi/get)      shift; wifi_get "$1" ;;
  /sys/link/wifi/set)      shift; wifi_set_one "$1" ;;
  /sys/link/wifi/params)   shift; wifi_params "$@" ;;
  /sys/link/wifi/start)    shift; wifi_start "$@" ;;
  /sys/link/wifi/stop)     shift; wifi_stop "$@" ;;
  /sys/link/wifi/status)   shift; wifi_status "$@" ;;

  # wfb_ng
  /sys/link/wfb_ng/help)   wfb_help_json ;;
  /sys/link/wfb_ng/get)    shift; wfb_get "$1" ;;
  /sys/link/wfb_ng/set)    shift; wfb_set_one "$1" ;;
  /sys/link/wfb_ng/params) shift; wfb_params "$@" ;;
  /sys/link/wfb_ng/start)  shift; wfb_start "$@" ;;
  /sys/link/wfb_ng/stop)   shift; wfb_stop "$@" ;;
  /sys/link/wfb_ng/restart) shift; wfb_restart "$@" ;;
  /sys/link/wfb_ng/status) shift; wfb_status "$@" ;;

  *) echo "unknown path: $1" 1>&2; exit 2 ;;

esac
