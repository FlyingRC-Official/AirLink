#!/usr/bin/env python3
"""Run secret-safe V0.4.0 USB CLI acceptance checks on a physical AirLink."""

from __future__ import annotations

import argparse
import hashlib
import re
import time
from pathlib import Path

import serial


ESCAPE = b"+++AIRLINK-CLI\r\n"
EXPECTED_VERSION = "0.4.0-dev"
DIAGNOSTIC_TERMS = re.compile(
    r"rst:|reset|boot:|ESP_ERROR_CHECK|abort|guru|panic|assert|backtrace|"
    r"airlink|recovery|services ready|rollback|error|failed", re.IGNORECASE)


def open_serial(port_name: str, dtr: bool = False, rts: bool = False) -> serial.Serial:
    port = serial.Serial()
    port.port = port_name
    port.baudrate = 115200
    port.timeout = 0.05
    port.write_timeout = 1
    port.rtscts = False
    port.dsrdtr = False
    port.dtr = dtr
    port.rts = rts
    port.open()
    return port


def read_for(port: serial.Serial, seconds: float) -> bytes:
    output = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        output.extend(port.read(4096))
    return bytes(output)


def summarize_boot(output: bytes) -> str:
    text = output.decode("utf-8", errors="replace")
    lines = []
    for line in text.splitlines():
        safe = re.sub(r"((?:password|authorization|credential|token)[= :]+)\S+",
                      r"\1[REDACTED]", line, flags=re.IGNORECASE)
        if DIAGNOSTIC_TERMS.search(safe):
            lines.append(safe[:240])
    return "\n".join(lines[-80:])


def issue(port: serial.Serial, command: str, wait: float = 0.35) -> str:
    port.reset_input_buffer()
    port.write(command.encode("utf-8") + b"\r\n")
    port.flush()
    return read_for(port, wait).decode("utf-8", errors="replace")


def enter_cli(port: serial.Serial) -> tuple[str, bool]:
    direct = issue(port, "status", 0.5)
    if "firmware=" in direct:
        return direct, False

    port.reset_input_buffer()
    # Exercise a real cross-read match and process the status command that is
    # merged into the same final USB read as the escape tail.
    port.write(ESCAPE[:7])
    port.flush()
    time.sleep(0.05)
    port.write(ESCAPE[7:] + b"status\r\n")
    port.flush()
    output = read_for(port, 1.0).decode("utf-8", errors="replace")
    if "firmware=" not in output:
        excerpt = (output[:300] + " ... " + output[-300:]).replace("\r", "\\r").replace("\n", "\\n")
        raise RuntimeError(f"split USB CLI escape or merged tail command failed: {excerpt!r}")
    return output, "temporary USB CLI" in output


def parse_fields(output: str) -> dict[str, str]:
    return dict(re.findall(r"(?m)^([a-z][a-z0-9_]*)=([^\r\n]*)", output))


def check_atomic_abort(port: serial.Serial, generation: int) -> None:
    if "OK transaction begun" not in issue(port, "config begin"):
        raise RuntimeError("config begin failed")
    if "OK staged" not in issue(port, "config stage udp_port 0"):
        raise RuntimeError("config stage failed")
    if "ERR invalid staged configuration" not in issue(port, "config validate"):
        raise RuntimeError("invalid staged configuration was accepted")
    if "OK transaction aborted" not in issue(port, "config abort"):
        raise RuntimeError("config abort failed")
    after_output = issue(port, "config show", 0.6)
    match = re.search(r"generation=(\d+)", after_output)
    if match is None or int(match.group(1)) != generation:
        raise RuntimeError("aborted transaction changed the configuration generation")


def confirm_ota_health(port_name: str, timeout: float = 60.0) -> None:
    deadline = time.monotonic() + timeout
    last_error = "device did not re-enumerate"
    while time.monotonic() < deadline:
        try:
            with open_serial(port_name) as health_port:
                read_for(health_port, 1.0)
                status_output, temporary = enter_cli(health_port)
                while time.monotonic() < deadline:
                    fields = parse_fields(status_output)
                    version = fields.get("firmware", "")
                    state = int(fields.get("ota_image_state", "-1"))
                    if version != EXPECTED_VERSION:
                        raise RuntimeError(f"OTA rebooted into unexpected firmware {version or 'unknown'}")
                    if state == 2:
                        if temporary:
                            try:
                                issue(health_port, "reboot", 0.2)
                            except serial.SerialException:
                                pass
                        return
                    if state in (3, 4):
                        raise RuntimeError(f"OTA image was rejected or rolled back (state={state})")
                    time.sleep(2.0)
                    status_output = issue(health_port, "status", 0.7)
        except (serial.SerialException, serial.SerialTimeoutException, OSError) as exc:
            last_error = str(exc) or exc.__class__.__name__
            time.sleep(1.0)
    raise RuntimeError(f"OTA health confirmation timed out: {last_error}")


def usb_ota(port: serial.Serial, image_path: Path) -> None:
    port_name = str(port.port)
    image = image_path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    response = issue(port, f"ota begin {len(image)} {digest}", 2.5)
    if "OK ota ready" not in response:
        safe = " ".join(response.replace("\r", " ").replace("\n", " ").split())[-240:]
        raise RuntimeError(f"device rejected USB OTA begin: {safe}")
    for offset in range(0, len(image), 256):
        port.write(image[offset:offset + 256])
    port.flush()
    try:
        result = read_for(port, 8.0).decode("utf-8", errors="replace")
    except serial.SerialException:
        result = ""
    if "OK ota verified; rebooting" not in result:
        raise RuntimeError("USB OTA did not report verified completion")
    port.close()
    confirm_ota_health(port_name)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--settle", type=float, default=3.0,
                        help="discard boot output after the host opens USB")
    parser.add_argument("--diagnose-boot", action="store_true",
                        help="read only and print filtered boot/reset diagnostics")
    parser.add_argument("--summary-only", action="store_true",
                        help="with --diagnose-boot, omit diagnostic log lines")
    parser.add_argument("--open-downloader", action="store_true",
                        help="enter CLI and open the bounded esptool reset window")
    parser.add_argument("--ota-image", type=Path,
                        help="stream a signed AirLink application image over USB CLI")
    parser.add_argument("--dtr", action="store_true", help="assert DTR while diagnosing")
    parser.add_argument("--rts", action="store_true", help="assert RTS while diagnosing")
    args = parser.parse_args()

    with open_serial(args.port, args.dtr, args.rts) as port:
        # Windows may pulse the native USB Serial/JTAG control lines while the
        # handle is being created even when pyserial preconfigures DTR/RTS as
        # deasserted. Let that one boot finish before testing the byte stream.
        boot_output = read_for(port, args.settle)
        if args.diagnose_boot:
            print(f"observed_boots={boot_output.count(b'ESP-IDF v6.0.2')} dtr={int(args.dtr)} rts={int(args.rts)}")
            if not args.summary_only:
                print(summarize_boot(boot_output) or "No boot/reset diagnostic lines observed")
            return 0
        status_output, split_escape = enter_cli(port)
        if args.open_downloader:
            response = issue(port, "usb download", 0.5)
            if "OK USB downloader reset window open" not in response:
                raise RuntimeError("device did not open the USB downloader reset window")
            print(f"PASS port={args.port} downloader_window=15s")
            return 0
        if args.ota_image is not None:
            usb_ota(port, args.ota_image)
            print(f"PASS port={args.port} usb_ota={args.ota_image.name}")
            return 0
        status = parse_fields(status_output)
        required = {
            "firmware", "vehicle_queue_drops", "bridge_tx_queue_drops",
            "wifi_reconnects_total", "wifi_reconnect_streak", "free_heap",
            "reset_reason", "ota_in_progress", "ota_running_partition",
            "ota_image_state", "coredump_present", "coredump_size",
            "previous_boot_stage", "boot_stage",
        }
        missing = sorted(required - status.keys())
        if missing:
            raise RuntimeError(f"status fields missing: {', '.join(missing)}")
        if EXPECTED_VERSION not in status["firmware"]:
            raise RuntimeError(f"unexpected firmware version: {status['firmware']}")

        config_output = issue(port, "config show", 0.6)
        config = parse_fields(config_output)
        generation_match = re.search(r"generation=(\d+)", config_output)
        generation = int(generation_match.group(1)) if generation_match else -1
        if generation < 1:
            raise RuntimeError("invalid configuration generation")
        check_atomic_abort(port, generation)

        # Never print config-show output: it contains local physical-management
        # credentials. Only non-sensitive identity and verdicts leave the process.
        print(
            f"PASS port={args.port} firmware={status['firmware']} "
            f"serial={config.get('serial_number', 'unknown')} "
            f"role={config.get('bridge_role', 'unknown')} "
            f"partition={status['ota_running_partition']} "
            f"image_state={status['ota_image_state']} "
            f"split_escape={'yes' if split_escape else 'not-required'} "
            f"atomic_abort=yes"
        )
        # A prior interrupted test may already have left a MAVLink-configured
        # device in temporary CLI, in which case enter_cli() cannot infer how
        # the mode was entered.  Always restore the persisted MAVLink mode.
        if config.get("usb_mode") == "mavlink":
            try:
                issue(port, "reboot", 0.2)
            except (serial.SerialException, serial.SerialTimeoutException):
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
