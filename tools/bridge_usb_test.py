#!/usr/bin/env python3
"""Exercise the ground AirLink USB MAVLink side without flight-control commands."""

import argparse
import json
import struct
import time

import serial


def x25_crc(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        tmp = value ^ (crc & 0xFF)
        tmp ^= (tmp << 4) & 0xFF
        crc = ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF
    return crc


def gcs_heartbeat(sequence: int = 0) -> bytes:
    # MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, disarmed, MAV_STATE_ACTIVE.
    payload = struct.pack("<IBBBBB", 0, 6, 8, 0, 4, 3)
    header = bytes((len(payload), sequence & 0xFF, 255, 190, 0))
    checksum = x25_crc(header + payload + bytes((50,)))
    return bytes((0xFE,)) + header + payload + struct.pack("<H", checksum)


def param_request_list(sequence: int = 1) -> bytes:
    """Build a MAVLink 1 PARAM_REQUEST_LIST for autopilot sysid 1/compid 1."""
    payload = struct.pack("<BB", 1, 1)
    header = bytes((len(payload), sequence & 0xFF, 255, 190, 21))
    checksum = x25_crc(header + payload + bytes((159,)))
    return bytes((0xFE,)) + header + payload + struct.pack("<H", checksum)


def parse_frames(data: bytes) -> dict:
    frames = 0
    heartbeats = 0
    valid_heartbeats = 0
    parameter_values = 0
    valid_parameter_values = 0
    parameter_count = 0
    parameter_indices: set[int] = set()
    serial_parameters: dict[str, float] = {}
    message_ids: dict[int, int] = {}
    index = 0
    while index < len(data):
        magic = data[index]
        if magic == 0xFE and index + 8 <= len(data):
            payload_length = data[index + 1]
            frame_length = payload_length + 8
            if index + frame_length > len(data):
                break
            frame = data[index:index + frame_length]
            message_id = frame[5]
            frames += 1
            message_ids[message_id] = message_ids.get(message_id, 0) + 1
            if message_id == 0:
                heartbeats += 1
                expected = x25_crc(frame[1:-2] + bytes((50,)))
                actual = struct.unpack("<H", frame[-2:])[0]
                valid_heartbeats += expected == actual
            elif message_id == 22 and payload_length >= 25:
                parameter_values += 1
                expected = x25_crc(frame[1:-2] + bytes((220,)))
                actual = struct.unpack("<H", frame[-2:])[0]
                if expected == actual:
                    valid_parameter_values += 1
                    count, parameter_index = struct.unpack("<HH", frame[10:14])
                    parameter_count = max(parameter_count, count)
                    parameter_indices.add(parameter_index)
                    name = frame[14:30].split(b"\0", 1)[0].decode("ascii", errors="replace")
                    if name.startswith("SERIAL"):
                        serial_parameters[name] = struct.unpack("<f", frame[6:10])[0]
            index += frame_length
            continue
        if magic == 0xFD and index + 12 <= len(data):
            payload_length = data[index + 1]
            signature_length = 13 if data[index + 2] & 0x01 else 0
            frame_length = payload_length + 12 + signature_length
            if index + frame_length > len(data):
                break
            frame = data[index:index + frame_length]
            message_id = frame[7] | (frame[8] << 8) | (frame[9] << 16)
            frames += 1
            message_ids[message_id] = message_ids.get(message_id, 0) + 1
            if message_id == 0:
                heartbeats += 1
                expected = x25_crc(frame[1:10 + payload_length] + bytes((50,)))
                actual = struct.unpack("<H", frame[10 + payload_length:12 + payload_length])[0]
                valid_heartbeats += expected == actual
            elif message_id == 22 and payload_length >= 25:
                parameter_values += 1
                expected = x25_crc(frame[1:10 + payload_length] + bytes((220,)))
                actual = struct.unpack("<H", frame[10 + payload_length:12 + payload_length])[0]
                if expected == actual:
                    valid_parameter_values += 1
                    count, parameter_index = struct.unpack("<HH", frame[14:18])
                    parameter_count = max(parameter_count, count)
                    parameter_indices.add(parameter_index)
                    name = frame[18:34].split(b"\0", 1)[0].decode("ascii", errors="replace")
                    if name.startswith("SERIAL"):
                        serial_parameters[name] = struct.unpack("<f", frame[10:14])[0]
            index += frame_length
            continue
        index += 1
    indexed_parameters = sum(index < parameter_count for index in parameter_indices)
    return {
        "bytes_received": len(data),
        "frames": frames,
        "heartbeats": heartbeats,
        "valid_heartbeats": valid_heartbeats,
        "parameter_values": parameter_values,
        "valid_parameter_values": valid_parameter_values,
        "parameter_count": parameter_count,
        "unique_parameter_indices": indexed_parameters,
        "unindexed_parameter_values": len(parameter_indices) - indexed_parameters,
        "missing_parameter_indices": max(0, parameter_count - indexed_parameters),
        "serial_parameters": dict(sorted(serial_parameters.items())),
        "message_ids": message_ids,
    }


def open_serial_without_reset(port_name: str) -> serial.Serial:
    """Open the native USB serial port with modem-control lines deasserted.

    ESP32 USB Serial/JTAG can interpret host DTR/RTS transitions as reset or
    boot-mode requests.  Set the desired line state before opening so repeated
    bridge tests do not accidentally reboot the AirLink under test.
    """
    port = serial.Serial()
    port.port = port_name
    port.baudrate = 115200
    port.timeout = 0.1
    port.write_timeout = 1
    port.rtscts = False
    port.dsrdtr = False
    port.dtr = False
    port.rts = False
    port.open()
    return port


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--settle", type=float, default=5.0)
    parser.add_argument("--duration", type=float, default=8.0)
    parser.add_argument("--cli-status", action="store_true",
                        help="escape to temporary CLI, read status, then reboot")
    parser.add_argument("--request-params", action="store_true",
                        help="send PARAM_REQUEST_LIST and report PARAM_VALUE completeness")
    args = parser.parse_args()

    cli_status = ""
    with open_serial_without_reset(args.port) as port:
        settle_deadline = time.monotonic() + args.settle
        while time.monotonic() < settle_deadline:
            port.read(4096)
        outbound = gcs_heartbeat()
        port.write(outbound)
        if args.request_params:
            port.flush()
            time.sleep(1.0)
            port.write(param_request_list())
        port.flush()
        captured = bytearray()
        deadline = time.monotonic() + args.duration
        heartbeat_sequence = 2
        next_heartbeat = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            captured.extend(port.read(4096))
            if args.request_params and time.monotonic() >= next_heartbeat:
                port.write(gcs_heartbeat(heartbeat_sequence))
                port.flush()
                heartbeat_sequence = (heartbeat_sequence + 1) & 0xFF
                next_heartbeat += 1.0
        if args.cli_status:
            port.write(b"+++AIRLINK-CLI\r\n")
            port.flush()
            time.sleep(0.5)
            port.read(4096)
            port.write(b"status\r\n")
            port.flush()
            time.sleep(1.0)
            cli_status = port.read(4096).decode("utf-8", errors="replace")
            port.write(b"reboot\r\n")
            port.flush()

    result = parse_frames(bytes(captured))
    result["gcs_heartbeat_bytes_sent"] = len(outbound)
    if args.cli_status:
        result["cli_status"] = cli_status
    print(json.dumps(result, sort_keys=True))
    passed = result["valid_heartbeats"] > 0
    if args.request_params:
        passed = (passed and result["parameter_count"] > 0 and
                  result["missing_parameter_indices"] == 0)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
