#!/bin/sh
# exec-handler.sh (video-focused edition with instant-apply)
# Usage: exec-handler.sh <path> [args...]
# Contract: /exec with JSON {"path":"/sys/<cap>/<command>","args":[...]}
# This script routes /sys/video/* and generic utilities.
# It applies video settings immediately by sending SIGHUP to 'majestic' and
# falls back to restarting the service if SIGHUP fails or the process isn't running.
#
# Exit codes (recommendation):
#   0   success
#   2   usage/validation error
#   3   unsupported/not running/state issue
#   4   resource not found
#   124 timeout (reserved by daemon)
# >128  signal termination
#
# Notes:
# - Keep parsing cheap: argv only; no JSON parsing here.
# - Print machine-readable JSON on stdout only for /help; human text otherwise.
# - Errors go to stderr.

PATH=/usr/sbin:/usr/bin:/sbin:/bin

die(){ echo "$*" 1>&2; exit 2; }

# ---------- process control (majestic) ----------
majestic_pids() {
  pidof majestic 2>/dev/null
}

sighup_majestic() {
  pids="$(majestic_pids)"
  [ -n "$pids" ] || return 3    # not running
  kill -HUP $pids 2>/dev/null || return 4
  return 0
}

restart_majestic() {
  if [ -x /etc/init.d/S95majestic ]; then
    /etc/init.d/S95majestic restart >/dev/null 2>&1
    return $?
  fi
  return 127
}

apply_video_settings() {
  # Try a quick SIGHUP first
  if sighup_majestic; then
    echo "applied via SIGHUP"
    return 0
  fi
  # Fallback to restart (usually safest to re-read full config)
  if restart_majestic; then
    echo "applied via restart"
    return 0
  fi
  echo "apply failed: majestic not running and no restart method" 1>&2
  return 3
}

# ---------- mapping (logical key -> device CLI key) ----------
map_cli_key() {
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
    # allow raw dotted keys to pass through for power users:
    .* )              echo "$1" ;;
    *)                return 1 ;;
  esac
}

# ---------- normalization helpers ----------
norm_bool() {
  v=$(echo "$1" | tr 'A-Z' 'a-z')
  case "$v" in
    1|true|on|yes)  echo 1 ;;
    0|false|off|no) echo 0 ;;
    *)              echo "__ERR__" ;;
  esac
}

# bitrate normalization (expects kbps for cli; accepts 4000k/4M/etc.)
norm_bitrate() {
  v="$1"
  case "$v" in
    *[!0-9kKmM]*) echo "__ERR__" ;;   # reject weird chars
    *[kK])        echo "${v%[kK]}" ;;
    *[mM])        echo $(( ${v%[mM]} * 1000 )) ;;
    *)            echo "$v" ;;
  esac
}

# ---------- device CLI wrappers ----------
cli_get() { cli -g "$1"; }
cli_set() { cli -s "$1" "$2"; }

# ---------- /sys/video/help ----------
video_help_json() {
  cat <<'JSON'
{
  "cap": "video",
  "contract_version": "0.2",
  "commands": [
    { "name":"get",
      "description":"Read a single setting",
      "args":[{"key":"name","type":"enum","control":{"kind":"select","options":["codec","fps","bitrate","rcMode","gopSize","size","exposure","mirror","flip","contrast","hue","saturation","luminance","outgoing_server"],"multi":false},"required":true}]
    },
    { "name":"set",
      "description":"Set one setting (single key=value) and apply instantly",
      "args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true,"description":"key=value"}]
    },
    { "name":"params",
      "description":"Set multiple settings and apply instantly",
      "args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false,"description":"Repeated key=value tokens"}]
    },
    { "name":"apply",
      "description":"Re-read settings without changing values (SIGHUP → restart fallback)",
      "args":[]
    },
    { "name":"help",
      "description":"Describe available video settings and UI hints",
      "args":[]
    }
  ],
  "settings": [
    {"key":"codec","type":"enum","required":false,"default":"h265","description":"Video codec",
      "control":{"kind":"select","options":["h265","h264"],"multi":false}},
    {"key":"fps","type":"enum","required":false,"default":60,"description":"Frames per second",
      "control":{"kind":"select","options":[30,60,90,120],"multi":false}},
    {"key":"bitrate","type":"int","required":false,"default":12544,"description":"Target bitrate (kbps)",
      "control":{"kind":"range","min":2048,"max":20480,"step":512,"unit":"kbps"}},
    {"key":"rcMode","type":"enum","required":false,"default":"cbr","description":"Rate control mode",
      "control":{"kind":"select","options":["cbr","vbr","avbr"],"multi":false}},
    {"key":"gopSize","type":"float","required":false,"default":10,"description":"GOP size (seconds)",
      "control":{"kind":"range","min":0.5,"max":10,"step":0.5,"unit":"s"}},
    {"key":"size","type":"enum","required":false,"default":"1280x720","description":"Frame size",
      "control":{"kind":"select","options":["960x540","1280x720","1920x1080"],"multi":false}},
    {"key":"exposure","type":"int","required":false,"default":7,"description":"ISP exposure",
      "control":{"kind":"range","min":5,"max":32,"step":1}},
    {"key":"mirror","type":"bool","required":false,"default":false,"description":"Horizontal mirror",
      "control":{"kind":"toggle"}},
    {"key":"flip","type":"bool","required":false,"default":false,"description":"Vertical flip",
      "control":{"kind":"toggle"}},
    {"key":"contrast","type":"int","required":false,"default":50,"description":"Image contrast",
      "control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"hue","type":"int","required":false,"default":50,"description":"Image hue",
      "control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"saturation","type":"int","required":false,"default":50,"description":"Image saturation",
      "control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"luminance","type":"int","required":false,"default":50,"description":"Image luminance",
      "control":{"kind":"range","min":0,"max":100,"step":1}},
    {"key":"outgoing_server","type":"enum","required":false,"default":"udp://224.0.0.1:5600","description":"Primary output",
      "control":{"kind":"select","options":["udp://192.168.2.20:5600","udp://192.168.2.20:5700","udp://192.168.2.20:5701","udp://192.168.2.20:5702","udp://192.168.2.20:5703","udp://224.0.0.1:5600"],"multi":false}}
  ]
}
JSON
}

# ---------- /sys/video/get <name> ----------
video_get() {
  name="$1"
  [ -n "$name" ] || die "missing name"
  cli_key="$(map_cli_key "$name")" || die "unknown setting: $name"
  cli_get "$cli_key"
}

# ---------- /sys/video/set key=value (then apply) ----------
video_set_one() {
  pair="$1"
  key="${pair%%=*}"
  val="${pair#*=}"
  [ -n "$key" ] || die "missing key in pair"
  [ "$key" != "$val" ] || die "missing value in pair"

  case "$key" in
    mirror|flip)
      n="$(norm_bool "$val")"; [ "$n" != "__ERR__" ] || die "invalid bool: $key=$val"
      val="$n"
      ;;
    bitrate)
      n="$(norm_bitrate "$val")"; [ "$n" != "__ERR__" ] || die "invalid bitrate: $val"
      val="$n"
      ;;
    # others: raw passthrough; device CLI enforces domain
  esac

  cli_key="$(map_cli_key "$key")" || die "unknown setting: $key"
  cli_set "$cli_key" "$val" || die "set failed: $key=$val"
}

# ---------- /sys/video/params key=value ... (then apply) ----------
video_params() {
  ok=1
  for kv in "$@"; do
    # skip flags if any
    case "$kv" in --*) continue;; esac
    out="$(video_set_one "$kv" 2>&1)" || { echo "$out" 1>&2; ok=0; }
  done
  [ $ok -eq 1 ] || exit 2
  apply_video_settings || exit $?
  echo "ok"
}

# ---------- /sys/video/set wrapper ----------
video_set_cmd() {
  [ $# -ge 1 ] || die "usage: /sys/video/set key=value"
  video_set_one "$1" || exit $?
  apply_video_settings || exit $?
  echo "ok"
}

# ---------- /sys/video/apply ----------
video_apply_cmd() {
  apply_video_settings
}

# ==========================================================
case "$1" in
  /sys/video/help)    video_help_json ;;

  /sys/video/get)     shift; video_get "$1" ;;

  /sys/video/set)     shift; video_set_cmd "$@" ;;

  /sys/video/params)  shift; video_params "$@" ;;

  /sys/video/apply)   shift; video_apply_cmd "$@" ;;

  # generic utility you already had:
  /sys/ping)          shift; ping -c 1 -W 1 "$1" 2>&1 ;;

  *)                  echo "unknown path: $1" 1>&2; exit 2 ;;
esac
