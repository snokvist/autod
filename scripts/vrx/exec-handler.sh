#!/bin/sh
# exec-handler.sh (pixelpilot + udp relay stubs, instant-apply, external help, reboot)
# Usage: exec-handler.sh <path> [args...]
# Contract: /exec with JSON {"path":"/sys/<cap>/<command>","args":[...]}
#
# Implemented:
#   /sys/help                                   (describe general system commands)
#   /sys/reboot                                 (schedule reboot)
#   /sys/shutdown                               (schedule shutdown)
#   /sys/pixelpilot/help|start|stop|toggle_record
#   /sys/pixelpilot_mini_rk/help|toggle_osd|toggle_recording|gamma|start|stop|restart
#   /sys/udp_relay/help|start|stop|status
#   /sys/joystick2crsf/help|get|set|reload|start|stop|restart|status
#   /sys/link/help                                   (advertise link help endpoints)
#   /sys/link/wfb/help|get|set|status|start|stop|restart|enable|disable
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
DVR_MEDIA_DIR="${DVR_MEDIA_DIR:-/media}"
JOYSTICK2CRSF_CONF="${JOYSTICK2CRSF_CONF:-/etc/joystick2crsf.conf}"
WFB_CONF="${WFB_CONF:-/etc/wfb.conf}"
SYNC_MAX_SLOTS=10
AUTOD_HTTP_PORT="${AUTOD_HTTP_PORT:-55667}"
AUTOD_HTTP_HOST="${AUTOD_HTTP_HOST:-127.0.0.1}"
AUTOD_HTTP_BASE="${AUTOD_HTTP_BASE:-http://${AUTOD_HTTP_HOST}:${AUTOD_HTTP_PORT}}"
PIXELPILOT_MINI_RK_GAMMA_BIN="${PIXELPILOT_MINI_RK_GAMMA_BIN:-/usr/bin/gamma}"
PIXELPILOT_MINI_RK_GAMMA_PRESETS="${PIXELPILOT_MINI_RK_GAMMA_PRESETS:-/etc/presets.ini}"
case "$AUTOD_HTTP_BASE" in
  */) AUTOD_HTTP_BASE="${AUTOD_HTTP_BASE%/}" ;;
esac

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

http_url_for_path(){
  path="$1"
  [ -n "$path" ] || path="/"
  case "$path" in
    http://*|https://*) printf '%s' "$path"; return 0 ;;
  esac
  case "$path" in
    /*) printf '%s%s' "$AUTOD_HTTP_BASE" "$path" ;;
    *) printf '%s/%s' "$AUTOD_HTTP_BASE" "$path" ;;
  esac
}

http_get_local(){
  path="$1"
  url="$(http_url_for_path "$path")" || return 2
  if have curl; then
    curl -fsS "$url"
    return $?
  fi
  if have wget; then
    wget -q -O - "$url"
    return $?
  fi
  echo "sync commands require curl or wget" 1>&2
  return 4
}

http_post_local(){
  path="$1"; body="$2"
  url="$(http_url_for_path "$path")" || return 2
  if have curl; then
    printf '%s' "$body" | curl -fsS -H 'Content-Type: application/json' --data-binary @- "$url"
    return $?
  fi
  if have wget; then
    tmp="$(mktemp /tmp/vrx-sync.XXXXXX 2>/dev/null)"
    if [ -z "$tmp" ]; then tmp="/tmp/vrx-sync.$$"; fi
    printf '%s' "$body" > "$tmp" || { rm -f "$tmp"; echo "failed to write sync payload" 1>&2; return 4; }
    wget -q -O - --header='Content-Type: application/json' --post-file="$tmp" "$url"
    rc=$?
    rm -f "$tmp"
    return $rc
  fi
  echo "sync commands require curl or wget" 1>&2
  return 4
}

json_escape(){
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

# ======================= General =======================
reboot_cmd(){
  ( nohup sh -c 'sleep 1; reboot now' >/dev/null 2>&1 & )
  echo "reboot scheduled"
  return 0
}

shutdown_cmd(){
  ( nohup sh -c 'shutdown now' >/dev/null 2>&1 & )
  echo "shutdown scheduled"
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

# ======================= Joystick2CRFS =======================
joystick2crsf_conf_path(){ echo "$JOYSTICK2CRSF_CONF"; }

joystick2crsf_conf_get(){
  key="$1"
  file="$(joystick2crsf_conf_path)"
  if [ ! -r "$file" ]; then
    echo "joystick2crsf config not found: $file" 1>&2
    return 4
  fi
  result="$(awk -v key="$key" '
    BEGIN{found=0;}
    {
      line=$0;
      trimmed=line;
      sub(/^[ \t]+/, "", trimmed);
      if (trimmed=="" || substr(trimmed,1,1)=="#") next;
      if (index(trimmed, key "=")==1){
        rest=substr(trimmed, length(key)+2);
        commentStart=index(rest, "#");
        if(commentStart>0){
          rest=substr(rest,1,commentStart-1);
        }
        gsub(/^[ \t]+/, "", rest);
        gsub(/[ \t]+$/, "", rest);
        printf("FOUND:%s\n", rest);
        found=1;
        exit;
      }
    }
    END{ if(!found) print "MISSING"; }
  ' "$file" 2>/dev/null)"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "failed to read joystick2crsf config" 1>&2
    return 4
  fi
  case "$result" in
    MISSING)
      echo "joystick2crsf config key not found: $key" 1>&2
      return 4
      ;;
    FOUND:*)
      value="${result#FOUND:}"
      printf '%s=%s\n' "$key" "$value"
      return 0
      ;;
    *)
      echo "failed to read joystick2crsf config" 1>&2
      return 4
      ;;
  esac
}

joystick2crsf_conf_set(){
  key="$1"; value="$2"
  file="$(joystick2crsf_conf_path)"
  ensure_parent_dir "$(dirname "$file")"
  tmp="$file.tmp.$$"
  if [ -f "$file" ]; then
    if ! awk -v key="$key" -v value="$value" '
      BEGIN{found=0;}
      {
        line=$0;
        trimmed=line;
        sub(/^[ \t]+/, "", trimmed);
        if (trimmed=="" || substr(trimmed,1,1)=="#"){
          print line;
          next;
        }
        if(!found && index(trimmed, key "=")==1){
          match(line, /^[ \t]*/);
          prefix=substr(line, 1, RLENGTH);
          rest=substr(trimmed, length(key)+2);
          comment="";
          commentStart=index(rest, "#");
          pre=rest;
          if(commentStart>0){
            comment=substr(rest, commentStart);
            pre=substr(rest, 1, commentStart-1);
          }
          spacing="";
          if(match(pre, /[ \t]+$/)){
            spacing=substr(pre, RSTART, RLENGTH);
          }
          printf("%s%s=%s%s%s\n", prefix, key, value, spacing, comment);
          found=1;
        } else {
          print line;
        }
      }
      END{
        if(!found){
          printf("%s=%s\n", key, value);
        }
      }
    ' "$file" >"$tmp"; then
      rm -f "$tmp"
      echo "failed to update joystick2crsf config" 1>&2
      return 4
    fi
  else
    if ! printf '%s=%s\n' "$key" "$value" >"$tmp"; then
      rm -f "$tmp"
      echo "failed to write joystick2crsf config" 1>&2
      return 4
    fi
  fi
  if ! mv "$tmp" "$file"; then
    rm -f "$tmp"
    echo "failed to write joystick2crsf config" 1>&2
    return 4
  fi
  chmod 644 "$file" 2>/dev/null || true
  return 0
}

joystick2crsf_pids(){ pidof joystick2crsf 2>/dev/null; }

joystick2crsf_unit_candidates(){
  echo "joystick2crsf.service"
  echo "joystick2crsf"
}

joystick2crsf_start(){
  if have systemctl; then
    for unit in $(joystick2crsf_unit_candidates); do
      if systemctl start "$unit" >/dev/null 2>&1; then
        echo "joystick2crsf started"
        return 0
      fi
    done
  fi
  if have service; then
    if service joystick2crsf start >/dev/null 2>&1; then
      echo "joystick2crsf started"
      return 0
    fi
  fi
  echo "joystick2crsf start unsupported on this device" 1>&2
  return 3
}

joystick2crsf_stop(){
  if have systemctl; then
    for unit in $(joystick2crsf_unit_candidates); do
      if systemctl stop "$unit" >/dev/null 2>&1; then
        echo "joystick2crsf stopped"
        return 0
      fi
    done
  fi
  if have service; then
    if service joystick2crsf stop >/dev/null 2>&1; then
      echo "joystick2crsf stopped"
      return 0
    fi
  fi
  echo "joystick2crsf stop unsupported on this device" 1>&2
  return 3
}

joystick2crsf_restart(){
  if have systemctl; then
    for unit in $(joystick2crsf_unit_candidates); do
      if systemctl restart "$unit" >/dev/null 2>&1; then
        echo "joystick2crsf restarted"
        return 0
      fi
    done
  fi
  if joystick2crsf_stop; then
    sleep 1
    if joystick2crsf_start; then
      echo "joystick2crsf restarted"
      return 0
    fi
  fi
  echo "joystick2crsf restart unsupported on this device" 1>&2
  return 3
}

joystick2crsf_status(){
  if have systemctl; then
    for unit in $(joystick2crsf_unit_candidates); do
      if systemctl is-active --quiet "$unit" >/dev/null 2>&1; then
        echo "joystick2crsf running (systemd)"
        return 0
      fi
    done
  fi
  pids="$(joystick2crsf_pids)"
  if [ -n "$pids" ]; then
    echo "joystick2crsf running (pid $pids)"
  else
    echo "joystick2crsf not running"
  fi
  return 0
}

joystick2crsf_reload(){
  if have systemctl; then
    for unit in $(joystick2crsf_unit_candidates); do
      if systemctl reload "$unit" >/dev/null 2>&1; then
        echo "joystick2crsf reloaded via systemctl"
        return 0
      fi
    done
  fi
  pids="$(joystick2crsf_pids)"
  if [ -z "$pids" ]; then
    echo "joystick2crsf not running" 1>&2
    return 3
  fi
  if kill -HUP $pids 2>/dev/null; then
    echo "joystick2crsf reloaded via SIGHUP ($pids)"
    return 0
  fi
  echo "failed to signal joystick2crsf" 1>&2
  return 4
}

joystick2crsf_get(){
  if [ $# -ne 1 ]; then
    echo "usage: joystick2crsf/get <key>" 1>&2
    return 2
  fi
  key="$1"
  if [ -z "$key" ]; then
    echo "usage: joystick2crsf/get <key>" 1>&2
    return 2
  fi
  joystick2crsf_conf_get "$key"
}

joystick2crsf_set(){
  if [ $# -eq 1 ]; then
    case "$1" in
      *=*)
        key="${1%%=*}"
        value="${1#*=}"
        ;;
      *)
        echo "usage: joystick2crsf/set key=value" 1>&2
        return 2
        ;;
    esac
  elif [ $# -eq 2 ]; then
    key="$1"
    value="$2"
  else
    echo "usage: joystick2crsf/set key=value" 1>&2
    return 2
  fi
  if [ -z "$key" ]; then
    echo "missing key" 1>&2
    return 2
  fi
  joystick2crsf_conf_set "$key" "$value"
  rc=$?
  if [ $rc -ne 0 ]; then
    return $rc
  fi
  echo "ok"
  return 0
}

# ======================= Link WFB =======================
wfb_conf_path(){ echo "$WFB_CONF"; }

wfb_conf_get(){
  key="$1"
  [ -n "$key" ] || { echo "usage: link/wfb/get key" 1>&2; return 2; }
  file="$(wfb_conf_path)"
  if [ ! -r "$file" ]; then
    echo "wfb config not found: $file" 1>&2
    return 4
  fi
  result="$(awk -v key="$key" '
    function trim(str){ sub(/^[ \t]+/, "", str); sub(/[ \t]+$/, "", str); return str; }
    BEGIN{ in_params=0; found=0; }
    {
      line=$0
      if(match(line, /^[ \t]*\[/)){
        section=trim(line)
        if(tolower(section)=="[parameters]"){
          in_params=1
        } else if(in_params){
          exit
        } else {
          in_params=0
        }
        next
      }
      if(!in_params) next
      trimmed=trim(line)
      if(trimmed=="" || substr(trimmed,1,1)=="#") next
      if(index(trimmed, key "=")==1){
        value=substr(trimmed, length(key)+2)
        value=trim(value)
        printf("FOUND:%s\n", value)
        found=1
        exit
      }
    }
    END{ if(!found) print "MISSING"; }
  ' "$file" 2>/dev/null)"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "failed to read wfb config" 1>&2
    return 4
  fi
  case "$result" in
    FOUND:*)
      value="${result#FOUND:}"
      printf '%s=%s\n' "$key" "$value"
      return 0
      ;;
    MISSING)
      echo "wfb config key not found in [parameters]: $key" 1>&2
      return 4
      ;;
    *)
      echo "failed to read wfb config" 1>&2
      return 4
      ;;
  esac
}

wfb_conf_set(){
  if [ $# -eq 1 ]; then
    case "$1" in
      *=*)
        key="${1%%=*}"
        value="${1#*=}"
        ;;
      *)
        echo "usage: link/wfb/set key=value" 1>&2
        return 2
        ;;
    esac
  elif [ $# -eq 2 ]; then
    key="$1"
    value="$2"
  else
    echo "usage: link/wfb/set key=value" 1>&2
    return 2
  fi
  if [ -z "$key" ]; then
    echo "missing key" 1>&2
    return 2
  fi
  file="$(wfb_conf_path)"
  ensure_parent_dir "$(dirname "$file")"
  tmp="$file.tmp.$$"
  if [ -f "$file" ]; then
    if ! awk -v key="$key" -v value="$value" '
      function trim(str){ sub(/^[ \t]+/, "", str); sub(/[ \t]+$/, "", str); return str; }
      BEGIN{ in_params=0; found=0; seen_params=0; }
      {
        line=$0
        if(match(line, /^[ \t]*\[/)){
          if(in_params && !found){
            printf("%s=%s\n", key, value)
            found=1
          }
          section=trim(line)
          if(tolower(section)=="[parameters]"){
            in_params=1
            seen_params=1
          } else {
            in_params=0
          }
          print line
          next
        }
        if(in_params){
          trimmed=trim(line)
          if(trimmed=="" || substr(trimmed,1,1)=="#"){ print line; next; }
          if(index(trimmed, key "=")==1){
            match(line, /^[ \t]*/); prefix=substr(line, 1, RLENGTH)
            rest=trim(substr(trimmed, length(key)+2))
            comment=""
            pre=rest
            commentStart=index(rest, "#")
            if(commentStart>0){
              comment=substr(rest, commentStart)
              pre=substr(rest, 1, commentStart-1)
            }
            spacing=""
            if(match(pre, /[ \t]+$/)){
              spacing=substr(pre, RSTART, RLENGTH)
            }
            printf("%s%s=%s%s%s\n", prefix, key, value, spacing, comment)
            found=1
            next
          }
        }
        print line
      }
      END{
        if(!found){
          if(!seen_params){
            print ""
            print "[parameters]"
          }
          printf("%s=%s\n", key, value)
        }
      }
    ' "$file" > "$tmp"; then
      rm -f "$tmp"
      echo "failed to update wfb config" 1>&2
      return 4
    fi
  else
    if ! {
      echo "[parameters]" && printf '%s=%s\n' "$key" "$value"
    } > "$tmp"; then
      rm -f "$tmp"
      echo "failed to write wfb config" 1>&2
      return 4
    fi
  fi
  if ! mv "$tmp" "$file"; then
    rm -f "$tmp"
    echo "failed to write wfb config" 1>&2
    return 4
  fi
  chmod 644 "$file" 2>/dev/null || true
  echo "ok"
  return 0
}

wfb_unit(){ echo "wfb_supervisor.service"; }

wfb_systemctl(){
  action="$1"
  if ! have systemctl; then
    echo "systemctl unavailable" 1>&2
    return 3
  fi
  unit="$(wfb_unit)"
  systemctl "$action" "$unit" >/dev/null 2>&1
}

wfb_start(){
  wfb_systemctl start
  rc=$?
  case $rc in
    0) echo "wfb supervisor started"; return 0 ;;
    3) return 3 ;;
    *) echo "failed to start wfb supervisor" 1>&2; return 4 ;;
  esac
}

wfb_stop(){
  wfb_systemctl stop
  rc=$?
  case $rc in
    0) echo "wfb supervisor stopped"; return 0 ;;
    3) return 3 ;;
    *) echo "failed to stop wfb supervisor" 1>&2; return 4 ;;
  esac
}

wfb_restart(){
  wfb_systemctl restart
  rc=$?
  case $rc in
    0) echo "wfb supervisor restarted"; return 0 ;;
    3) return 3 ;;
    *) echo "failed to restart wfb supervisor" 1>&2; return 4 ;;
  esac
}

wfb_enable(){
  wfb_systemctl enable
  rc=$?
  case $rc in
    0) echo "wfb supervisor enabled"; return 0 ;;
    3) return 3 ;;
    *) echo "failed to enable wfb supervisor" 1>&2; return 4 ;;
  esac
}

wfb_disable(){
  wfb_systemctl disable
  rc=$?
  case $rc in
    0) echo "wfb supervisor disabled"; return 0 ;;
    3) return 3 ;;
    *) echo "failed to disable wfb supervisor" 1>&2; return 4 ;;
  esac
}

wfb_status(){
  if ! have systemctl; then
    echo "systemctl unavailable" 1>&2
    return 3
  fi
  systemctl is-active "$(wfb_unit)"
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

pixelpilot_mini_rk_gamma(){
  if [ ! -x "$PIXELPILOT_MINI_RK_GAMMA_BIN" ]; then
    echo "gamma utility unavailable at $PIXELPILOT_MINI_RK_GAMMA_BIN" 1>&2
    return 4
  fi
  "$PIXELPILOT_MINI_RK_GAMMA_BIN" --presets "$PIXELPILOT_MINI_RK_GAMMA_PRESETS" "$@"
}

# legacy aliases that keep older endpoints working; prefer /sys/reboot and /sys/shutdown
pixelpilot_mini_rk_reboot(){
  reboot_cmd "$@"
}

pixelpilot_mini_rk_shutdown(){
  shutdown_cmd "$@"
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

# ======================= Sync slots =======================
sync_status_cmd(){
  if [ $# -ne 0 ]; then
    echo "usage: sync/status" 1>&2
    return 2
  fi
  http_get_local "/sync/slaves"
}

sync_move_append(){
  sid="$1"; slot_value="$2"
  if [ -z "$sid" ]; then
    echo "missing slave_id" 1>&2
    return 2
  fi
  escaped="$(json_escape "$sid")"
  entry="{\"slave_id\":\"$escaped\",\"slot\":"
  if [ "$slot_value" = "null" ]; then
    entry="${entry}null}"
  else
    entry="${entry}${slot_value}}"
  fi
  if [ -n "$sync_moves_buf" ]; then
    sync_moves_buf="$sync_moves_buf,$entry"
  else
    sync_moves_buf="$entry"
  fi
  return 0
}

sync_move_cmd(){
  if [ $# -lt 2 ]; then
    echo "usage: sync/move slave_id=<id> slot=<n|null> [slave_id=<id> slot=<n|null>]..." 1>&2
    return 2
  fi
  sync_moves_buf=""
  move_count=0
  current_id=""
  current_slot=""
  current_has_slot=0
  while [ $# -gt 0 ]; do
    arg="$1"
    case "$arg" in
      slave_id=*|id=*)
        if [ -n "$current_id" ]; then
          if [ $current_has_slot -eq 0 ]; then
            echo "slot missing for slave_id $current_id" 1>&2
            return 2
          fi
          sync_move_append "$current_id" "$current_slot" || return $?
          move_count=$((move_count+1))
          current_slot=""
          current_has_slot=0
        fi
        current_id="${arg#*=}"
        ;;
      slot=*)
        if [ -z "$current_id" ]; then
          echo "slot specified before slave_id" 1>&2
          return 2
        fi
        slot_raw="${arg#*=}"
        if [ -z "$slot_raw" ] || [ "$slot_raw" = "null" ] || [ "$slot_raw" = "none" ] || [ "$slot_raw" = "clear" ]; then
          current_slot="null"
          current_has_slot=1
        else
          if ! printf '%s' "$slot_raw" | grep -Eq '^[0-9]+$'; then
            echo "invalid slot: $slot_raw" 1>&2
            return 2
          fi
          slot_int="$slot_raw"
          if [ "$slot_int" -le 0 ] || [ "$slot_int" -gt "$SYNC_MAX_SLOTS" ]; then
            echo "slot must be between 1 and $SYNC_MAX_SLOTS" 1>&2
            return 2
          fi
          current_slot="$slot_int"
          current_has_slot=1
        fi
        ;;
      *)
        echo "unknown arg: $arg" 1>&2
        return 2
        ;;
    esac
    shift
  done
  if [ -n "$current_id" ]; then
    if [ $current_has_slot -eq 0 ]; then
      echo "slot missing for slave_id $current_id" 1>&2
      return 2
    fi
    sync_move_append "$current_id" "$current_slot" || return $?
    move_count=$((move_count+1))
  fi
  if [ "$move_count" -le 0 ]; then
    echo "no moves specified" 1>&2
    return 2
  fi
  body="{\"moves\":[${sync_moves_buf}]}"
  http_post_local "/sync/push" "$body"
}

sync_replay_cmd(){
  if [ $# -eq 0 ]; then
    echo "usage: sync/replay slot=<n> [slot=<n> ...] [slave_id=<id> ...]" 1>&2
    return 2
  fi
  slot_buf=""
  slot_count=0
  id_buf=""
  id_count=0
  while [ $# -gt 0 ]; do
    arg="$1"
    case "$arg" in
      slot=*)
        slot_raw="${arg#*=}"
        if ! printf '%s' "$slot_raw" | grep -Eq '^[0-9]+$'; then
          echo "invalid slot: $slot_raw" 1>&2
          return 2
        fi
        slot_int="$slot_raw"
        if [ "$slot_int" -le 0 ] || [ "$slot_int" -gt "$SYNC_MAX_SLOTS" ]; then
          echo "slot must be between 1 and $SYNC_MAX_SLOTS" 1>&2
          return 2
        fi
        if [ -n "$slot_buf" ]; then
          slot_buf="$slot_buf,$slot_int"
        else
          slot_buf="$slot_int"
        fi
        slot_count=$((slot_count+1))
        ;;
      slave_id=*|id=*)
        sid="${arg#*=}"
        if [ -z "$sid" ]; then
          echo "missing slave_id value" 1>&2
          return 2
        fi
        escaped="$(json_escape "$sid")"
        if [ -n "$id_buf" ]; then
          id_buf="$id_buf,\"$escaped\""
        else
          id_buf="\"$escaped\""
        fi
        id_count=$((id_count+1))
        ;;
      *)
        echo "unknown arg: $arg" 1>&2
        return 2
        ;;
    esac
    shift
  done
  if [ $slot_count -le 0 ] && [ $id_count -le 0 ]; then
    echo "no replay targets supplied" 1>&2
    return 2
  fi
  body="{"
  sep=""
  if [ $slot_count -gt 0 ]; then
    body="${body}\"replay_slots\":[${slot_buf}]"
    sep="," 
  fi
  if [ $id_count -gt 0 ]; then
    body="${body}${sep}\"replay_ids\":[${id_buf}]"
  fi
  body="${body}}"
  http_post_local "/sync/push" "$body"
}

sync_delete_cmd(){
  if [ $# -eq 0 ]; then
    echo "usage: sync/delete slave_id=<id> [slave_id=<id> ...]" 1>&2
    return 2
  fi
  id_buf=""
  id_count=0
  while [ $# -gt 0 ]; do
    arg="$1"
    case "$arg" in
      slave_id=*|id=*)
        sid="${arg#*=}"
        if [ -z "$sid" ]; then
          echo "missing slave_id value" 1>&2
          return 2
        fi
        escaped="$(json_escape "$sid")"
        if [ -n "$id_buf" ]; then
          id_buf="$id_buf,\"$escaped\""
        else
          id_buf="\"$escaped\""
        fi
        id_count=$((id_count+1))
        ;;
      *)
        echo "unknown arg: $arg" 1>&2
        return 2
        ;;
    esac
    shift
  done
  if [ $id_count -le 0 ]; then
    echo "no slave_id entries provided" 1>&2
    return 2
  fi
  body="{\"delete_ids\":[${id_buf}]}"
  http_post_local "/sync/push" "$body"
}

# ======================= DISPATCH =======================
case "$1" in
  # general
  /sys/help)               print_help_msg "sys_help.msg" ;;
  /sys/reboot)             shift; reboot_cmd "$@" ;;
  /sys/shutdown)           shift; shutdown_cmd "$@" ;;

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

  # joystick2crsf
  /sys/joystick2crsf/help)     print_help_msg "joystick2crsf_help.msg" ;;
  /sys/joystick2crsf/get)      shift; joystick2crsf_get "$@" ;;
  /sys/joystick2crsf/set)      shift; joystick2crsf_set "$@" ;;
  /sys/joystick2crsf/reload)   shift; joystick2crsf_reload "$@" ;;
  /sys/joystick2crsf/start)    shift; joystick2crsf_start "$@" ;;
  /sys/joystick2crsf/stop)     shift; joystick2crsf_stop "$@" ;;
  /sys/joystick2crsf/restart)  shift; joystick2crsf_restart "$@" ;;
  /sys/joystick2crsf/status)   shift; joystick2crsf_status "$@" ;;

  # link
  /sys/link/help)          print_help_msg "link_root_help.msg" ;;

  # link wfb
  /sys/link/wfb/help)      print_help_msg "link_help.msg" ;;
  /sys/link/wfb/get)       shift; wfb_conf_get "$@" ;;
  /sys/link/wfb/set)       shift; wfb_conf_set "$@" ;;
  /sys/link/wfb/start)     shift; wfb_start "$@" ;;
  /sys/link/wfb/stop)      shift; wfb_stop "$@" ;;
  /sys/link/wfb/restart)   shift; wfb_restart "$@" ;;
  /sys/link/wfb/enable)    shift; wfb_enable "$@" ;;
  /sys/link/wfb/disable)   shift; wfb_disable "$@" ;;
  /sys/link/wfb/status)    shift; wfb_status "$@" ;;

  # dvr recordings
  /sys/dvr/list)           shift; dvr_list "$@" ;;
  /sys/dvr/delete_all)     shift; dvr_delete_all "$@" ;;

  # pixelpilot mini rk
  /sys/pixelpilot_mini_rk/help)             print_help_msg "pixelpilot_mini_rk_help.msg" ;;
  /sys/pixelpilot_mini_rk/toggle_osd)       shift; pixelpilot_mini_rk_toggle_osd "$@" ;;
  /sys/pixelpilot_mini_rk/toggle_recording) shift; pixelpilot_mini_rk_toggle_recording "$@" ;;
  /sys/pixelpilot_mini_rk/gamma)            shift; pixelpilot_mini_rk_gamma "$@" ;;
  /sys/pixelpilot_mini_rk/reboot)           shift; pixelpilot_mini_rk_reboot "$@" ;;
  /sys/pixelpilot_mini_rk/shutdown)         shift; pixelpilot_mini_rk_shutdown "$@" ;;
  /sys/pixelpilot_mini_rk/start)            shift; pixelpilot_mini_rk_start "$@" ;;
  /sys/pixelpilot_mini_rk/stop)             shift; pixelpilot_mini_rk_stop "$@" ;;
  /sys/pixelpilot_mini_rk/restart)          shift; pixelpilot_mini_rk_restart "$@" ;;

  # sync slots
  /sys/sync/help)           print_help_msg "sync_help.msg" ;;
  /sys/sync/status)         shift; sync_status_cmd "$@" ;;
  /sys/sync/move)           shift; sync_move_cmd "$@" ;;
  /sys/sync/replay)         shift; sync_replay_cmd "$@" ;;
  /sys/sync/delete)         shift; sync_delete_cmd "$@" ;;

  # utility
  /sys/ping)               shift; ping -c 1 -W 1 "$1" 2>&1 ;;

  *) echo "unknown path: $1" 1>&2; exit 2 ;;
esac
