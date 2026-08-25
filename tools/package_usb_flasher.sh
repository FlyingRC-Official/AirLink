#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
release_dir=${1:?usage: package_usb_flasher.sh RELEASE_DIR VERSION}
version=${2:?usage: package_usb_flasher.sh RELEASE_DIR VERSION}
flasher="$root/tools/usb_flasher"
firmware="$flasher/public/firmware/$version"
page_name="AirLink-Flasher-$version.html"
bundle_name="AirLink-USB-Flasher-$version"
bundle="$root/dist/$bundle_name"
archive="$root/dist/$bundle_name.zip"
standalone="$root/dist/$page_name"

if [ ! -f "$release_dir/manifest.json" ]; then
    echo "release manifest missing: $release_dir/manifest.json" >&2
    exit 1
fi
if [ -e "$bundle" ] || [ -e "$archive" ] || [ -e "$standalone" ]; then
    echo "USB flasher destination already exists: $bundle, $archive or $standalone" >&2
    exit 1
fi

mkdir -p "$firmware"
cp "$release_dir/bootloader.bin" "$release_dir/partition-table.bin" \
   "$release_dir/ota_data_initial.bin" "$release_dir/airlink.bin" \
   "$release_dir/manifest.json" "$firmware/"

(cd "$flasher" && npm ci && npm run build && npm test)

mkdir -p "$bundle"
cp "$flasher/README.md" "$flasher/THIRD_PARTY_NOTICES.md" \
   "$flasher/start_flasher.bat" "$flasher/start_flasher.command" "$bundle/"
cp "$flasher/www/$page_name" "$bundle/$page_name"
cp "$flasher/www/$page_name" "$standalone"
chmod +x "$bundle/start_flasher.command"
(cd "$root/dist" && zip -qr "$bundle_name.zip" "$bundle_name")
printf '%s\n' "$standalone"
printf '%s\n' "$archive"
