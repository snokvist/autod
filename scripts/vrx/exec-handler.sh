#!/bin/sh
# exec-handler.sh (pixelpilot + udp relay stubs, instant-apply, external help, reboot)
# Usage: exec-handler.sh <path> [args...]
# Contract: /exec with JSON {"path":"/sys/<cap>/<command>","args":[...]}
#
# Implemented:
#   /sys/reboot                                 (schedule reboot)
#   /sys/pixelpilot/help|start|stop|toggle_record
#   /sys/pixelpilot_mini_rk/help|toggle_osd|toggle_recording|reboot|shutdown|start|stop|restart
#   /sys/udp_relay/help|start|stop|status
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
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
HELP_DIR="${HELP_DIR:-$SCRIPT_DIR}"
STATE_DIR="${VRX_STATE_DIR:-/tmp/vrx}"
LINK_STATE_FILE="$STATE_DIR/link.env"
DVR_MEDIA_DIR="${DVR_MEDIA_DIR:-/media}"

if [ "$(id -u)" -ne 0 ]; then
  echo "exec-handler.sh requires root privileges; ensure autod.service runs as root" 1>&2
  exit 4
fi

# ======================= Helpers =======================
die(){ echo "$*" 1>&2; exit 2; }
have(){ command -v "$1" >/dev/null 2>&1; }
ensure_parent_dir(){ dir="$1"; [ -d "$dir" ] || mkdir -p "$dir"; }

# print external help JSON (fallback to simple error payload)
print_help_msg(){
  name="$1"
  for base in "$HELP_DIR" "$SCRIPT_DIR" "/etc/autod"; do
    [ -n "$base" ] || continue
    file="$base/$name"
    if [ -r "$file" ]; then
      cat "$file"
      return 0
    fi
  done
  echo "{\"error\":\"help file not found\",\"name\":\"$name\"}"
  return 4
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

# ======================= General =======================
reboot_cmd(){
  ( nohup sh -c 'sleep 1; reboot now' >/dev/null 2>&1 & )
  echo "reboot scheduled"
  return 0
}

# ======================= Pixelpilot =======================
pixelpilot_pids(){ pidof pixelpilot 2>/dev/null; }

start_pixelpilot(){
  if ! have systemctl; then
    echo "systemctl unavailable" 1>&2
    return 3
  fi
  if systemctl start openipc >/dev/null 2>&1; then
    echo "openipc started"
    return 0
  fi
  echo "failed to start openipc" 1>&2
  return 4
}

stop_pixelpilot(){
  if ! have systemctl; then
    echo "systemctl unavailable" 1>&2
    return 3
  fi
  if systemctl stop openipc >/dev/null 2>&1; then
    echo "openipc stopped"
    return 0
  fi
  echo "failed to stop openipc" 1>&2
  return 4
}

toggle_pixelpilot_record(){
  pids="$(pixelpilot_pids)"
  if [ -z "$pids" ]; then
    echo "pixelpilot not running" 1>&2
    return 3
  fi
  if kill -SIGUSR $pids 2>/dev/null; then
    echo "toggled pixelpilot recording via SIGUSR ($pids)"
    return 0
  fi
  echo "failed to signal pixelpilot" 1>&2
  return 4
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

pixelpilot_start_cmd(){ start_pixelpilot; }
pixelpilot_stop_cmd(){ stop_pixelpilot; }
pixelpilot_toggle_record_cmd(){ toggle_pixelpilot_record; }

# ======================= UDP Relay =======================
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

# ======================= DVR recordings =======================
dvr_list(){
  dir="$DVR_MEDIA_DIR"
  if [ ! -d "$dir" ]; then
    echo "media directory not found: $dir" 1>&2
    return 3
  fi
  set -- "$dir"/*.mp4
  if [ "$1" = "$dir"'/'"*.mp4" ]; then
    return 0
  fi
  tmp="$(mktemp 2>/dev/null)"
  if [ -z "$tmp" ]; then
    tmp="/tmp/dvr_list.$$"
  fi
  : > "$tmp" 2>/dev/null || { echo "failed to create temp file" 1>&2; return 4; }
  for file in "$@"; do
    [ -f "$file" ] || continue
    name="${file##*/}"
    size=""
    mtime=""
    if have stat; then
      size="$(stat -c '%s' "$file" 2>/dev/null)"
      [ -n "$size" ] || size="$(stat -f '%z' "$file" 2>/dev/null)"
      mtime="$(stat -c '%Y' "$file" 2>/dev/null)"
      [ -n "$mtime" ] || mtime="$(stat -f '%m' "$file" 2>/dev/null)"
    fi
    if [ -z "$size" ]; then
      size="$(wc -c < "$file" 2>/dev/null)"
    fi
    if [ -z "$mtime" ]; then
      mtime="$(date -r "$file" +%s 2>/dev/null)"
    fi
    case "$size" in
      ''|*[!0-9]*) size=0 ;;
    esac
    case "$mtime" in
      ''|*[!0-9]*) mtime=0 ;;
    esac
    printf '%s\t%s\t%s\n' "$mtime" "$size" "$name" >>"$tmp"
  done
  if [ -s "$tmp" ]; then
    sort -rn "$tmp" | while IFS="$(printf '\t')" read -r mtime size name; do
      [ -n "$name" ] || continue
      printf '%s\t%s\t%s\n' "$name" "$size" "$mtime"
    done
  fi
  rm -f "$tmp"
  return 0
}

dvr_delete_all(){
  dir="$DVR_MEDIA_DIR"
  if [ ! -d "$dir" ]; then
    echo "media directory not found: $dir" 1>&2
    return 3
  fi
  count=0
  for file in "$dir"/*.mp4; do
    if [ "$file" = "$dir"'/'"*.mp4" ]; then
      break
    fi
    count=$((count + 1))
  done
  if [ "$count" -eq 0 ]; then
    echo "no recordings to delete"
    return 0
  fi
  if find "$dir" -maxdepth 1 -type f -name '*.mp4' -exec rm -f -- {} \;; then
    echo "deleted $count recording(s)"
    return 0
  fi
  echo "failed to delete recordings" 1>&2
  return 4
}

# ======================= Pixelpilot Mini RK =======================
pixelpilot_mini_rk_pids(){ pidof pixelpilot_mini_rk 2>/dev/null; }

send_pid_signal(){
  pid="$1"
  signal="$2"
  if kill -"$signal" "$pid" 2>/dev/null; then
    return 0
  fi
  short_sig="$signal"
  case "$short_sig" in
    SIG*) short_sig="${short_sig#SIG}" ;;
  esac
  if [ "$short_sig" != "$signal" ] && kill -"$short_sig" "$pid" 2>/dev/null; then
    return 0
  fi
  return 1
}

pixelpilot_mini_rk_signal(){
  signal="$1"
  action="$2"
  [ -n "$signal" ] || signal="SIGUSR1"
  [ -n "$action" ] || action="action"
  pids="$(pixelpilot_mini_rk_pids)"
  if [ -z "$pids" ]; then
    echo "pixelpilot_mini_rk not running" 1>&2
    return 3
  fi
  success_pids=""
  active_but_failed_pids=""
  missing_pids=""
  for pid in $pids; do
    if send_pid_signal "$pid" "$signal"; then
      success_pids="$success_pids $pid"
      continue
    fi
    if kill -0 "$pid" 2>/dev/null; then
      active_but_failed_pids="$active_but_failed_pids $pid"
      continue
    fi
    if [ -d "/proc/$pid" ]; then
      active_but_failed_pids="$active_but_failed_pids $pid"
    else
      missing_pids="$missing_pids $pid"
    fi
  done

  success_pids="${success_pids# }"
  active_but_failed_pids="${active_but_failed_pids# }"
  missing_pids="${missing_pids# }"

  if [ -n "$success_pids" ]; then
    echo "$action toggled via $signal ($success_pids)"
    if [ -n "$active_but_failed_pids" ]; then
      echo "pixelpilot_mini_rk running but $signal delivery failed (pid $active_but_failed_pids)" 1>&2
    fi
    if [ -n "$missing_pids" ]; then
      echo "pixelpilot_mini_rk pid(s) exited before they could be signalled ($missing_pids)" 1>&2
    fi
    return 0
  fi

  if [ -n "$active_but_failed_pids" ]; then
    echo "pixelpilot_mini_rk running but $signal delivery failed (pid $active_but_failed_pids)" 1>&2
    return 4
  fi

  if [ -n "$missing_pids" ]; then
    echo "pixelpilot_mini_rk exited before it could be signalled ($missing_pids)" 1>&2
    echo "failed to signal pixelpilot_mini_rk with $signal" 1>&2
    return 4
  fi

  echo "failed to signal pixelpilot_mini_rk with $signal" 1>&2
  return 4
}

pixelpilot_mini_rk_toggle_osd(){ pixelpilot_mini_rk_signal SIGUSR1 "OSD"; }
pixelpilot_mini_rk_toggle_recording(){ pixelpilot_mini_rk_signal SIGUSR2 "Recording"; }

pixelpilot_mini_rk_reboot(){
  ( nohup sh -c 'reboot now' >/dev/null 2>&1 & )
  echo "reboot requested"
  return 0
}

pixelpilot_mini_rk_shutdown(){
  ( nohup sh -c 'shutdown now' >/dev/null 2>&1 & )
  echo "shutdown requested"
  return 0
}

pixelpilot_mini_rk_unit_candidates(){
  echo "pixelpilot_mini_rk.service"
  echo "pixelpilot_mini_rk"
}

pixelpilot_mini_rk_start(){
  if have systemctl; then
    for unit in $(pixelpilot_mini_rk_unit_candidates); do
      if systemctl start "$unit" >/dev/null 2>&1; then
        echo "pixelpilot_mini_rk started"
        return 0
      fi
    done
  fi
  if have service; then
    if service pixelpilot_mini_rk start >/dev/null 2>&1; then
      echo "pixelpilot_mini_rk started"
      return 0
    fi
  fi
  echo "pixelpilot_mini_rk start unsupported on this device" 1>&2
  return 3
}

pixelpilot_mini_rk_stop(){
  if have systemctl; then
    for unit in $(pixelpilot_mini_rk_unit_candidates); do
      if systemctl stop "$unit" >/dev/null 2>&1; then
        echo "pixelpilot_mini_rk stopped"
        return 0
      fi
    done
  fi
  if have service; then
    if service pixelpilot_mini_rk stop >/dev/null 2>&1; then
      echo "pixelpilot_mini_rk stopped"
      return 0
    fi
  fi
  echo "pixelpilot_mini_rk stop unsupported on this device" 1>&2
  return 3
}

pixelpilot_mini_rk_restart(){
  if have systemctl; then
    for unit in $(pixelpilot_mini_rk_unit_candidates); do
      if systemctl restart "$unit" >/dev/null 2>&1; then
        echo "pixelpilot_mini_rk restarted"
        return 0
      fi
    done
  fi
  if pixelpilot_mini_rk_stop; then
    sleep 1
    if pixelpilot_mini_rk_start; then
      echo "pixelpilot_mini_rk restarted"
      return 0
    fi
  fi
  echo "pixelpilot_mini_rk restart unsupported on this device" 1>&2
  return 3
}

# ======================= DISPATCH =======================
case "$1" in
  # general
  /sys/reboot)             shift; reboot_cmd "$@" ;;

  # pixelpilot
  /sys/pixelpilot/help)           print_help_msg "pixelpilot_help.msg" ;;
  /sys/pixelpilot/start)          shift; pixelpilot_start_cmd "$@" ;;
  /sys/pixelpilot/stop)           shift; pixelpilot_stop_cmd "$@" ;;
  /sys/pixelpilot/toggle_record)  shift; pixelpilot_toggle_record_cmd "$@" ;;

  # udp relay
  /sys/udp_relay/help)     print_help_msg "udp_relay_help.msg" ;;
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

  # dvr recordings
  /sys/dvr/list)           shift; dvr_list "$@" ;;
  /sys/dvr/delete_all)     shift; dvr_delete_all "$@" ;;

  # pixelpilot mini rk
  /sys/pixelpilot_mini_rk/help)             print_help_msg "pixelpilot_mini_rk_help.msg" ;;
  /sys/pixelpilot_mini_rk/toggle_osd)       shift; pixelpilot_mini_rk_toggle_osd "$@" ;;
  /sys/pixelpilot_mini_rk/toggle_recording) shift; pixelpilot_mini_rk_toggle_recording "$@" ;;
  /sys/pixelpilot_mini_rk/reboot)           shift; pixelpilot_mini_rk_reboot "$@" ;;
  /sys/pixelpilot_mini_rk/shutdown)         shift; pixelpilot_mini_rk_shutdown "$@" ;;
  /sys/pixelpilot_mini_rk/start)            shift; pixelpilot_mini_rk_start "$@" ;;
  /sys/pixelpilot_mini_rk/stop)             shift; pixelpilot_mini_rk_stop "$@" ;;
  /sys/pixelpilot_mini_rk/restart)          shift; pixelpilot_mini_rk_restart "$@" ;;

  # utility
  /sys/ping)               shift; ping -c 1 -W 1 "$1" 2>&1 ;;

  *) echo "unknown path: $1" 1>&2; exit 2 ;;
esac
