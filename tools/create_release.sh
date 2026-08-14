#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-$root/build}
version=${2:-dev}
dist="$root/dist/airlink-$version"
mkdir -p "$dist"
cp "$build/airlink.bin" "$build/bootloader/bootloader.bin" "$build/partition_table/partition-table.bin" "$dist/"
cp "$build/ota_data_initial.bin" "$build/flash_args" "$dist/"
if command -v esptool >/dev/null 2>&1; then
    if esptool --help 2>&1 | grep -q 'merge-bin'; then
        merge_command=merge-bin; flash_mode_option=--flash-mode; flash_size_option=--flash-size
    else
        merge_command=merge_bin; flash_mode_option=--flash_mode; flash_size_option=--flash_size
    fi
    esptool --chip esp32c5 "$merge_command" -o "$dist/airlink-${version}-merged.bin" \
        "$flash_mode_option" dio "$flash_size_option" 8MB \
        0x2000 "$build/bootloader/bootloader.bin" 0x8000 "$build/partition_table/partition-table.bin" \
        0x19000 "$build/ota_data_initial.bin" 0x30000 "$build/airlink.bin" >&2
else
    if python3 -m esptool --help 2>&1 | grep -q 'merge-bin'; then
        merge_command=merge-bin; flash_mode_option=--flash-mode; flash_size_option=--flash-size
    else
        merge_command=merge_bin; flash_mode_option=--flash_mode; flash_size_option=--flash_size
    fi
    python3 -m esptool --chip esp32c5 "$merge_command" -o "$dist/airlink-${version}-merged.bin" \
        "$flash_mode_option" dio "$flash_size_option" 8MB \
        0x2000 "$build/bootloader/bootloader.bin" 0x8000 "$build/partition_table/partition-table.bin" \
        0x19000 "$build/ota_data_initial.bin" 0x30000 "$build/airlink.bin" >&2
fi
python3 - "$dist" "$version" <<'PY'
import hashlib,json,sys
from datetime import datetime,timezone
from pathlib import Path
p=Path(sys.argv[1]);bins=sorted(p.glob('*.bin'))
digests={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in bins}
(p/'SHA256SUMS.txt').write_text(''.join(f'{digests[f.name]}  {f.name}\n' for f in bins))
(p/'flash_args').write_text(
    '--flash-mode dio --flash-freq 80m --flash-size 8MB\n'
    '0x2000 bootloader.bin\n'
    '0x8000 partition-table.bin\n'
    '0x19000 ota_data_initial.bin\n'
    '0x30000 airlink.bin\n')
(p/'manifest.json').write_text(json.dumps({
    'schema_version':1,'hardware_id':'airlink-c5-mesh-v1','target_chip':'esp32c5','version':sys.argv[2],
    'created_utc':datetime.now(timezone.utc).isoformat(),'flash_bytes':8388608,'psram_bytes':8388608,
    'ota_image':'airlink.bin','images':digests,
    'flash_offsets':{'bootloader.bin':'0x2000','partition-table.bin':'0x8000',
                     'ota_data_initial.bin':'0x19000','airlink.bin':'0x30000'}},indent=2)+'\n')
PY
cp "$root/docs/PINOUT.md" "$root/docs/FACTORY_TEST.md" "$root/docs/ACCEPTANCE.md" \
   "$root/docs/POWER_WIRING.md" "$root/docs/RECOVERY.md" "$root/CHANGELOG.md" \
   "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$dist/"
echo "$dist"
