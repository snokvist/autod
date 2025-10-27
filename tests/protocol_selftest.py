#!/usr/bin/env python3
"""Generate reference CRSF and MAVLink frames for joystick2crfs."""

CRSF_DEST = 0xC8
CRSF_TYPE_CHANNELS = 0x16
CRSF_MIN = 172
CRSF_MAX = 1811
CRSF_RANGE = CRSF_MAX - CRSF_MIN

MAVLINK_STX = 0xFE
MAVLINK_MSG_RC_OVERRIDE = 70
MAVLINK_PAYLOAD_LEN = 18
MAVLINK_RC_CRC_EXTRA = 124
MAVLINK_MIN_US = 1000
MAVLINK_MAX_US = 2000
MAVLINK_RANGE_US = MAVLINK_MAX_US - MAVLINK_MIN_US


def crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0xD5) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def crc_x25(data: bytes, seed: int = 0xFFFF) -> int:
    crc = seed
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return crc & 0xFFFF


def pack_crsf(channels):
    frame = bytearray(26)
    frame[0] = CRSF_DEST
    frame[1] = 24
    frame[2] = CRSF_TYPE_CHANNELS

    bit = 0
    for value in channels:
        value &= 0x7FF
        byte_index = bit // 8
        offset = bit % 8
        frame[3 + byte_index] |= (value << offset) & 0xFF
        if offset > 5 and byte_index + 1 < 22:
            frame[3 + byte_index + 1] |= (value >> (8 - offset)) & 0xFF
        if offset > 2 and byte_index + 2 < 22:
            frame[3 + byte_index + 2] |= (value >> (16 - offset)) & 0xFF
        bit += 11

    crc_region = bytes(frame[2:25])
    frame[25] = crc8(crc_region)
    return bytes(frame)


def crsf_to_mavlink(value: int) -> int:
    if value <= CRSF_MIN:
        return MAVLINK_MIN_US
    if value >= CRSF_MAX:
        return MAVLINK_MAX_US
    scaled = (value - CRSF_MIN) * MAVLINK_RANGE_US
    scaled = (scaled + CRSF_RANGE // 2) // CRSF_RANGE
    output = MAVLINK_MIN_US + scaled
    return max(MAVLINK_MIN_US, min(MAVLINK_MAX_US, output))


def pack_mavlink(channels, seq=0, sysid=255, compid=190, target_sysid=1, target_compid=1):
    frame = bytearray(26)
    frame[0] = MAVLINK_STX
    frame[1] = MAVLINK_PAYLOAD_LEN
    frame[2] = seq & 0xFF
    frame[3] = sysid & 0xFF
    frame[4] = compid & 0xFF
    frame[5] = MAVLINK_MSG_RC_OVERRIDE
    frame[6] = target_sysid & 0xFF
    frame[7] = target_compid & 0xFF

    offset = 8
    for value in channels[:8]:
        mv = crsf_to_mavlink(value)
        frame[offset] = mv & 0xFF
        frame[offset + 1] = (mv >> 8) & 0xFF
        offset += 2

    crc_payload = bytes(frame[6:6 + MAVLINK_PAYLOAD_LEN])
    crc = crc_x25(crc_payload)
    crc = crc_x25(bytes([MAVLINK_MSG_RC_OVERRIDE]), crc)
    crc = crc_x25(bytes([MAVLINK_RC_CRC_EXTRA]), crc)
    frame[24] = crc & 0xFF
    frame[25] = (crc >> 8) & 0xFF
    return bytes(frame)


def format_hex(blob: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in blob)


def main():
    sample = [
        CRSF_MIN,
        CRSF_MIN + CRSF_RANGE // 4,
        CRSF_MIN + CRSF_RANGE // 2,
        CRSF_MIN + (CRSF_RANGE * 3) // 4,
        CRSF_MAX,
        CRSF_MAX - CRSF_RANGE // 3,
        CRSF_MIN + CRSF_RANGE // 3,
        CRSF_MIN + (CRSF_RANGE * 2) // 3,
        CRSF_MIN + CRSF_RANGE // 5,
        CRSF_MAX - CRSF_RANGE // 5,
        CRSF_MIN + CRSF_RANGE // 6,
        CRSF_MAX - CRSF_RANGE // 6,
        CRSF_MIN + CRSF_RANGE // 7,
        CRSF_MAX - CRSF_RANGE // 7,
        CRSF_MIN + CRSF_RANGE // 8,
        CRSF_MAX - CRSF_RANGE // 8,
    ]

    crsf_frame = pack_crsf(sample)
    mavlink_frame = pack_mavlink(sample)

    print("CRSF frame (hex):")
    print(format_hex(crsf_frame))
    print()
    print("MAVLink RC_CHANNELS_OVERRIDE frame (hex):")
    print(format_hex(mavlink_frame))


if __name__ == "__main__":
    main()
