#!/usr/bin/env python3
"""Create a complete AirLink firmware release directory on any host OS."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    parser.add_argument("version")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = args.build.resolve()
    output = (args.output or root / "dist" / f"airlink-{args.version}").resolve()
    if output.exists():
        raise SystemExit(f"release destination already exists: {output}")
    output.mkdir(parents=True)

    sources = {
        "airlink.bin": build / "airlink.bin",
        "bootloader.bin": build / "bootloader" / "bootloader.bin",
        "partition-table.bin": build / "partition_table" / "partition-table.bin",
        "ota_data_initial.bin": build / "ota_data_initial.bin",
    }
    for name, source in sources.items():
        if not source.is_file():
            raise SystemExit(f"release input missing: {source}")
        shutil.copy2(source, output / name)

    merged = output / f"airlink-{args.version}-merged.bin"
    subprocess.run([
        sys.executable, "-m", "esptool", "--chip", "esp32c5", "merge-bin",
        "-o", str(merged), "--flash-mode", "dio", "--flash-size", "8MB",
        "0x2000", str(sources["bootloader.bin"]),
        "0x8000", str(sources["partition-table.bin"]),
        "0x19000", str(sources["ota_data_initial.bin"]),
        "0x30000", str(sources["airlink.bin"]),
    ], check=True)

    binaries = sorted(output.glob("*.bin"))
    digests = {item.name: hashlib.sha256(item.read_bytes()).hexdigest() for item in binaries}
    (output / "SHA256SUMS.txt").write_text(
        "".join(f"{digest}  {name}\n" for name, digest in digests.items()), encoding="utf-8")
    (output / "flash_args").write_text(
        "--flash-mode dio --flash-freq 80m --flash-size 8MB\n"
        "0x2000 bootloader.bin\n0x8000 partition-table.bin\n"
        "0x19000 ota_data_initial.bin\n0x30000 airlink.bin\n", encoding="utf-8")
    manifest = {
        "schema_version": 1,
        "hardware_id": "airlink-c5-mesh-v1",
        "target_chip": "esp32c5",
        "version": args.version,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "flash_bytes": 8388608,
        "psram_bytes": 8388608,
        "ota_image": "airlink.bin",
        "images": digests,
        "flash_offsets": {
            "bootloader.bin": "0x2000", "partition-table.bin": "0x8000",
            "ota_data_initial.bin": "0x19000", "airlink.bin": "0x30000",
        },
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for relative in [
        "docs/PINOUT.md", "docs/FACTORY_TEST.md", "docs/ACCEPTANCE.md",
        "docs/POWER_WIRING.md", "docs/RECOVERY.md", "CHANGELOG.md", "LICENSE",
        "THIRD_PARTY_NOTICES.md",
    ]:
        shutil.copy2(root / relative, output / Path(relative).name)
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
