#!/bin/sh
# exec-handler.sh (pixelpilot + udp relay stubs, instant-apply, external help, reboot)
# Usage: exec-handler.sh <path> [args...]
# Contract: /exec with JSON {"path":"/sys/<cap>/<command>","args":[...]}
#
# Implemented:
#   /sys/reboot                                 (schedule reboot)
#   /sys/pixelpilot/help|get|set|params|apply|stop|restart|start|status
#   /sys/pixelpilot_mini_rk/help|toggle_osd|toggle_recording|reboot
#   /sys/udp_relay/help|get|set|params|start|stop|status
#   /sys/link/help|mode|select|start|stop|status
#   /sys/ping                                    (utility passthrough)
#
# Notes:
# - Help JSON is externalized to message files under $HELP_DIR (default: /etc/autod).
# - Parameter storage uses fw_printenv/fw_setenv when available and falls back to
#   simple key=value state files under $STATE_DIR (default: /tmp/vrx).
# - Start/stop/status handlers are defensive stubs that attempt init/service/systemctl
#   control and otherwise return rc=3 to indicate unsupported operations.

PATH=/usr/sbin:/usr/bin:/sbin:/bin
HELP_DIR="${HELP_DIR:-/etc/autod}"
STATE_DIR="${VRX_STATE_DIR:-/tmp/vrx}"
PIXELPILOT_STATE_FILE="$STATE_DIR/pixelpilot.env"
UDP_RELAY_STATE_FILE="$STATE_DIR/udp_relay.env"
LINK_STATE_FILE="$STATE_DIR/link.env"

# ======================= Helpers =======================
die(){ echo "$*" 1>&2; exit 2; }
have(){ command -v "$1" >/dev/null 2>&1; }
ensure_parent_dir(){ dir="$1"; [ -d "$dir" ] || mkdir -p "$dir"; }

# print external help JSON (fallback to simple error payload)
print_help_msg(){
  name="$1"
  file="$HELP_DIR/$name"
  if [ -r "$file" ]; then
    cat "$file"
  else
    echo "{\"error\":\"help file not found\",\"name\":\"$name\"}"
    return 4
  fi
}

# kv storage fallbacks when fw_(print|set)env are unavailable
kv_file_get(){
  key="$1"; file="$2"
  [ -f "$file" ] || return 1
  while IFS= read -r line; do
    case "$line" in
      "#"*) continue ;;
      "${key}="*) echo "${line#${key}=}"; return 0 ;;
    esac
  done < "$file"
  return 1
}

kv_file_set(){
  key="$1"; value="$2"; file="$3"
  ensure_parent_dir "$(dirname "$file")"
  tmp="$file.tmp.$$"
  found=0
  {
    if [ -f "$file" ]; then
      while IFS= read -r line; do
        case "$line" in
          "${key}="*)
            echo "${key}=${value}"
            found=1
            ;;
          *)
            [ -n "$line" ] && echo "$line"
            ;;
        esac
      done < "$file"
    fi
    [ $found -eq 1 ] || echo "${key}=${value}"
  } > "$tmp" && mv "$tmp" "$file"
}

config_get(){
  key="$1"; file="$2"
  if have fw_printenv; then
    fw_printenv -n "$key" 2>/dev/null && return 0
  fi
  kv_file_get "$key" "$file"
}

config_set(){
  key="$1"; value="$2"; file="$3"
  if have fw_setenv; then
    if fw_setenv "$key" "$value" >/dev/null 2>&1; then
      echo "ok"
      return 0
    fi
  fi
  kv_file_set "$key" "$value" "$file"
  echo "ok"
  return 0
}

# Normalization helpers
norm_bool(){
  v=$(echo "$1" | tr 'A-Z' 'a-z')
  case "$v" in
    1|true|on|yes|enabled) echo 1 ;;
    0|false|off|no|disabled) echo 0 ;;
    *) echo "__ERR__" ;;
  esac
}

norm_bitrate(){
  v="$1"
  case "$v" in
    *[!0-9kKmM]*) echo "__ERR__" ;;
    *[kK])        echo "${v%[kK]}" ;;
    *[mM])        echo $(( ${v%[mM]} * 1000 )) ;;
    *)            echo "$v" ;;
  esac
}

norm_uint(){
  v="$1"
  case "$v" in
    ''|*[!0-9]*) echo "__ERR__" ;;
    *) echo "$v" ;;
  esac
}

# ======================= General =======================
reboot_cmd(){
  ( nohup sh -c 'sleep 1; reboot now' >/dev/null 2>&1 & )
  echo "reboot scheduled"
  return 0
}

# ======================= Pixelpilot =======================
pixelpilot_env_key(){ echo "pixelpilot_$1"; }
pixelpilot_get_value(){ config_get "$(pixelpilot_env_key "$1")" "$PIXELPILOT_STATE_FILE"; }
pixelpilot_set_value(){ config_set "$(pixelpilot_env_key "$1")" "$2" "$PIXELPILOT_STATE_FILE"; }

pixelpilot_pids(){ pidof pixelpilot 2>/dev/null; }

sighup_pixelpilot(){
  pids="$(pixelpilot_pids)"
  [ -n "$pids" ] || return 3
  kill -HUP $pids 2>/dev/null || return 4
  return 0
}

start_pixelpilot(){
  if [ -x /etc/init.d/S95pixelpilot ]; then /etc/init.d/S95pixelpilot start >/dev/null 2>&1 && { echo "pixelpilot started"; return 0; }; fi
  if have systemctl; then systemctl start pixelpilot >/dev/null 2>&1 && { echo "pixelpilot started"; return 0; }; fi
  if have service; then service pixelpilot start >/dev/null 2>&1 && { echo "pixelpilot started"; return 0; }; fi
  echo "pixelpilot start unsupported on this device" 1>&2
  return 3
}

stop_pixelpilot(){
  if [ -x /etc/init.d/S95pixelpilot ]; then /etc/init.d/S95pixelpilot stop >/dev/null 2>&1 && { echo "pixelpilot stopped"; return 0; }; fi
  if have systemctl; then systemctl stop pixelpilot >/dev/null 2>&1 && { echo "pixelpilot stopped"; return 0; }; fi
  if have service; then service pixelpilot stop >/dev/null 2>&1 && { echo "pixelpilot stopped"; return 0; }; fi
  echo "pixelpilot stop unsupported on this device" 1>&2
  return 3
}

restart_pixelpilot(){
  if [ -x /etc/init.d/S95pixelpilot ]; then /etc/init.d/S95pixelpilot restart >/dev/null 2>&1 && { echo "pixelpilot restarted"; return 0; }; fi
  if have systemctl; then systemctl restart pixelpilot >/dev/null 2>&1 && { echo "pixelpilot restarted"; return 0; }; fi
  if have service; then service pixelpilot restart >/dev/null 2>&1 && { echo "pixelpilot restarted"; return 0; }; fi
  if start_pixelpilot; then return 0; fi
  echo "pixelpilot restart unsupported on this device" 1>&2
  return 3
}

pixelpilot_status(){
  pids="$(pixelpilot_pids)"
  if [ -n "$pids" ]; then
    echo "pixelpilot running (pid $pids)"
  else
    echo "pixelpilot not running"
  fi
  return 0
}

apply_pixelpilot_settings(){
  if sighup_pixelpilot; then echo "applied via SIGHUP"; return 0; fi
  if restart_pixelpilot; then echo "applied via restart"; return 0; fi
  echo "apply failed: pixelpilot not running and no restart method" 1>&2
  return 3
}

validate_pixelpilot_key(){
  case "$1" in
    profile|bitrate|stream_url|fec_mode|osd_enabled|latency_mode) return 0 ;;
    *) return 1 ;;
  esac
}

validate_pixelpilot_value(){
  key="$1"; val="$2"
  case "$key" in
    osd_enabled)
      norm="$(norm_bool "$val")"; [ "$norm" != "__ERR__" ] || die "invalid bool: $key=$val"; echo "$norm" ;;
    bitrate)
      norm="$(norm_bitrate "$val")"; [ "$norm" != "__ERR__" ] || die "invalid bitrate: $val"; echo "$norm" ;;
    profile)
      case "$val" in standard|long_range|cinematic) echo "$val" ;; *) die "invalid profile: $val" ;; esac ;;
    fec_mode)
      case "$val" in off|1/2|2/3|3/4|5/6) echo "$val" ;; *) die "invalid fec_mode: $val" ;; esac ;;
    latency_mode)
      case "$val" in low|normal|high) echo "$val" ;; *) die "invalid latency_mode: $val" ;; esac ;;
    stream_url)
      [ -n "$val" ] || die "stream_url must be non-empty"; echo "$val" ;;
    *) echo "$val" ;;
  esac
}

pixelpilot_get(){
  name="$1"
  [ -n "$name" ] || die "missing name"
  validate_pixelpilot_key "$name" || die "unknown setting: $name"
  pixelpilot_get_value "$name" || die "setting unavailable: $name"
}

pixelpilot_set_one(){
  pair="$1"
  key="${pair%%=*}"; val="${pair#*=}"
  [ -n "$key" ] || die "missing key in pair"
  [ "$key" != "$val" ] || die "missing value in pair"
  validate_pixelpilot_key "$key" || die "unknown setting: $key"
  norm_val="$(validate_pixelpilot_value "$key" "$val")"
  pixelpilot_set_value "$key" "$norm_val" >/dev/null || die "failed to persist $key"
}

pixelpilot_params(){
  ok=1
  for kv in "$@"; do
    case "$kv" in --*) continue ;; esac
    out="$(pixelpilot_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }
  done
  [ $ok -eq 1 ] || exit 2
  apply_pixelpilot_settings || exit $?
  echo "ok"
}

pixelpilot_set_cmd(){
  [ $# -ge 1 ] || die "usage: /sys/pixelpilot/set key=value"
  pixelpilot_set_one "$1" || exit $?
  apply_pixelpilot_settings || exit $?
  echo "ok"
}

pixelpilot_apply_cmd(){ apply_pixelpilot_settings; }
pixelpilot_stop_cmd(){ stop_pixelpilot; }
pixelpilot_restart_cmd(){ restart_pixelpilot; }
pixelpilot_start_cmd(){ start_pixelpilot; }
pixelpilot_status_cmd(){ pixelpilot_status; }

# ======================= UDP Relay =======================
udp_env_key(){ echo "udp_relay_$1"; }
udp_get_value(){ config_get "$(udp_env_key "$1")" "$UDP_RELAY_STATE_FILE"; }
udp_set_value(){ config_set "$(udp_env_key "$1")" "$2" "$UDP_RELAY_STATE_FILE"; }

udp_relay_start(){
  if [ -x /etc/init.d/S60udp_relay ]; then /etc/init.d/S60udp_relay start >/dev/null 2>&1 && { echo "udp relay started"; return 0; }; fi
  if have systemctl; then systemctl start udp-relay >/dev/null 2>&1 && { echo "udp relay started"; return 0; }; fi
  if have systemctl; then systemctl start udp_relay >/dev/null 2>&1 && { echo "udp relay started"; return 0; }; fi
  if have service; then service udp_relay start >/dev/null 2>&1 && { echo "udp relay started"; return 0; }; fi
  if have udp-relayctl; then udp-relayctl start >/dev/null 2>&1 && { echo "udp relay started"; return 0; }; fi
  echo "udp relay start unsupported on this device" 1>&2
  return 3
}

udp_relay_stop(){
  if [ -x /etc/init.d/S60udp_relay ]; then /etc/init.d/S60udp_relay stop >/dev/null 2>&1 && { echo "udp relay stopped"; return 0; }; fi
  if have systemctl; then systemctl stop udp-relay >/dev/null 2>&1 && { echo "udp relay stopped"; return 0; }; fi
  if have systemctl; then systemctl stop udp_relay >/dev/null 2>&1 && { echo "udp relay stopped"; return 0; }; fi
  if have service; then service udp_relay stop >/dev/null 2>&1 && { echo "udp relay stopped"; return 0; }; fi
  if have udp-relayctl; then udp-relayctl stop >/dev/null 2>&1 && { echo "udp relay stopped"; return 0; }; fi
  echo "udp relay stop unsupported on this device" 1>&2
  return 3
}

udp_relay_status(){
  if pidof udp-relay >/dev/null 2>&1; then
    echo "udp relay running"
  elif pidof udp_relay >/dev/null 2>&1; then
    echo "udp relay running"
  else
    echo "udp relay not running"
  fi
  return 0
}

validate_udp_key(){
  case "$1" in
    listen_addr|listen_port|target_addr|target_port|ttl|max_bitrate|enabled) return 0 ;;
    *) return 1 ;;
  esac
}

validate_udp_value(){
  key="$1"; val="$2"
  case "$key" in
    enabled)
      norm="$(norm_bool "$val")"; [ "$norm" != "__ERR__" ] || die "invalid bool: $key=$val"; echo "$norm" ;;
    listen_port|target_port|ttl|max_bitrate)
      norm="$(norm_uint "$val")"; [ "$norm" != "__ERR__" ] || die "invalid integer: $key=$val"; echo "$norm" ;;
    listen_addr|target_addr)
      [ -n "$val" ] || die "$key must be non-empty"; echo "$val" ;;
    *) echo "$val" ;;
  esac
}

udp_relay_get(){
  name="$1"
  [ -n "$name" ] || die "missing name"
  validate_udp_key "$name" || die "unknown setting: $name"
  udp_get_value "$name" || die "setting unavailable: $name"
}

udp_relay_set_one(){
  pair="$1"; key="${pair%%=*}"; val="${pair#*=}"
  [ -n "$key" ] || die "missing key"
  [ "$key" != "$val" ] || die "missing value"
  validate_udp_key "$key" || die "unknown setting: $key"
  norm_val="$(validate_udp_value "$key" "$val")"
  udp_set_value "$key" "$norm_val" >/dev/null || die "failed to persist $key"
}

udp_relay_params(){
  ok=1
  for kv in "$@"; do
    case "$kv" in --*) continue ;; esac
    out="$(udp_relay_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }
  done
  [ $ok -eq 1 ] || exit 2
  echo "ok"
}

udp_relay_set_cmd(){
  [ $# -ge 1 ] || die "usage: /sys/udp_relay/set key=value"
  udp_relay_set_one "$1" || exit $?
  echo "ok"
}

# ======================= Link Aggregation =======================
link_env_key(){ echo "vrx_link_mode"; }
link_get_value(){ config_get "$(link_env_key)" "$LINK_STATE_FILE"; }
link_set_value(){ config_set "$(link_env_key)" "$1" "$LINK_STATE_FILE"; }

default_link_mode(){ echo "udp_relay"; }

link_mode_get(){
  mode="$(link_get_value 2>/dev/null)"
  if [ -n "$mode" ]; then
    echo "$mode"
  else
    default_link_mode
  fi
}

link_mode_set(){
  mode="$1"
  case "$mode" in
    udp_relay|pixelpilot|none) ;;
    *) die "invalid link mode: $mode" ;;
  esac
  link_set_value "$mode" >/dev/null || die "failed to persist link mode"
  echo "ok"
}

link_route_start(){
  mode="$(link_mode_get)"
  case "$mode" in
    pixelpilot) start_pixelpilot ;;
    udp_relay)  udp_relay_start ;;
    none)       echo "link mode is none; nothing to start" ;;
    *)          echo "unknown link mode: $mode" 1>&2; return 2 ;;
  esac
}

link_route_stop(){
  mode="$(link_mode_get)"
  case "$mode" in
    pixelpilot) stop_pixelpilot ;;
    udp_relay)  udp_relay_stop ;;
    none)       echo "link mode is none; nothing to stop" ;;
    *)          echo "unknown link mode: $mode" 1>&2; return 2 ;;
  esac
}

link_route_status(){
  mode="$(link_mode_get)"
  case "$mode" in
    pixelpilot) pixelpilot_status ;;
    udp_relay)  udp_relay_status ;;
    none)       echo "link mode is none" ;;
    *)          echo "unknown link mode: $mode" 1>&2; return 2 ;;
  esac
}

# ======================= Pixelpilot Mini RK =======================
pixelpilot_mini_rk_pids(){ pidof pixelpilot_mini_rk 2>/dev/null; }

pixelpilot_mini_rk_signal(){
  action="$1"
  [ -n "$action" ] || action="action"
  pids="$(pixelpilot_mini_rk_pids)"
  if [ -z "$pids" ]; then
    echo "pixelpilot_mini_rk not running" 1>&2
    return 3
  fi
  if kill -SIGUSR1 $pids 2>/dev/null; then
    echo "$action toggled via SIGUSR1 ($pids)"
    return 0
  fi
  echo "failed to signal pixelpilot_mini_rk" 1>&2
  return 4
}

pixelpilot_mini_rk_toggle_osd(){ pixelpilot_mini_rk_signal "OSD"; }
pixelpilot_mini_rk_toggle_recording(){ pixelpilot_mini_rk_signal "Recording"; }

pixelpilot_mini_rk_reboot(){
  ( nohup sh -c 'reboot now' >/dev/null 2>&1 & )
  echo "reboot requested"
  return 0
}

# ======================= DISPATCH =======================
case "$1" in
  # general
  /sys/reboot)             shift; reboot_cmd "$@" ;;

  # pixelpilot
  /sys/pixelpilot/help)    print_help_msg "pixelpilot_help.msg" ;;
  /sys/pixelpilot/get)     shift; pixelpilot_get "$1" ;;
  /sys/pixelpilot/set)     shift; pixelpilot_set_cmd "$@" ;;
  /sys/pixelpilot/params)  shift; pixelpilot_params "$@" ;;
  /sys/pixelpilot/apply)   shift; pixelpilot_apply_cmd "$@" ;;
  /sys/pixelpilot/stop)    shift; pixelpilot_stop_cmd "$@" ;;
  /sys/pixelpilot/restart) shift; pixelpilot_restart_cmd "$@" ;;
  /sys/pixelpilot/start)   shift; pixelpilot_start_cmd "$@" ;;
  /sys/pixelpilot/status)  shift; pixelpilot_status_cmd "$@" ;;

  # udp relay
  /sys/udp_relay/help)     print_help_msg "udp_relay_help.msg" ;;
  /sys/udp_relay/get)      shift; udp_relay_get "$1" ;;
  /sys/udp_relay/set)      shift; udp_relay_set_cmd "$@" ;;
  /sys/udp_relay/params)   shift; udp_relay_params "$@" ;;
  /sys/udp_relay/start)    shift; udp_relay_start "$@" ;;
  /sys/udp_relay/stop)     shift; udp_relay_stop "$@" ;;
  /sys/udp_relay/status)   shift; udp_relay_status "$@" ;;

  # link aggregation
  /sys/link/help)          print_help_msg "link_help.msg" ;;
  /sys/link/mode)          shift; link_mode_get "$@" ;;
  /sys/link/select)        shift; link_mode_set "$1" ;;
  /sys/link/start)         shift; link_route_start "$@" ;;
  /sys/link/stop)          shift; link_route_stop "$@" ;;
  /sys/link/status)        shift; link_route_status "$@" ;;

  # pixelpilot mini rk
  /sys/pixelpilot_mini_rk/help)             print_help_msg "pixelpilot_mini_rk_help.msg" ;;
  /sys/pixelpilot_mini_rk/toggle_osd)       shift; pixelpilot_mini_rk_toggle_osd "$@" ;;
  /sys/pixelpilot_mini_rk/toggle_recording) shift; pixelpilot_mini_rk_toggle_recording "$@" ;;
  /sys/pixelpilot_mini_rk/reboot)           shift; pixelpilot_mini_rk_reboot "$@" ;;

  # utility
  /sys/ping)               shift; ping -c 1 -W 1 "$1" 2>&1 ;;

  *) echo "unknown path: $1" 1>&2; exit 2 ;;
esac
