#!/usr/bin/env python3
"""AirLink C5 V1 USB factory-test runner."""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import secrets
import time
from datetime import datetime, timezone
from pathlib import Path

import serial

ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"


def command(port: serial.Serial, text: str, timeout: float = 15.0) -> dict:
    port.reset_input_buffer()
    port.write((text + "\n").encode())
    port.flush()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if line.startswith("{"):
            return json.loads(line)
    raise TimeoutError(text)


def write_private_record(path: Path, record: dict) -> None:
    temporary = path.with_name(f".{path.name}.{secrets.token_hex(6)}.tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(record, output, indent=2)
            output.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def write_csv_record(path: Path, row: dict) -> None:
    fieldnames = ["serial", "timestamp", "operator", "passed", "hardware_id"]
    rows = []
    if path.exists():
        with path.open(newline="", encoding="utf-8") as source:
            rows = [existing for existing in csv.DictReader(source) if existing.get("serial") != row["serial"]]
    rows.append(row)
    temporary = path.with_name(f".{path.name}.{secrets.token_hex(6)}.tmp")
    with temporary.open("x", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    os.replace(temporary, path)


def password_valid(password: str) -> bool:
    return 12 <= len(password) <= 63 and all(0x21 <= ord(character) <= 0x7E for character in password)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--peer-port")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--operator", required=True)
    parser.add_argument("--initial-password")
    parser.add_argument("--uart-bytes", type=int, default=65536)
    parser.add_argument("--output", type=Path, default=Path("factory-results"))
    parser.add_argument("--skip-manual", action="store_true")
    arguments = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,23}", arguments.serial):
        raise SystemExit("Serial must be 1-24 ASCII letters, digits, dot, underscore, or hyphen")
    if arguments.initial_password is not None and not password_valid(arguments.initial_password):
        raise SystemExit("Initial password must be 12-63 printable ASCII characters without spaces")

    arguments.output.mkdir(mode=0o700, parents=True, exist_ok=True)
    arguments.output.chmod(0o700)
    private_path = arguments.output / f"{arguments.serial}.json"
    csv_path = arguments.output / "results.csv"

    previous = None
    if private_path.exists():
        previous = json.loads(private_path.read_text(encoding="utf-8"))
        if previous.get("serial") != arguments.serial or previous.get("passed"):
            raise SystemExit(f"Refusing to overwrite completed or mismatched record: {private_path}")
        password = previous.get("initial_password", "")
        if not password_valid(password):
            raise SystemExit(f"Existing retry record has no valid initial password: {private_path}")
        if arguments.initial_password is not None and arguments.initial_password != password:
            raise SystemExit("--initial-password does not match the saved retry record")
    else:
        password = arguments.initial_password or "".join(secrets.choice(ALPHABET) for _ in range(16))

    record = {
        "serial": arguments.serial,
        "operator": arguments.operator,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "port": arguments.port,
        "initial_password": password,
        "status": "in_progress",
        "passed": False,
        "results": [],
    }
    if previous is not None:
        record["previous_attempt"] = {
            "timestamp": previous.get("timestamp"),
            "status": previous.get("status"),
            "results": previous.get("results", []),
        }
    # Persist the credential before the irreversible identity command can run.
    write_private_record(private_path, record)

    results: list[dict] = []
    info: dict = {}
    try:
        with serial.Serial(arguments.port, 115200, timeout=0.5) as device:
            time.sleep(1)
            info = command(device, "factory info")
            results.append(info)
            results.append(command(device, "factory nvs"))
            scan = command(device, "factory wifi-scan", 30)
            results.append(scan)
            results.append({
                "test": "wifi_bands",
                "ok": scan.get("aps_2g", 0) > 0 and scan.get("aps_5g", 0) > 0,
                "aps_2g": scan.get("aps_2g", 0),
                "aps_5g": scan.get("aps_5g", 0),
            })
            for baud in (57600, 115200, 230400, 460800, 921600):
                results.append(command(device, f"factory uart-prbs {baud} {arguments.uart_bytes}", 60))
            results.append(command(device, "factory ble", 10))

            if arguments.peer_port:
                with serial.Serial(arguments.peer_port, 115200, timeout=0.5) as peer:
                    for bitrate in (125000, 250000, 500000, 1000000):
                        local_rate = command(device, f"factory can-bitrate {bitrate}")
                        peer_rate = command(peer, f"factory can-bitrate {bitrate}")
                        before = command(peer, "factory info")["can_rx"]
                        sent = command(device, "factory can-send 123 5a")
                        time.sleep(0.5)
                        after = command(peer, "factory info")["can_rx"]
                        results.append({
                            "test": "can_pair",
                            "bitrate": bitrate,
                            "ok": bool(local_rate.get("ok") and peer_rate.get("ok") and
                                       sent.get("ok") and after > before),
                            "before": before,
                            "after": after,
                        })
            else:
                results.append({"test": "can_pair", "ok": False, "error": "peer_port_required_for_PASS"})

            if not arguments.skip_manual:
                input("Hold BOOT, then press Enter (keep holding). ")
                boot = command(device, "factory boot")
                boot["ok"] = bool(boot.get("pressed"))
                results.append(boot)
                input("Release BOOT, then press Enter. ")
                results.append(command(device, "factory led blue"))
                results.append(command(device, "factory act"))
                indicator_ok = input("Are PWR, ACT pulse, and RGB indicators physically correct? [y/N] ").lower() == "y"
                results.append({"test": "indicator_visual", "ok": indicator_ok})
                for color in ("red", "green", "blue", "white"):
                    results.append(command(device, f"factory led {color}"))
                    color_ok = input(f"Is RGB LED {color}? [y/N] ").lower() == "y"
                    results.append({"test": f"led_visual_{color}", "ok": color_ok})
            else:
                results.append({"test": "manual_checks", "ok": False, "error": "manual_checks_skipped"})

            hardware_ok = bool(info.get("chip") and info.get("usb") and
                               info.get("flash_bytes") == 8388608 and
                               info.get("psram_bytes") == 8388608)
            if hardware_ok and all(result.get("ok", False) for result in results):
                # Identity is the final irreversible operation, after every other test passed.
                results.append(command(device, f"factory identity {arguments.serial} {password}"))
    except Exception as error:  # Preserve the credential and partial evidence for a safe retry.
        results.append({"test": "runner_exception", "ok": False, "error": f"{type(error).__name__}: {error}"})

    passed = bool(results and all(result.get("ok", False) for result in results) and
                  info.get("chip") and info.get("usb") and
                  info.get("flash_bytes") == 8388608 and info.get("psram_bytes") == 8388608 and
                  any(result.get("test") == "identity" and result.get("ok") for result in results))
    record.update({
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "status": "passed" if passed else "failed",
        "passed": passed,
        "results": results,
    })
    write_private_record(private_path, record)
    write_csv_record(csv_path, {
        "serial": arguments.serial,
        "timestamp": record["timestamp"],
        "operator": arguments.operator,
        "passed": passed,
        "hardware_id": "airlink-c5-mesh-v1",
    })
    print(json.dumps({"serial": arguments.serial, "passed": passed, "json": str(private_path)}))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
