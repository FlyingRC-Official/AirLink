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

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/airlink_core/include" \
  "$root/tests/test_stream.c" \
  "$root/components/airlink_core/airlink_stream.c" -o "$out-stream"
"$out-stream"

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

# Release builds must retain a local USB configuration path so a board can be
# recovered without knowing its Wi-Fi credentials.
python3 - "$root/components/airlink_usb/airlink_usb.c" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
required = [
    'strcmp(line, "config show")',
    'strncmp(line, "config set ", 11)',
    'strcmp(line, "config reset")',
    'airlink_config_validate(&config)',
    'airlink_router_fc_armed()',
    'OK saved; reboot required',
    'fc_bytes_in=',
    'config begin',
    'config stage ',
    'config validate',
    'config commit',
    'config abort',
    'airlink_escape_feed(',
    'bridge_tx_queue_drops=',
    'wifi_reconnects_total=',
    'firmware=%s',
    'free_heap=',
    'can_rx_frames=',
    'ota_in_progress=',
]
for marker in required:
    assert marker in source, f"missing USB configuration guard: {marker}"
PY

# The universal image must retain both halves of the point-to-point bridge and
# a physical USB recovery path when the ground role is carrying MAVLink.
python3 - \
  "$root/components/airlink_router/airlink_router.c" \
  "$root/components/airlink_wifi/airlink_wifi.c" \
  "$root/components/airlink_usb/airlink_usb.c" \
  "$root/main/app_main.c" <<'PY'
from pathlib import Path
import sys

router, wifi, usb, main = (Path(path).read_text() for path in sys.argv[1:])
assert 'type == AIRLINK_ENDPOINT_UART || type == AIRLINK_ENDPOINT_BRIDGE' in router
assert '.type = AIRLINK_ENDPOINT_BRIDGE' in wifi
assert 'bridge_connect()' in wifi
assert 'replacing stale TCP client from reconnecting station' in wifi
assert '#define NET_PACKET_QUEUE 64' in wifi
assert '#define NETWORK_TASK_PRIORITY 19' in wifi
assert '#define TCP_TX_BURST 8U' in wifi
assert 'service_tcp_tx(client)' in wifi
assert 'airlink_stream_chunk_size(length - offset' in router
assert '+++AIRLINK-CLI\\r\\n' in usb
assert '#define USB_QUEUE_DEPTH 64' in usb
assert '#define USB_TASK_PRIORITY 19' in usb
assert 'usb_queue_drops=' in usb
assert 'config.bridge_role = AIRLINK_BRIDGE_GROUND' in usb
assert 'hardware_ok && !recovery && !ground_bridge' in main
PY
