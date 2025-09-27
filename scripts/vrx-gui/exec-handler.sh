#!/bin/sh
# exec-handler.sh (vrx-gui stubs for gui-driven vrx management)
# Usage: exec-handler.sh <path> [args...]
# Contract: /exec with JSON {"path":"/sys/<cap>/<command>","args":[...]}
#
# Implemented (stubs):
#   /sys/gui/help                 (static help payload)
#   /sys/gui/sync                 (placeholder for capability sync)
#   /sys/gui/status               (very light weight state reporting)
#   /sys/ping                     (utility passthrough)
#
# This handler is intentionally light-weight; it mirrors the structure of the
# legacy vrx handler so that we can iterate quickly while keeping GUI-specific
# behaviour isolated. The sync endpoint will eventually interrogate the local
# autod instance for its capability set and available ports so the GUI can make
# smart decisions about VTX pairing.

PATH=/usr/sbin:/usr/bin:/sbin:/bin
HELP_DIR="${HELP_DIR:-/etc/autod}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STATE_DIR="${VRX_GUI_STATE_DIR:-/tmp/vrx-gui}"
GUI_STATE_FILE="$STATE_DIR/gui.env"
HELP_FILE_NAME="vrx_gui_help.msg"

# ======================= Helpers =======================
die(){ echo "$*" 1>&2; exit 2; }
have(){ command -v "$1" >/dev/null 2>&1; }
ensure_parent_dir(){ dir="$1"; [ -d "$dir" ] || mkdir -p "$dir"; }

print_help_msg(){
  for base in "$HELP_DIR" "$SCRIPT_DIR"; do
    file="$base/$HELP_FILE_NAME"
    if [ -r "$file" ]; then
      cat "$file"
      return 0
    fi
  done
  echo '{"error":"help file not found","name":"vrx_gui_help"}'
  return 4
}

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
  kv_file_get "$key" "$file"
}

config_set(){
  key="$1"; value="$2"; file="$3"
  kv_file_set "$key" "$value" "$file"
  echo "ok"
  return 0
}

# ======================= GUI Stubs =======================

gui_state_key(){ echo "gui_$1"; }

gui_get_value(){ config_get "$(gui_state_key "$1")" "$GUI_STATE_FILE"; }

gui_set_value(){ config_set "$(gui_state_key "$1")" "$2" "$GUI_STATE_FILE"; }

sync_caps_stub(){
  # TODO: replace this stub with a real capability discovery routine that queries
  # the local autod instance and returns GUI consumable metadata.
  echo '{"status":"pending","caps":[],"message":"sync routine not yet implemented"}'
  return 0
}

report_status(){
  last_sync="$(gui_get_value last_sync 2>/dev/null)"
  [ -n "$last_sync" ] || last_sync="never"
  cat <<JSON
{"status":"stub","last_sync":"$last_sync"}
JSON
  return 0
}

record_sync_timestamp(){
  gui_set_value last_sync "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >/dev/null 2>&1
}

run_ping(){
  host="$1"
  [ -n "$host" ] || die "ping requires a host"
  if have ping; then
    ping -c 1 "$host"
    return $?
  fi
  echo '{"error":"ping unavailable"}'
  return 4
}

# ======================= Dispatch =======================

[ $# -ge 1 ] || die "usage: $0 <path> [args...]"
path="$1"; shift

case "$path" in
  /sys/gui/help)
    print_help_msg
    ;;
  /sys/gui/sync)
    record_sync_timestamp
    sync_caps_stub
    ;;
  /sys/gui/status)
    report_status
    ;;
  /sys/ping)
    run_ping "$@"
    ;;
  *)
    echo '{"error":"unsupported command","path":"'"$path"'"}'
    exit 3
    ;;
esac
