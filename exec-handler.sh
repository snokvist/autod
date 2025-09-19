#!/bin/sh
# Usage: exec-handler.sh <path> [args...]
PATH=/usr/sbin:/usr/bin:/sbin:/bin

case "$1" in
  /sys/video/exec)
    shift
    echo "video exec ok: $@"
    ;;
  /control)
    shift
    key="$1"; value="$2"
    case "$key" in
      bitrate)  /usr/bin/video-ctl set-bitrate "$value" ;;
      gop)      /usr/bin/video-ctl set-gop "$value" ;;
      *) echo "unknown control key: $key" 1>&2; exit 2 ;;
    esac
    echo "ok: $key=$value"
    ;;
  /sys/ping)
    shift
    ping -c 1 -W 1 "$1" 2>&1
    ;;
  *)
    echo "unknown path: $1" 1>&2
    exit 2
    ;;
esac
