#!/usr/bin/env python3
"""Automated, secret-safe two-unit AirLink Wi-Fi bridge acceptance.

The test uses the air unit's LOG_CLI USB port and the ground unit's USB MAVLink
port. It never sends flight-control commands or CAN frames. Pairing is opt-in;
when requested, the AP password moves directly between the two serial sessions
in memory and is never printed or written to disk.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

try:
    import serial
except ModuleNotFoundError:
    serial = None

from bridge_usb_test import gcs_heartbeat, param_request_list, parse_frames


ESCAPE = b"+++AIRLINK-CLI\r\n"
SAFE_CONFIG_KEYS = (
    "serial_number", "bridge_role", "route_mode", "wifi_mode", "wifi_band",
    "ap_ssid", "sta_ssid", "usb_mode", "uart_baud", "udp_port", "tcp_port",
)
COUNTER_KEYS = (
    "bridge_tx_queue_drops", "vehicle_queue_drops", "usb_queue_drops",
    "uart_rx_overflow", "uart_high_queue_drops", "uart_normal_queue_drops",
    "tcp_queue_alloc_failures",
)


class AcceptanceError(RuntimeError):
    pass


def parse_fields(text: str) -> dict[str, str]:
    return dict(re.findall(r"(?m)^([a-z][a-z0-9_]*)=([^\r\n]*)", text))


def as_int(fields: dict[str, str], key: str) -> int:
    try:
        return int(fields.get(key, "0"))
    except ValueError:
        return 0


def safe_config(config: dict[str, str]) -> dict[str, str]:
    return {key: config.get(key, "") for key in SAFE_CONFIG_KEYS}


def status_summary(status: dict[str, str]) -> dict[str, int | str]:
    keys = (
        "firmware", "uptime_seconds", "reset_reason", "boot_count",
        "coredump_present", "coredump_size", "previous_boot_stage", "boot_stage",
        "fc_seen", "fc_bytes_in", "fc_bytes_out", "fc_parse_errors",
        "wifi_ap_started", "wifi_sta_connected", "wifi_rssi", "wifi_channel",
        "bridge_role", "bridge_connected", "bridge_reconnects",
        "bridge_last_errno", "tcp_last_errno", "bridge_connects_total",
        "tcp_accepts_total", "tcp_disconnects_total", "network_task_loops",
        "tcp_queue_alloc_failures", "tcp_queue_peak", "tcp_queue_current",
        "tcp_send_would_block", "tcp_listener_active",
        "wifi_reconnects_total", "wifi_reconnect_streak", *COUNTER_KEYS,
    )
    numeric = set(keys) - {
        "firmware", "previous_boot_stage", "boot_stage", "bridge_role",
    }
    return {key: as_int(status, key) if key in numeric else status.get(key, "") for key in keys}


def open_port(name: str, settle: float = 2.0):
    if serial is None:
        raise AcceptanceError("pyserial is required; use the ESP-IDF Python environment")
    port = serial.Serial()
    port.port = name
    port.baudrate = 115200
    port.timeout = 0.05
    port.write_timeout = 2
    port.rtscts = False
    port.dsrdtr = False
    port.dtr = False
    port.rts = False
    port.open()
    read_for(port, settle)
    return port


def read_for(port, seconds: float) -> bytes:
    output = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        output.extend(port.read(4096))
    return bytes(output)


def issue(port, command: str, wait: float = 0.6) -> str:
    port.reset_input_buffer()
    try:
        port.write(command.encode("utf-8") + b"\r\n")
        port.flush()
    except Exception as exc:
        raise AcceptanceError("USB command write failed") from exc
    return read_for(port, wait).decode("utf-8", errors="replace")


def enter_cli(port) -> tuple[dict[str, str], bool]:
    direct = issue(port, "status", 0.7)
    direct_fields = parse_fields(direct)
    if "firmware" in direct_fields:
        return direct_fields, False
    port.reset_input_buffer()
    port.write(ESCAPE[:7])
    port.flush()
    time.sleep(0.05)
    port.write(ESCAPE[7:])
    port.flush()
    read_for(port, 0.6)
    fields = parse_fields(issue(port, "status", 0.9))
    if "firmware" not in fields:
        raise AcceptanceError("device did not enter USB CLI")
    return fields, True


def snapshot(port_name: str, reboot_temporary: bool = True) -> tuple[dict[str, str], dict[str, str]]:
    with open_port(port_name) as port:
        status, temporary = enter_cli(port)
        config = parse_fields(issue(port, "config show", 0.9))
        # A previous interrupted run can already have left a MAVLink-configured
        # unit in temporary CLI, so `temporary` alone is not sufficient.  The
        # persisted USB mode is authoritative and must be restored for traffic.
        if reboot_temporary and (temporary or config.get("usb_mode") == "mavlink"):
            try:
                issue(port, "reboot", 0.2)
            except AcceptanceError:
                pass
    return status, config


def require_response(port, command: str, expected: str, label: str) -> None:
    if expected not in issue(port, command, 0.8):
        raise AcceptanceError(f"atomic configuration failed at {label}")


def pair_units(air_port: str, ground_port: str) -> dict[str, str]:
    with open_port(air_port) as air:
        _, temporary = enter_cli(air)
        if temporary:
            raise AcceptanceError("air unit must use LOG_CLI USB mode")
        air_config = parse_fields(issue(air, "config show", 0.9))
    ssid = air_config.get("ap_ssid", "")
    password = air_config.get("ap_password", "")
    if not ssid or len(password) < 8 or "\r" in password or "\n" in password:
        raise AcceptanceError("air AP credentials are invalid")

    with open_port(ground_port) as ground:
        _, _ = enter_cli(ground)
        committed = False
        try:
            require_response(ground, "config begin", "OK transaction begun", "begin")
            require_response(ground, "config stage sta_ssid " + ssid, "OK staged", "SSID")
            require_response(ground, "config stage sta_password " + password, "OK staged", "password")
            require_response(ground, "config validate", "OK valid", "validate")
            require_response(ground, "config commit", "OK committed", "commit")
            committed = True
            try:
                issue(ground, "reboot", 0.2)
            except AcceptanceError:
                pass
        finally:
            if not committed:
                try:
                    issue(ground, "config abort", 0.4)
                except Exception:
                    pass
    return {"target_ssid": ssid, "credentials_persisted_on_host": "no"}


def capture_bridge(port_name: str, duration: float, request_params: bool) -> dict:
    with open_port(port_name) as port:
        port.write(gcs_heartbeat())
        port.flush()
        if request_params:
            time.sleep(1.0)
            port.write(param_request_list())
            port.flush()
        captured = bytearray()
        deadline = time.monotonic() + duration
        sequence = 2
        next_heartbeat = time.monotonic() + 1.0
        next_parameter_retry = time.monotonic() + 10.0
        parameter_observed = False
        while time.monotonic() < deadline:
            captured.extend(port.read(4096))
            now = time.monotonic()
            if request_params and not parameter_observed and now >= next_parameter_retry:
                # The first request can legitimately cross while Wi-Fi/TCP is
                # still reconnecting after temporary CLI. Retry the read-only
                # request until the first valid PARAM_VALUE is observed, then
                # stop so the FC can complete one uninterrupted list.
                parsed = parse_frames(bytes(captured))
                parameter_observed = parsed["valid_parameter_values"] > 0
                if not parameter_observed:
                    port.write(param_request_list(sequence))
                    port.flush()
                    sequence = (sequence + 1) & 0xFF
                    next_parameter_retry = now + 10.0
            if now >= next_heartbeat:
                port.write(gcs_heartbeat(sequence))
                port.flush()
                sequence = (sequence + 1) & 0xFF
                next_heartbeat += 1.0
    return parse_frames(bytes(captured))


def wait_for_bridge_telemetry(port_name: str, timeout: float, request_probe: bool) -> dict:
    with open_port(port_name, 0.5) as port:
        captured = bytearray()
        start = time.monotonic()
        deadline = start + timeout
        sequence = 192
        next_heartbeat = start
        next_probe = start + 5.0
        while time.monotonic() < deadline:
            captured.extend(port.read(4096))
            now = time.monotonic()
            if now >= next_heartbeat:
                port.write(gcs_heartbeat(sequence))
                port.flush()
                sequence = (sequence + 1) & 0xFF
                next_heartbeat += 1.0
            if request_probe and now >= next_probe:
                # PARAM_REQUEST_LIST is read-only and also proves the complete
                # ground->air->FC return path. Some ArduPilot serial profiles
                # do not resume periodic telemetry until such a request arrives.
                port.write(param_request_list(sequence))
                port.flush()
                sequence = (sequence + 1) & 0xFF
                next_probe = now + 10.0
            parsed = parse_frames(bytes(captured))
            if parsed["valid_heartbeats"] > 0:
                return {
                    "recovered": True,
                    "after_seconds": round(now - start, 3),
                    "valid_heartbeats": parsed["valid_heartbeats"],
                    "bytes_received": parsed["bytes_received"],
                }
    parsed = parse_frames(bytes(captured))
    return {
        "recovered": False,
        "after_seconds": None,
        "valid_heartbeats": parsed["valid_heartbeats"],
        "bytes_received": parsed["bytes_received"],
    }


def reboot_air_and_wait(air_port: str, ground_port: str, timeout: float) -> dict:
    with open_port(ground_port) as ground:
        baseline = bytearray()
        baseline_start = time.monotonic()
        baseline_deadline = baseline_start + min(30.0, timeout)
        sequence = 30
        next_heartbeat = baseline_start
        next_probe = baseline_start
        while time.monotonic() < baseline_deadline:
            baseline.extend(ground.read(4096))
            now = time.monotonic()
            if now >= next_heartbeat:
                ground.write(gcs_heartbeat(sequence))
                ground.flush()
                sequence = (sequence + 1) & 0xFF
                next_heartbeat = now + 1.0
            if now >= next_probe:
                ground.write(param_request_list(sequence))
                ground.flush()
                sequence = (sequence + 1) & 0xFF
                next_probe = now + 5.0
            baseline_frames = parse_frames(bytes(baseline))
            if baseline_frames["valid_heartbeats"] > 0:
                break
        baseline_frames = parse_frames(bytes(baseline))
        with open_port(air_port, 1.0) as air:
            air_status, temporary = enter_cli(air)
            if temporary:
                raise AcceptanceError("air unit unexpectedly entered temporary CLI")
            boot_count_before = as_int(air_status, "boot_count")
            issue(air, "reboot", 0.2)

        # Drop everything received before the reboot command completed.  This
        # prevents buffered pre-reboot heartbeats from producing a false 0 ms
        # automatic-recovery result.
        ground.reset_input_buffer()
        captured = bytearray()
        deadline = time.monotonic() + timeout
        sequence = 96
        next_heartbeat = time.monotonic()
        next_probe = time.monotonic() + 5.0
        recovered_after = None
        start = time.monotonic()
        while time.monotonic() < deadline:
            captured.extend(ground.read(4096))
            now = time.monotonic()
            if now >= next_heartbeat:
                ground.write(gcs_heartbeat(sequence))
                ground.flush()
                sequence = (sequence + 1) & 0xFF
                next_heartbeat += 1.0
            if now >= next_probe:
                ground.write(param_request_list(sequence))
                ground.flush()
                sequence = (sequence + 1) & 0xFF
                next_probe = now + 10.0
            parsed = parse_frames(bytes(captured))
            if parsed["valid_heartbeats"] > 0:
                recovered_after = round(now - start, 3)
                break
        recovered = parse_frames(bytes(captured))
    time.sleep(1.0)
    air_after, _ = snapshot(air_port, reboot_temporary=False)
    boot_count_after = as_int(air_after, "boot_count")
    return {
        "baseline_valid_heartbeats": baseline_frames["valid_heartbeats"],
        "recovered_valid_heartbeats": recovered["valid_heartbeats"],
        "recovered_after_seconds": recovered_after,
        "boot_count_before": boot_count_before,
        "boot_count_after": boot_count_after,
    }


def counter_delta(before: dict[str, str], after: dict[str, str], key: str) -> int:
    old = as_int(before, key)
    new = as_int(after, key)
    return new - old if new >= old else new


def record_check(report: dict, name: str, passed: bool | None, detail: str) -> None:
    report["checks"][name] = {
        "result": "SKIP" if passed is None else ("PASS" if passed else "FAIL"),
        "detail": detail,
    }


def self_test() -> int:
    frame = gcs_heartbeat(7)
    parsed = parse_frames(b"noise" + frame + frame)
    assert parsed["frames"] == 2
    assert parsed["valid_heartbeats"] == 2
    assert parsed["bytes_received"] == len(b"noise" + frame + frame)
    fields = parse_fields("firmware=0.3.3-dev\r\nbridge_connected=1\r\n")
    assert fields["bridge_connected"] == "1"
    assert "ap_password" not in safe_config({"ap_password": "secret", "ap_ssid": "air"})
    report = {"checks": {}}
    record_check(report, "sample", True, "ok")
    assert report["checks"]["sample"]["result"] == "PASS"
    print("PASS wifi_bridge_acceptance self-test")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--air-port")
    parser.add_argument("--ground-port")
    parser.add_argument("--expected-version", default="0.3.3-dev")
    parser.add_argument("--pair", action="store_true", help="atomically copy air AP credentials to ground STA")
    parser.add_argument("--settle", type=float, default=10.0)
    parser.add_argument("--ready-timeout", type=float, default=45.0,
                        help="maximum wait for bridge telemetry after temporary CLI reboot")
    parser.add_argument("--duration", type=float, default=12.0)
    parser.add_argument("--parameter-duration", type=float, default=60.0)
    parser.add_argument("--skip-parameters", action="store_true")
    parser.add_argument("--reconnect", action="store_true", help="reboot the air unit and require automatic recovery")
    parser.add_argument("--reconnect-timeout", type=float, default=45.0)
    parser.add_argument("--max-queue-drops", type=int, default=0)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.air_port or not args.ground_port:
        parser.error("--air-port and --ground-port are required")

    report: dict = {
        "schema": "airlink-wifi-bridge-acceptance/v1",
        "air_port": args.air_port,
        "ground_port": args.ground_port,
        "pairing": None,
        "checks": {},
        "failures": [],
        "result": "FAIL",
    }
    failures: list[str] = report["failures"]
    try:
        if args.pair:
            report["pairing"] = pair_units(args.air_port, args.ground_port)
            time.sleep(args.settle)

        air_before, air_config_before = snapshot(args.air_port)
        ground_before, ground_config_before = snapshot(args.ground_port)
        report["devices_before"] = {
            "air": {"status": status_summary(air_before), "config": safe_config(air_config_before)},
            "ground": {"status": status_summary(ground_before), "config": safe_config(ground_config_before)},
        }
        time.sleep(args.settle)

        initial_recovery = wait_for_bridge_telemetry(
            args.ground_port, args.ready_timeout, not args.skip_parameters)
        report["initial_recovery"] = initial_recovery
        record_check(
            report,
            "initial_bridge_recovery",
            initial_recovery["recovered"],
            f"after={initial_recovery['after_seconds']}s, "
            f"heartbeats={initial_recovery['valid_heartbeats']}, "
            f"bytes={initial_recovery['bytes_received']}",
        )
        if not initial_recovery["recovered"]:
            failures.append("bridge did not recover after initial ground temporary-CLI reboot")

        # Temporary CLI intentionally tears down the ground bridge. Measure
        # steady-state air-side drops only after that recovery has completed.
        # A heartbeat can reach USB just before the air task has accounted the
        # abandoned frames from the deliberately closed old TCP session, so
        # allow that bounded close/accept transition to settle first.
        time.sleep(2.0)
        air_counter_baseline, _ = snapshot(args.air_port, reboot_temporary=False)
        report["air_counter_baseline"] = status_summary(air_counter_baseline)

        telemetry = capture_bridge(args.ground_port, args.duration, False)
        report["telemetry"] = telemetry
        telemetry_ok = telemetry["valid_heartbeats"] > 0
        record_check(
            report,
            "telemetry_bridge",
            telemetry_ok,
            f"{telemetry['valid_heartbeats']} valid heartbeat(s), "
            f"{telemetry['bytes_received']} byte(s)",
        )
        if not telemetry_ok:
            failures.append("no valid MAVLink heartbeat crossed the bridge")

        if not args.skip_parameters:
            parameters = capture_bridge(args.ground_port, args.parameter_duration, True)
            report["parameters"] = parameters
            parameters_ok = (
                parameters["parameter_count"] > 0
                and parameters["missing_parameter_indices"] == 0
                and parameters["valid_parameter_values"] == parameters["parameter_values"]
            )
            record_check(
                report,
                "parameter_transfer",
                parameters_ok,
                f"{parameters['parameter_count']} parameter index(es), "
                f"{parameters['missing_parameter_indices']} missing, "
                f"{parameters['valid_parameter_values']}/{parameters['parameter_values']} valid frames",
            )
            if parameters["parameter_count"] <= 0:
                failures.append("flight controller reported no parameters")
            elif parameters["missing_parameter_indices"] != 0:
                failures.append(f"missing {parameters['missing_parameter_indices']} parameter indices")
            if parameters["valid_parameter_values"] != parameters["parameter_values"]:
                failures.append("one or more PARAM_VALUE frames failed CRC validation")
        else:
            record_check(report, "parameter_transfer", None, "disabled by --skip-parameters")

        air_after, air_config_after = snapshot(args.air_port)
        ground_after, ground_config_after = snapshot(args.ground_port)
        report["devices_after"] = {
            "air": {"status": status_summary(air_after), "config": safe_config(air_config_after)},
            "ground": {"status": status_summary(ground_after), "config": safe_config(ground_config_after)},
        }

        device_failures: list[str] = []
        for label, status, config, expected_role in (
            ("air", air_after, air_config_after, "air"),
            ("ground", ground_after, ground_config_after, "ground"),
        ):
            if args.expected_version not in status.get("firmware", ""):
                device_failures.append(f"{label} firmware version mismatch")
            if config.get("bridge_role") != expected_role:
                device_failures.append(f"{label} bridge role is not {expected_role}")
        if air_config_after.get("ap_ssid") != ground_config_after.get("sta_ssid"):
            device_failures.append("air AP SSID and ground STA SSID do not match")
        if as_int(air_after, "fc_seen") != 1:
            device_failures.append("air unit did not observe the flight controller")
        if as_int(air_after, "bridge_connected") != 1 or as_int(ground_after, "bridge_connected") != 1:
            device_failures.append("bridge was not connected at post-test status capture")
        record_check(
            report,
            "device_configuration",
            not device_failures,
            "; ".join(device_failures) if device_failures else "roles, version, SSID, FC and connected state are valid",
        )
        failures.extend(device_failures)

        deltas = {}
        for key in COUNTER_KEYS:
            deltas["air_" + key] = counter_delta(air_counter_baseline, air_after, key)
            deltas["ground_" + key] = counter_delta(ground_before, ground_after, key)
        report["counter_deltas"] = deltas
        excessive = {key: value for key, value in deltas.items() if value > args.max_queue_drops}
        record_check(
            report,
            "drop_counters",
            not excessive,
            json.dumps(excessive, sort_keys=True) if excessive else "all monitored deltas are within limit",
        )
        if excessive:
            failures.append("drop/overflow counters exceeded limit: " + json.dumps(excessive, sort_keys=True))

        if args.reconnect:
            time.sleep(args.settle)
            reconnect = reboot_air_and_wait(args.air_port, args.ground_port, args.reconnect_timeout)
            report["reconnect"] = reconnect
            reconnect_ok = (
                reconnect["baseline_valid_heartbeats"] > 0
                and reconnect["recovered_valid_heartbeats"] > 0
                and reconnect["boot_count_after"] > reconnect["boot_count_before"]
            )
            record_check(
                report,
                "automatic_reconnect",
                reconnect_ok,
                f"baseline={reconnect['baseline_valid_heartbeats']}, "
                f"recovered={reconnect['recovered_valid_heartbeats']}, "
                f"after={reconnect['recovered_after_seconds']}s, "
                f"boot={reconnect['boot_count_before']}->{reconnect['boot_count_after']}",
            )
            if reconnect["baseline_valid_heartbeats"] <= 0:
                failures.append("bridge had no heartbeat before reconnect test")
            if reconnect["recovered_valid_heartbeats"] <= 0:
                failures.append("bridge did not recover after air-unit reboot")
        else:
            record_check(report, "automatic_reconnect", None, "enable with --reconnect")
    except Exception as exc:
        failures.append(str(exc) or exc.__class__.__name__)
        record_check(report, "test_execution", False, failures[-1])

    report["result"] = "PASS" if not failures else "FAIL"
    encoded = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True)
    print(encoded)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(encoded + "\n", encoding="utf-8")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
