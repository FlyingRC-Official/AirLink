#!/usr/bin/env python3
"""Capture synchronized, secret-safe logs from an AirLink pair."""

from __future__ import annotations

import argparse
import re
import time

import serial


ESCAPE = b"+++AIRLINK-CLI\r\n"


def open_port(name: str) -> serial.Serial:
    port = serial.Serial()
    port.port = name
    port.baudrate = 115200
    port.timeout = 0.02
    port.write_timeout = 2
    port.dsrdtr = False
    port.rtscts = False
    port.dtr = False
    port.rts = False
    port.open()
    return port


def redact(text: str) -> str:
    return re.sub(
        r"(?im)^(ap_password|sta_password|admin_password)=.*$",
        r"\1=<redacted>",
        text,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--air-port", required=True)
    parser.add_argument("--ground-port", required=True)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--enter-ground-cli", action="store_true")
    args = parser.parse_args()

    air = open_port(args.air_port)
    ground = open_port(args.ground_port)
    try:
        time.sleep(1.0)
        air.reset_input_buffer()
        ground.reset_input_buffer()
        if args.enter_ground_cli:
            ground.write(ESCAPE)
            ground.flush()
        air_log = bytearray()
        ground_log = bytearray()
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            air_log.extend(air.read(4096))
            ground_log.extend(ground.read(4096))
    finally:
        air.close()
        ground.close()

    print("--- AIR ---")
    print(redact(air_log.decode("utf-8", errors="replace")))
    print("--- GROUND ---")
    print(redact(ground_log.decode("utf-8", errors="replace")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
