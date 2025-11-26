#!/bin/sh
# used_rssi + voltage test generator (decreasing)
# - writes to:
#     /tmp/aalink_ext.msg  -> used_rssi=<0..80>
#     /tmp/crsf_log.msg    -> voltage=<0.0..10.0>
# - updates every 1s by default
# - used_rssi: -2 per step (wraps 0 -> 80)
# - voltage:   -0.1 per step (wraps 0.0 -> 10.0)
# - both start at their max values when the script starts

# Defaults (can be overridden via environment if you want)
USED_RSSI_MAX=${USED_RSSI_MAX:-80}
USED_RSSI_STEP=${USED_RSSI_STEP:-2}

VOLT_MAX_TENTHS=${VOLT_MAX_TENTHS:-100}   # 10.0 V -> 100 tenths
VOLT_STEP_TENTHS=${VOLT_STEP_TENTHS:-1}   # 0.1 V -> 1 tenth

SLEEP_SEC=${SLEEP_SEC:-1}

# Start at max values
used_rssi=$USED_RSSI_MAX
volt_tenths=$VOLT_MAX_TENTHS

while :; do
    # Write current values
    echo "used_rssi=$used_rssi" > /tmp/aalink_ext.msg

    # Print voltage as one decimal place
    printf 'voltage=%d.%01d\n' \
        $((volt_tenths / 10)) \
        $((volt_tenths % 10)) \
        > /tmp/crsf_log.msg

    # Decrement used_rssi and wrap
    used_rssi=$((used_rssi - USED_RSSI_STEP))
    if [ "$used_rssi" -lt 0 ]; then
        used_rssi=$USED_RSSI_MAX
    fi

    # Decrement voltage (in tenths) and wrap
    volt_tenths=$((volt_tenths - VOLT_STEP_TENTHS))
    if [ "$volt_tenths" -lt 0 ]; then
        volt_tenths=$VOLT_MAX_TENTHS
    fi

    sleep "$SLEEP_SEC"
done
