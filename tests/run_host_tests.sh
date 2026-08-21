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

# ESP-IDF 6.0.2 returns ESP_ERR_WIFI_NOT_STARTED when band mode is selected
# before esp_wifi_start(). Keep the production initialization order guarded.
python3 - "$root/components/airlink_wifi/airlink_wifi.c" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
start = source.index("esp_err_t airlink_wifi_start(")
end = source.index("\nvoid airlink_wifi_get_status", start)
body = source[start:end]
calls = [
    "esp_wifi_set_mode(mode)",
    "esp_wifi_set_config(WIFI_IF_AP",
    "esp_wifi_set_config(WIFI_IF_STA",
    "esp_wifi_start()",
    "esp_wifi_set_band_mode(band)",
    "esp_wifi_connect()",
]
positions = {call: body.index(call) for call in calls}
assert positions["esp_wifi_set_mode(mode)"] < positions["esp_wifi_set_config(WIFI_IF_AP"]
assert positions["esp_wifi_set_mode(mode)"] < positions["esp_wifi_set_config(WIFI_IF_STA"]
assert positions["esp_wifi_set_config(WIFI_IF_AP"] < positions["esp_wifi_start()"]
assert positions["esp_wifi_set_config(WIFI_IF_STA"] < positions["esp_wifi_start()"]
assert positions["esp_wifi_start()"] < positions["esp_wifi_set_band_mode(band)"]
assert positions["esp_wifi_set_band_mode(band)"] < positions["esp_wifi_connect()"]
PY
