#!/usr/bin/env python3
"""Collect secret-safe AirLink logs and optionally decode the flash coredump."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
except ModuleNotFoundError:
    serial = None


COREDUMP_OFFSET = "0x630000"
COREDUMP_SIZE = "0x40000"
ESCAPE = b"+++AIRLINK-CLI\r\n"
SECRET_PATTERN = re.compile(
    r"(?i)((?:ap_|sta_|admin_)?password|authorization|credential|token)([= :]+)([^\r\n ]+)"
)


def redact(text: str) -> str:
    return SECRET_PATTERN.sub(r"\1\2[REDACTED]", text)


def parse_fields(text: str) -> dict[str, str]:
    return dict(re.findall(r"(?m)^([a-z][a-z0-9_]*)=([^\r\n]*)", text))


def read_for(port, seconds: float) -> bytes:
    output = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        output.extend(port.read(4096))
    return bytes(output)


def open_serial(port_name: str):
    if serial is None:
        raise RuntimeError("pyserial is required; use the ESP-IDF Python environment")
    port = serial.Serial()
    port.port = port_name
    port.baudrate = 115200
    port.timeout = 0.05
    port.write_timeout = 2
    port.rtscts = False
    port.dsrdtr = False
    port.dtr = False
    port.rts = False
    port.open()
    return port


def issue(port, command: bytes, wait: float) -> bytes:
    port.reset_input_buffer()
    port.write(command + b"\r\n")
    port.flush()
    return read_for(port, wait)


def collect_live(port_name: str, seconds: float, open_downloader: bool) -> tuple[str, dict[str, str], bool]:
    transcript = bytearray()
    status: dict[str, str] = {}
    downloader_opened = False
    with open_serial(port_name) as port:
        transcript.extend(read_for(port, seconds))
        try:
            response = issue(port, b"status", 0.8)
            transcript.extend(response)
            status = parse_fields(response.decode("utf-8", "replace"))
            temporary = False
            if "firmware" not in status:
                port.reset_input_buffer()
                port.write(ESCAPE[:7])
                port.flush()
                time.sleep(0.05)
                port.write(ESCAPE[7:])
                port.flush()
                transcript.extend(read_for(port, 0.7))
                response = issue(port, b"status", 0.9)
                transcript.extend(response)
                status = parse_fields(response.decode("utf-8", "replace"))
                temporary = "firmware" in status
            if open_downloader and "firmware" in status:
                response = issue(port, b"usb download", 0.5)
                transcript.extend(response)
                downloader_opened = b"OK USB downloader reset window open" in response
            elif temporary:
                try:
                    transcript.extend(issue(port, b"reboot", 0.2))
                except Exception:
                    pass
        except Exception as exc:
            transcript.extend(("\n[collector] live USB unavailable: " + exc.__class__.__name__ + "\n").encode())
    return redact(transcript.decode("utf-8", "replace")), status, downloader_opened


def safe_status(status: dict[str, str]) -> dict[str, str]:
    blocked = {"ap_password", "sta_password", "admin_password", "authorization", "token"}
    return {key: value for key, value in status.items() if key not in blocked and "password" not in key}


def run_logged(command: list[str], output: Path) -> int:
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    output.write_text(redact(completed.stdout), encoding="utf-8")
    return completed.returncode


def self_test() -> int:
    sample = "ap_password=secret\nAuthorization: bearer-token\nboot_stage=usb-ready\n"
    cleaned = redact(sample)
    assert "secret" not in cleaned and "bearer-token" not in cleaned
    assert "boot_stage=usb-ready" in cleaned
    print("PASS collect_crash_diagnostics self-test")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port")
    parser.add_argument("--capture-seconds", type=float, default=5.0)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--read-coredump", action="store_true")
    parser.add_argument("--open-downloader", action="store_true",
                        help="ask a responsive application for its 15-second ROM download window")
    parser.add_argument("--bootloader-ready", action="store_true",
                        help="device is already in ROM download mode; do not toggle reset before reading")
    parser.add_argument("--elf", type=Path, default=Path("build-release/airlink.elf"))
    parser.add_argument("--gdb", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.port:
        parser.error("--port is required")

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = args.output_dir or Path("diagnostic-reports") / f"{stamp}-{args.port}"
    output_dir.mkdir(parents=True, exist_ok=True)
    summary: dict = {
        "schema": "airlink-crash-diagnostics/v1",
        "port": args.port,
        "captured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "coredump_offset": COREDUMP_OFFSET,
        "coredump_partition_size": COREDUMP_SIZE,
        "files": {},
        "errors": [],
    }

    downloader_opened = False
    try:
        transcript, status, downloader_opened = collect_live(
            args.port, args.capture_seconds, args.open_downloader)
        serial_path = output_dir / "serial-redacted.log"
        serial_path.write_text(transcript, encoding="utf-8")
        status_path = output_dir / "status-redacted.json"
        status_path.write_text(json.dumps(safe_status(status), indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
        summary["files"].update(serial_log=serial_path.name, status=status_path.name)
        summary["live_status_available"] = "firmware" in status
        summary["reported_coredump_present"] = status.get("coredump_present")
        summary["previous_boot_stage"] = status.get("previous_boot_stage")
        summary["boot_stage"] = status.get("boot_stage")
    except Exception as exc:
        summary["errors"].append("live capture failed: " + exc.__class__.__name__)

    if args.read_coredump:
        raw_path = output_dir / "coredump.raw"
        read_log = output_dir / "coredump-read.log"
        before = "no-reset" if args.bootloader_ready else "default-reset"
        read_command = [
            sys.executable, "-m", "esptool", "--chip", "esp32c5", "--port", args.port,
            "--before", before, "--after", "hard-reset", "read-flash",
            COREDUMP_OFFSET, COREDUMP_SIZE, str(raw_path),
        ]
        read_code = run_logged(read_command, read_log)
        summary["files"]["coredump_read_log"] = read_log.name
        summary["coredump_read_exit_code"] = read_code
        if read_code == 0 and raw_path.is_file():
            summary["files"]["coredump_raw"] = raw_path.name
            if args.elf.is_file():
                decode_log = output_dir / "coredump-decoded.txt"
                decode_command = [
                    sys.executable, "-m", "esp_coredump", "--chip", "esp32c5",
                    "info_corefile", "--core", str(raw_path), "--core-format", "raw",
                ]
                if args.gdb:
                    decode_command.extend(("--gdb", str(args.gdb)))
                decode_command.append(str(args.elf))
                decode_code = run_logged(decode_command, decode_log)
                summary["files"]["coredump_decoded"] = decode_log.name
                summary["coredump_decode_exit_code"] = decode_code
            else:
                summary["errors"].append("ELF file not found; raw coredump was preserved")
        else:
            summary["errors"].append(
                "coredump read failed; hold BOOT while reconnecting and rerun with --bootloader-ready")

    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"output_dir": str(output_dir), **summary}, indent=2, sort_keys=True))
    return 0 if not summary["errors"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
