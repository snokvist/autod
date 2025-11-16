# Protocol self-test notes

Run `tests/protocol_selftest.py` to print reference CRSF and MAVLink frames built
from a deterministic channel set. The script mirrors the helper algorithms used
by `joystick2crsf` (CRSF packing, CRSF→µs scaling, and MAVLink v2 X25 checksums)
and emits both hex dumps:

```bash
python3 tests/protocol_selftest.py
```

## Verifying CRSF output

1. Start `joystick2crsf` with `protocol=crsf` while time-slicing channel values
   (for example with the on-screen diagnostics or by feeding known joystick
   positions).
2. Capture the UDP datagrams (`udp_target`) with `tcpdump -X udp port 14550`
   or use the existing `tests/crsf_dump.c` helper against a UDP capture.
3. Compare the hex payloads against the script output. Each frame should match
   the `CRSF frame (hex)` dump byte-for-byte.

## Verifying MAVLink output

1. Set `protocol=mavlink` and adjust the `mavlink_*` IDs to match your flight
   stack.
2. Use `tcpdump` or `socat - UDP-RECV:<port>,fork` to capture the UDP stream.
3. Confirm that each datagram begins with `FD 12 00 00` (MAVLink v2 header with zero flags) and that
   the payload matches the `MAVLink RC_CHANNELS_OVERRIDE` frame dump printed by
   the script. The first eight channels should scale to the 1000–2000 µs range.

In both modes the Server-Sent Events telemetry (`sse_enabled=true`) continues to
stream CRSF-scaled values at 10 Hz for debugging dashboards.
