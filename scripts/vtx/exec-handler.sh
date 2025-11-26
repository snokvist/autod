#!/bin/sh
# exec-handler.sh (video + link stubs, instant-apply)
# Usage: exec-handler.sh <path> [args...]
# Contract: /exec with JSON {"path":"/sys/<cap>/<command>","args":[...]}
#
# Implemented:
#   /sys/video/help|get|set|params|apply|stop|restart|start(alias→restart)
#   /sys/link/help
#   /sys/link/start|stop|status (routes using wifi_mode)
#   /sys/link/select <wfb_ng|ap|sta>  (sets wifi_mode)
#   /sys/link/wifi/help|get|set|params|start|stop|status
#   /sys/link/wfb_ng/help|get|set|params|start|stop|status
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

# ======================= VIDEO (majestic) =======================
majestic_pids(){ pidof majestic 2>/dev/null; }

sighup_majestic(){
  pids="$(majestic_pids)"
  [ -n "$pids" ] || return 3
  kill -HUP $pids 2>/dev/null || return 4
  return 0
}

restart_majestic(){
  if [ -x /etc/init.d/S95majestic ]; then /etc/init.d/S95majestic restart >/dev/null 2>&1; return $?; fi
  return 127
}

stop_majestic(){
  if [ -x /etc/init.d/S95majestic ]; then /etc/init.d/S95majestic stop >/dev/null 2>&1; return $?; fi
  return 127
}

apply_video_settings(){
  if sighup_majestic; then echo "applied via SIGHUP"; return 0; fi
  if restart_majestic; then echo "applied via restart"; return 0; fi
  echo "apply failed: majestic not running and no restart method" 1>&2
  return 3
}

# mapping (video keys)
map_cli_key(){
  case "$1" in
    codec)            echo ".video0.codec" ;;
    fps)              echo ".video0.fps" ;;
    bitrate)          echo ".video0.bitrate" ;;
    rcMode)           echo ".video0.rcMode" ;;
    gopSize)          echo ".video0.gopSize" ;;
    size)             echo ".video0.size" ;;
    exposure)         echo ".isp.exposure" ;;
    mirror)           echo ".image.mirror" ;;
    flip)             echo ".image.flip" ;;
    contrast)         echo ".image.contrast" ;;
    hue)              echo ".image.hue" ;;
    saturation)       echo ".image.saturation" ;;
    luminance)        echo ".image.luminance" ;;
    outgoing_enabled) echo ".outgoing.enabled" ;;
    outgoing_server)  echo ".outgoing.server" ;;
    .* )              echo "$1" ;;
    *)                return 1 ;;
  esac
}

# normalization
norm_bool(){
  v=$(echo "$1" | tr 'A-Z' 'a-z')
  case "$v" in
    1|true|on|yes)  echo true ;;
    0|false|off|no) echo false ;;
    *)             echo "__ERR__" ;;
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

is_ipv4_octet(){
  case "$1" in
    25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9]) return 0 ;;
    *) return 1 ;;
  esac
}

validate_udp_uri(){
  uri="$1"
  case "$uri" in
    udp://*:* ) ;;
    *) return 1 ;;
  esac
  rest=${uri#udp://}
  host=${rest%%:*}
  port=${rest#*:}
  [ -n "$host" ] && [ -n "$port" ] || return 1
  case "$host" in
    *[!0-9.]* ) return 1 ;;
  esac
  case "$port" in
    *[!0-9]*) return 1 ;;
    [1-9]) ;;
    [1-9][0-9]) ;;
    [1-9][0-9][0-9]) ;;
    [1-9][0-9][0-9][0-9]) ;;
    [1-5][0-9][0-9][0-9][0-9]) ;;
    6[0-4][0-9][0-9][0-9]) ;;
    65[0-4][0-9][0-9]) ;;
    655[0-2][0-9]) ;;
    6553[0-5]) ;;
    *) return 1 ;;
  esac
  oldifs=$IFS
  IFS=.
  set -- $host
  IFS=$oldifs
  [ $# -eq 4 ] || return 1
  for oct in "$@"; do is_ipv4_octet "$oct" || return 1; done
  return 0
}

# device CLI wrappers
cli_get(){ cli -g "$1"; }
cli_set(){ cli -s "$1" "$2"; }

# video commands
video_help_json(){ emit_msg "video_help.msg"; }

video_get(){ name="$1"; [ -n "$name" ] || die "missing name"; cli_key="$(map_cli_key "$name")" || die "unknown setting: $name"; cli_get "$cli_key"; }

video_set_one(){
  pair="$1"; key="${pair%%=*}"; val="${pair#*=}"
  [ -n "$key" ] || die "missing key in pair"; [ "$key" != "$val" ] || die "missing value in pair"
  case "$key" in
    mirror|flip|outgoing_enabled)
                        n="$(norm_bool "$val")"; [ "$n" != "__ERR__" ] || die "invalid bool: $key=$val"; val="$n" ;;
    bitrate)            n="$(norm_bitrate "$val")"; [ "$n" != "__ERR__" ] || die "invalid bitrate: $val"; val="$n" ;;
    outgoing_server)    validate_udp_uri "$val" || die "invalid outgoing server: $val" ;;
  esac
  cli_key="$(map_cli_key "$key")" || die "unknown setting: $key"
  cli_set "$cli_key" "$val" || die "set failed: $key=$val"
}

video_params(){
  ok=1
  for kv in "$@"; do case "$kv" in --*) continue;; esac; out="$(video_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }; done
  [ $ok -eq 1 ] || exit 2
  apply_video_settings || exit $?
  echo "ok"
}

video_set_cmd(){ [ $# -ge 1 ] || die "usage: /sys/video/set key=value"; video_set_one "$1" || exit $?; apply_video_settings || exit $?; echo "ok"; }
video_apply_cmd(){ apply_video_settings; }
video_stop_cmd(){ if stop_majestic; then echo "stopped"; exit 0; fi; echo "stop failed: no stop method" 1>&2; exit 3; }
video_restart_cmd(){ if restart_majestic; then echo "restarted"; exit 0; fi; echo "restart failed: no restart method" 1>&2; exit 3; }
video_start_cmd(){ video_restart_cmd; }

# ======================= LINK (wifi_mode-driven) =======================
# Active link type is held in wifi_mode (fw_printenv -n wifi_mode).
# Values: wfb_ng | ap | sta. Routing: wfb_ng → wfb; ap/sta → wifi.

wifi_mode_get(){
  if have fw_printenv; then fw_printenv -n wifi_mode 2>/dev/null; else echo "sta"; fi
}

wifi_mode_set(){
  mode="$1"
  case "$mode" in wfb_ng|ap|sta) ;; *) die "invalid wifi_mode: $mode";; esac
  if have fw_setenv; then fw_setenv wifi_mode "$mode" >/dev/null 2>&1 || die "failed to set wifi_mode"; echo "ok"; else die "fw_setenv not available"; fi
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
  # placeholder (hardcoded simple status string)
  echo "wifi: status unavailable (placeholder)"
  return 0
}

wifi_get(){ name="$1"; [ -n "$name" ] || die "missing name"; link_param_get "$name"; }
wifi_set_one(){ pair="$1"; key="${pair%%=*}"; val="${pair#*=}"; [ -n "$key" ] || die "missing key"; [ "$key" != "$val" ] || die "missing value"; link_param_set "$key" "$val" >/dev/null || exit $?; }
wifi_params(){ ok=1; for kv in "$@"; do case "$kv" in --*) continue;; esac; out="$(wifi_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }; done; [ $ok -eq 1 ] || exit 2; echo "ok"; }

# WFB-NG stubs
wfb_help_json(){ emit_msg "wfb_help.msg"; }

wfb_start(){
  if [ -x /etc/init.d/S95wfb_ng ]; then /etc/init.d/S95wfb_ng start >/dev/null 2>&1 && { echo "wfb_ng started"; return 0; } fi
  if [ -x /etc/init.d/S95wfb-ng ]; then /etc/init.d/S95wfb-ng start >/dev/null 2>&1 && { echo "wfb_ng started"; return 0; } fi
  echo "wfb_ng start unsupported on this device" 1>&2; return 3
}

wfb_stop(){
  if [ -x /etc/init.d/S95wfb_ng ]; then /etc/init.d/S95wfb_ng stop >/dev/null 2>&1 && { echo "wfb_ng stopped"; return 0; } fi
  if [ -x /etc/init.d/S95wfb-ng ]; then /etc/init.d/S95wfb-ng stop >/dev/null 2>&1 && { echo "wfb_ng stopped"; return 0; } fi
  echo "wfb_ng stop unsupported on this device" 1>&2; return 3
}

wfb_status(){
  # placeholder
  echo "wfb_ng: status unavailable (placeholder)"
  return 0
}

wfb_get(){ name="$1"; [ -n "$name" ] || die "missing name"; link_param_get "$name"; }
wfb_set_one(){ pair="$1"; key="${pair%%=*}"; val="${pair#*=}"; [ -n "$key" ] || die "missing key"; [ "$key" != "$val" ] || die "missing value"; link_param_set "$key" "$val" >/dev/null || exit $?; }
wfb_params(){ ok=1; for kv in "$@"; do case "$kv" in --*) continue;; esac; out="$(wfb_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }; done; [ $ok -eq 1 ] || exit 2; echo "ok"; }

# Overall link control using wifi_mode
link_help_json(){ emit_msg "link_help.msg"; }

link_route_start(){
  mode="$(wifi_mode_get)"
  case "$mode" in
    wfb_ng) wfb_start ;;
    ap|sta|*) wifi_start ;;  # default to WiFi for any non-wfb_ng
  esac
}

link_route_stop(){
  mode="$(wifi_mode_get)"
  case "$mode" in
    wfb_ng) wfb_stop ;;
    ap|sta|*) wifi_stop ;;
  esac
}

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

# ======================= Firmware env helpers =======================
fw_help_json(){ emit_msg "fw_help.msg"; }

fw_env_set(){
  key="$1"
  val="$2"
  if have fw_setenv; then fw_setenv "$key" "$val" >/dev/null 2>&1 || die "failed to set $key"; else die "fw_setenv not available"; fi
}

fw_supported_key(){
  case "$1" in link_mode|wlanssid|msposd_tty|wlanpass) return 0 ;; *) return 1 ;; esac
}

fw_get(){
  name="$1"
  [ -n "$name" ] || die "missing name"
  fw_supported_key "$name" || die "unknown setting: $name"
  have fw_printenv || die "fw_printenv not available"
  out=$(fw_printenv "$name" 2>&1) || { echo "$out" 1>&2; exit 2; }
  echo "$out"
}

fw_set_one(){
  pair="$1"
  key="${pair%%=*}"
  val="${pair#*=}"
  [ -n "$key" ] || die "missing key in pair"
  [ "$key" != "$val" ] || die "missing value in pair"
  case "$key" in
    link_mode)
      case "$val" in ""|apfpv|wfb) ;; *) die "invalid link_mode: $val" ;; esac ;;
    wlanssid)
      validate_alnum_8_24 "$val" || die "invalid wlanssid: must be empty or 8-24 alphanumerics" ;;
    msposd_tty)
      case "$val" in standalone|/dev/ttyS1|/dev/ttyS2) ;; *) die "invalid msposd_tty: $val" ;; esac ;;
    wlanpass)
      validate_alnum_8_24 "$val" || die "invalid wlanpass: must be empty or 8-24 alphanumerics" ;;
    *)
      die "unknown setting: $key" ;;
  esac
  fw_env_set "$key" "$val"
  echo "ok"
}

fw_set_cmd(){ [ $# -ge 1 ] || die "usage: /sys/fw/set key=value"; fw_set_one "$1"; }

fw_params(){
  ok=1
  for kv in "$@"; do case "$kv" in --*) continue;; esac; out="$(fw_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }; done
  [ $ok -eq 1 ] || exit 2
  echo "ok"
}

validate_alnum_8_24(){
  val="$1"
  [ -z "$val" ] && return 0
  case "$val" in *[!A-Za-z0-9]*) return 1 ;; esac
  len=${#val}
  [ $len -ge 8 ] && [ $len -le 24 ]
}

# ======================= OSD (antenna overlay) =======================
osd_help_json(){ emit_msg "osd_help.msg"; }

osd_run_service(){
  action="$1"
  if [ -x /etc/init.d/S96antenna_osd ]; then /etc/init.d/S96antenna_osd "$action" >/dev/null 2>&1 && return 0; fi
  return 127
}

osd_start(){
  if osd_run_service start; then echo "osd started"; return 0; fi
  echo "osd start unsupported on this device" 1>&2
  return 3
}

osd_stop(){
  if osd_run_service stop; then echo "osd stopped"; return 0; fi
  echo "osd stop unsupported on this device" 1>&2
  return 3
}

osd_restart(){
  if osd_run_service restart; then echo "osd restarted"; return 0; fi
  echo "osd restart unsupported on this device" 1>&2
  return 3
}

osd_toggle_pause(){
  if osd_run_service reload; then echo "osd pause toggled"; return 0; fi
  echo "osd toggle_pause unsupported on this device" 1>&2
  return 3
}

osd_status(){
  echo "osd: status unavailable (placeholder)"
  return 0
}

# ======================= DISPATCH =======================
case "$1" in
  # system
  /sys/help)            sys_help_json ;;
  /sys/fw/help)         fw_help_json ;;
  /sys/fw/get)          shift; fw_get "$1" ;;
  /sys/fw/set)          shift; fw_set_cmd "$@" ;;
  /sys/fw/params)       shift; fw_params "$@" ;;
  /sys/shutdown)        shift; sys_shutdown_cmd "$@" ;;
  /sys/restart)         shift; sys_restart_cmd "$@" ;;

  # osd
  /sys/osd/help)        osd_help_json ;;
  /sys/osd/start)       shift; osd_start "$@" ;;
  /sys/osd/stop)        shift; osd_stop "$@" ;;
  /sys/osd/restart)     shift; osd_restart "$@" ;;
  /sys/osd/toggle_pause) shift; osd_toggle_pause "$@" ;;
  /sys/osd/status)      shift; osd_status "$@" ;;

  # video
  /sys/video/help)      video_help_json ;;
  /sys/video/get)       shift; video_get "$1" ;;
  /sys/video/set)       shift; video_set_cmd "$@" ;;
  /sys/video/params)    shift; video_params "$@" ;;
  /sys/video/apply)     shift; video_apply_cmd "$@" ;;
  /sys/video/stop)      shift; video_stop_cmd "$@" ;;
  /sys/video/restart)   shift; video_restart_cmd "$@" ;;
  /sys/video/start)     shift; video_start_cmd "$@" ;;

  # link overall (wifi_mode-driven)
  /sys/link/help)       link_help_json ;;
  /sys/link/select)     shift; wifi_mode_set "$1" ;;
  /sys/link/start)      shift; link_route_start "$@" ;;
  /sys/link/stop)       shift; link_route_stop "$@" ;;
  /sys/link/status)     shift; link_route_status "$@" ;;

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
  /sys/link/wfb_ng/status) shift; wfb_status "$@" ;;

  # utility
  /sys/ping)               shift; ping -c 1 -W 1 "$1" 2>&1 ;;

  *) echo "unknown path: $1" 1>&2; exit 2 ;;
esac
