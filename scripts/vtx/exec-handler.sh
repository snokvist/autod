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
    outgoing_server)  echo ".outgoing.server" ;;
    .* )              echo "$1" ;;
    *)                return 1 ;;
  esac
}

# normalization
norm_bool(){
  v=$(echo "$1" | tr 'A-Z' 'a-z')
  case "$v" in 1|true|on|yes) echo 1;; 0|false|off|no) echo 0;; *) echo "__ERR__";; esac
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
video_help_json(){
  cat <<'JSON'
{
  "cap": "video",
  "contract_version": "0.2",
  "commands": [
    {"name":"get","description":"Read a single setting","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["codec","fps","bitrate","rcMode","gopSize","size","exposure","mirror","flip","contrast","hue","saturation","luminance","outgoing_server"],"multi":false},"required":true}]},
    {"name":"set","description":"Set one setting (key=value) and apply instantly","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true,"description":"key=value"}]},
    {"name":"params","description":"Set multiple settings and apply instantly","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false,"description":"Repeated key=value tokens"}]},
    {"name":"apply","description":"Re-read settings without changing values (SIGHUP → restart fallback)","args":[]},
    {"name":"stop","description":"Stop the streaming service","args":[]},
    {"name":"restart","description":"Restart the streaming service (stop→start)","args":[]},
    {"name":"start","description":"Alias of restart (ensures a clean start)","args":[]},
    {"name":"help","description":"Describe available video settings and UI hints","args":[]}
  ],
  "settings": [
    {"key":"codec","type":"enum","required":false,"default":"h265","description":"Video codec","control":{"kind":"select","options":["h265","h264"],"multi":false}},
    {"key":"fps","type":"enum","required":false,"default":60,"description":"Frames per second","control":{"kind":"select","options":[30,60,90,120],"multi":false}},
    {"key":"bitrate","type":"int","required":false,"default":12544,"description":"Target bitrate (kbps)","control":{"kind":"range","min":2048,"max":20480,"step":512,"unit":"kbps"}},
    {"key":"rcMode","type":"enum","required":false,"default":"cbr","description":"Rate control mode","control":{"kind":"select","options":["cbr","vbr","avbr"],"multi":false}},
    {"key":"gopSize","type":"float","required":false,"default":10,"description":"GOP size (seconds)","control":{"kind":"range","min":0.5,"max":10,"step":0.5,"unit":"s"}},
    {"key":"size","type":"enum","required":false,"default":"1280x720","description":"Frame size","control":{"kind":"select","options":["960x540","1280x720","1920x1080"],"multi":false}},
    {"key":"exposure","type":"int","required":false,"default":7,"description":"ISP exposure","control":{"kind":"range","min":5,"max":32,"step":1}},
    {"key":"mirror","type":"bool","required":false,"default":false,"description":"Horizontal mirror","control":{"kind":"toggle"}},
    {"key":"flip","type":"bool","required":false,"default":false,"description":"Vertical flip","control":{"kind":"toggle"}},
    {"key":"contrast","type":"int","required":false,"default":50,"description":"Image contrast","control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"hue","type":"int","required":false,"default":50,"description":"Image hue","control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"saturation","type":"int","required":false,"default":50,"description":"Image saturation","control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"luminance","type":"int","required":false,"default":50,"description":"Image luminance","control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"outgoing_server","type":"string","required":false,"default":"udp://224.0.0.1:5600","description":"Primary output (udp://{ip}:{port})","control":{"kind":"text","placeholder":"udp://192.168.2.20:5600"}}
  ]
}
JSON
}

video_get(){ name="$1"; [ -n "$name" ] || die "missing name"; cli_key="$(map_cli_key "$name")" || die "unknown setting: $name"; cli_get "$cli_key"; }

video_set_one(){
  pair="$1"; key="${pair%%=*}"; val="${pair#*=}"
  [ -n "$key" ] || die "missing key in pair"; [ "$key" != "$val" ] || die "missing value in pair"
  case "$key" in
    mirror|flip)        n="$(norm_bool "$val")"; [ "$n" != "__ERR__" ] || die "invalid bool: $key=$val"; val="$n" ;;
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
wifi_help_json(){
  cat <<'JSON'
{
  "cap":"link.wifi",
  "contract_version":"0.2",
  "commands":[
    {"name":"get","description":"Read a wifi parameter","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["wlanpwr","wlanpass","wlanchan","wlanssid","wifi_mode"],"multi":false},"required":true}]},
    {"name":"set","description":"Set one wifi parameter (key=value)","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true}]},
    {"name":"params","description":"Set multiple wifi parameters","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false}]},
    {"name":"start","description":"Start WiFi link","args":[]},
    {"name":"stop","description":"Stop WiFi link","args":[]},
    {"name":"status","description":"WiFi link status (placeholder)","args":[]},
    {"name":"help","description":"Describe WiFi controls","args":[]}
  ],
  "settings":[
    {"key":"wlanpwr","type":"int","control":{"kind":"range","min":100,"max":3100,"step":100},"description":"TX power (arbitrary units)"},
    {"key":"wlanpass","type":"string","control":{"kind":"select","options":["Enter value..."],"multi":false,"allow_free":true},"description":"WPA2 passphrase"},
    {"key":"wlanchan","type":"int","control":{"kind":"select","options":[36,40,44,48,149,153,157,161,165],"multi":false,"allow_free":false},"description":"Non-DFS 5GHz channel"},
    {"key":"wlanssid","type":"string","control":{"kind":"select","options":["Drone","Enter value..."],"multi":false,"allow_free":true},"description":"SSID"},
    {"key":"wifi_mode","type":"enum","control":{"kind":"select","options":["wfb_ng","ap","sta"],"multi":false,"allow_free":true},"description":"Active link type / mode"}
  ]
}
JSON
}

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
wfb_help_json(){
  cat <<'JSON'
{
  "cap":"link.wfb_ng",
  "contract_version":"0.2",
  "commands":[
    {"name":"get","description":"Read a WFB-NG parameter","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["wlanpwr","wlanchan","wifi_mode"],"multi":false},"required":true}]},
    {"name":"set","description":"Set one WFB-NG parameter (key=value)","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true}]},
    {"name":"params","description":"Set multiple WFB-NG parameters","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false}]},
    {"name":"start","description":"Start WFB-NG link","args":[]},
    {"name":"stop","description":"Stop WFB-NG link","args":[]},
    {"name":"status","description":"WFB-NG link status (placeholder)","args":[]},
    {"name":"help","description":"Describe WFB-NG controls","args":[]}
  ]
}
JSON
}

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
link_help_json(){
  cat <<'JSON'
{
  "cap":"link",
  "contract_version":"0.2",
  "commands":[
    {"name":"select","description":"Set wifi_mode (wfb_ng|ap|sta) which determines the active link","args":[{"key":"type","type":"enum","control":{"kind":"select","options":["wfb_ng","ap","sta"],"multi":false,"allow_free":true},"required":true}]},
    {"name":"start","description":"Start the active link determined by wifi_mode","args":[]},
    {"name":"stop","description":"Stop the active link determined by wifi_mode","args":[]},
    {"name":"status","description":"Status of the active link (placeholder)","args":[]},
    {"name":"help","description":"Describe overall link controls","args":[]}
  ]
}
JSON
}

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

# ======================= DISPATCH =======================
case "$1" in
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
