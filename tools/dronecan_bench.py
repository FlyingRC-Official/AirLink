#!/usr/bin/env python3
"""Safety-checked MAVLink helpers for the AirLink DroneCAN bench."""

from __future__ import annotations

import argparse
import json
import math
import time
from datetime import datetime, timezone
from pathlib import Path

from pymavlink import mavutil


ARMED_FLAG = mavutil.mavlink.MAV_MODE_FLAG_SAFETY_ARMED


def connect(port: str, timeout: float = 15.0):
    link = mavutil.mavlink_connection(port, baud=115200, autoreconnect=True)
    heartbeat = link.wait_heartbeat(timeout=timeout)
    if heartbeat is None:
        link.close()
        raise RuntimeError(f"no flight-controller heartbeat on {port}")
    link.target_system = heartbeat.get_srcSystem()
    link.target_component = heartbeat.get_srcComponent()
    return link, heartbeat


def require_disarmed(heartbeat) -> None:
    if int(heartbeat.base_mode) & ARMED_FLAG:
        raise RuntimeError("refusing bench mutation: flight controller is armed")


def param_name(message) -> str:
    value = message.param_id
    if isinstance(value, bytes):
        return value.split(b"\0", 1)[0].decode("ascii")
    return str(value).split("\0", 1)[0]


def receive_parameters(link, timeout: float, require_complete: bool) -> dict[int, dict]:
    link.mav.param_request_list_send(link.target_system, link.target_component)
    deadline = time.monotonic() + timeout
    last_new = time.monotonic()
    expected = None
    parameters: dict[int, dict] = {}
    while time.monotonic() < deadline:
        message = link.recv_match(type="PARAM_VALUE", blocking=True, timeout=0.5)
        if message is None:
            if parameters and not require_complete and time.monotonic() - last_new > 2.0:
                break
            continue
        if message.get_srcSystem() != link.target_system:
            continue
        index = int(message.param_index)
        message_count = int(message.param_count)
        expected = max(message_count, expected or 0)
        # Named PARAM_VALUE responses use UINT16_MAX as their index. They can
        # arrive asynchronously while a full list is streaming and must not be
        # counted toward the contiguous 0..param_count-1 acceptance criterion.
        if index < 0 or index >= message_count:
            continue
        if index not in parameters:
            last_new = time.monotonic()
        parameters[index] = {
            "index": index,
            "name": param_name(message),
            "value": float(message.param_value),
            "type": int(message.param_type),
        }
        if expected and len(parameters) >= expected:
            break
    if not parameters:
        raise RuntimeError("no parameters received")
    if require_complete:
        expected = expected or 0
        missing = sorted(set(range(expected)) - parameters.keys())
        if missing:
            preview = ",".join(map(str, missing[:20]))
            raise RuntimeError(
                f"incomplete parameter transfer: got {len(parameters)}/{expected}; "
                f"missing indices {preview}"
            )
    return parameters


def command_backup(args) -> None:
    link, heartbeat = connect(args.port)
    try:
        require_disarmed(heartbeat)
        parameters = receive_parameters(link, args.timeout, True)
        document = {
            "captured_at": datetime.now(timezone.utc).isoformat(),
            "port": args.port,
            "system_id": link.target_system,
            "component_id": link.target_component,
            "parameters": [parameters[index] for index in sorted(parameters)],
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
        print(f"PASS backup={args.output} parameters={len(parameters)} armed=no")
    finally:
        link.close()


def read_parameter(link, name: str, timeout: float = 3.0):
    encoded = name.encode("ascii")
    for _ in range(3):
        link.mav.param_request_read_send(link.target_system, link.target_component,
                                         encoded, -1)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            message = link.recv_match(type="PARAM_VALUE", blocking=True, timeout=0.25)
            if message is not None and param_name(message) == name:
                return message
    raise RuntimeError(f"parameter not found: {name}")


def command_get(args) -> None:
    link, heartbeat = connect(args.port)
    try:
        require_disarmed(heartbeat)
        for name in args.names:
            message = read_parameter(link, name)
            print(f"{name}={message.param_value:g} type={int(message.param_type)}")
        print("PASS armed=no")
    finally:
        link.close()


def command_set(args) -> None:
    link, heartbeat = connect(args.port)
    try:
        require_disarmed(heartbeat)
        for assignment in args.assignments:
            name, raw = assignment.split("=", 1)
            desired = float(raw)
            current = read_parameter(link, name)
            param_type = int(current.param_type)
            confirmed = None
            for _ in range(3):
                link.mav.param_set_send(link.target_system, link.target_component,
                                        name.encode("ascii"), desired, param_type)
                deadline = time.monotonic() + 3.0
                while time.monotonic() < deadline:
                    message = link.recv_match(type="PARAM_VALUE", blocking=True, timeout=0.25)
                    if message is not None and param_name(message) == name:
                        confirmed = float(message.param_value)
                        break
                if confirmed is not None and math.isclose(confirmed, desired,
                                                          rel_tol=0, abs_tol=0.01):
                    break
            if confirmed is None or not math.isclose(confirmed, desired,
                                                     rel_tol=0, abs_tol=0.01):
                raise RuntimeError(f"parameter write not confirmed: {name}={desired:g}")
            print(f"SET {name}: {float(current.param_value):g} -> {confirmed:g}")
        print("PASS armed=no writes_confirmed=yes")
    finally:
        link.close()


def command_reboot(args) -> None:
    link, heartbeat = connect(args.port)
    try:
        require_disarmed(heartbeat)
        link.mav.command_long_send(
            link.target_system, link.target_component,
            mavutil.mavlink.MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN,
            0, 1, 0, 0, 0, 0, 0, 0,
        )
        print("PASS reboot_requested=yes armed=no")
    finally:
        link.close()


def command_soak(args) -> None:
    link, heartbeat = connect(args.port)
    try:
        require_disarmed(heartbeat)
        parameter_names = [
            "CAN_D1_UC_SER_EN", "CAN_D1_UC_S1_NOD", "CAN_D1_UC_S1_IDX",
            "CAN_D1_UC_S1_BD", "CAN_D1_UC_S1_PRO",
        ]
        started = time.monotonic()
        deadline = started + args.seconds
        next_heartbeat = started
        next_request = started
        next_report = started + 30.0
        request_index = 0
        pending: tuple[str, float] | None = None
        messages = 0
        heartbeats = 1
        parameter_responses = 0
        bad_data = 0
        last_fc_heartbeat = started
        max_heartbeat_gap = 0.0
        initial_errors = int(getattr(link, "total_receive_errors", 0))
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_heartbeat:
                link.mav.heartbeat_send(
                    mavutil.mavlink.MAV_TYPE_GCS,
                    mavutil.mavlink.MAV_AUTOPILOT_INVALID,
                    0, 0, mavutil.mavlink.MAV_STATE_ACTIVE,
                )
                next_heartbeat = now + 1.0
            if pending is None and now >= next_request:
                name = parameter_names[request_index % len(parameter_names)]
                link.mav.param_request_read_send(link.target_system,
                                                 link.target_component,
                                                 name.encode("ascii"), -1)
                pending = (name, now)
                request_index += 1
                next_request = now + args.param_interval
            message = link.recv_match(blocking=True, timeout=0.2)
            now = time.monotonic()
            if message is not None:
                message_type = message.get_type()
                if message_type == "BAD_DATA":
                    bad_data += 1
                else:
                    messages += 1
                if message_type == "HEARTBEAT" and message.get_srcSystem() == link.target_system:
                    gap = now - last_fc_heartbeat
                    max_heartbeat_gap = max(max_heartbeat_gap, gap)
                    last_fc_heartbeat = now
                    heartbeats += 1
                if (pending is not None and message_type == "PARAM_VALUE" and
                        param_name(message) == pending[0]):
                    parameter_responses += 1
                    pending = None
            if pending is not None and now - pending[1] > 5.0:
                raise RuntimeError(f"parameter response timeout during soak: {pending[0]}")
            if now - last_fc_heartbeat > args.max_heartbeat_gap:
                raise RuntimeError(
                    f"flight-controller heartbeat gap exceeded "
                    f"{args.max_heartbeat_gap:g} seconds; messages={messages} "
                    f"param_responses={parameter_responses}"
                )
            if now >= next_report:
                print(f"PROGRESS seconds={int(now - started)} messages={messages} "
                      f"heartbeats={heartbeats} param_responses={parameter_responses}",
                      flush=True)
                next_report += 30.0
        receive_errors = int(getattr(link, "total_receive_errors", 0)) - initial_errors
        if bad_data or receive_errors:
            raise RuntimeError(f"MAVLink parse/CRC errors: bad_data={bad_data} "
                               f"receive_errors={receive_errors}")
        print(f"PASS seconds={int(args.seconds)} messages={messages} heartbeats={heartbeats} "
              f"param_responses={parameter_responses} bad_data=0 receive_errors=0 "
              f"max_heartbeat_gap={max_heartbeat_gap:.3f}s armed=no")
    finally:
        link.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    subparsers = parser.add_subparsers(dest="command", required=True)

    backup = subparsers.add_parser("backup")
    backup.add_argument("--output", required=True, type=Path)
    backup.add_argument("--timeout", type=float, default=90.0)
    backup.set_defaults(handler=command_backup)

    get = subparsers.add_parser("get")
    get.add_argument("names", nargs="+")
    get.set_defaults(handler=command_get)

    set_parser = subparsers.add_parser("set")
    set_parser.add_argument("assignments", nargs="+")
    set_parser.set_defaults(handler=command_set)

    reboot = subparsers.add_parser("reboot")
    reboot.set_defaults(handler=command_reboot)

    soak = subparsers.add_parser("soak")
    soak.add_argument("--seconds", type=float, default=300.0)
    soak.add_argument("--param-interval", type=float, default=2.0)
    soak.add_argument("--max-heartbeat-gap", type=float, default=5.0)
    soak.set_defaults(handler=command_soak)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    args.handler(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
