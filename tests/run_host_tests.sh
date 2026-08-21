#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/airlink-host-tests
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/airlink_core/include" \
  -I"$root/components/airlink_router/include" \
  -I"$root/components/airlink_config/include" \
  "$root/tests/test_mavlink.c" \
  "$root/components/airlink_core/airlink_crc.c" \
  "$root/components/airlink_config/airlink_provision.c" \
  "$root/components/airlink_router/airlink_mavlink.c" -o "$out"
"$out"

# A blank device has no "airlink" namespace. The initial open must be
# read-write so first boot can create it before storing the default A/B record.
if grep -F 'nvs_open(NVS_NAMESPACE, NVS_READONLY' \
  "$root/components/airlink_config/airlink_config.c" >/dev/null; then
  echo "config init opens a possibly absent NVS namespace read-only" >&2
  exit 1
fi
