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

cc -std=c11 -Wall -Wextra -Werror \
  -DCANARD_ENABLE_CANFD=0 -DCANARD_MULTI_IFACE=0 \
  -I"$root/components/airlink_can/include" \
  -I"$root/components/airlink_can/dsdl_generated/include" \
  -I"$root/components/airlink_can/libcanard" \
  "$root/tests/test_dronecan.c" \
  "$root/components/airlink_can/airlink_dronecan.c" \
  "$root/components/airlink_can/libcanard/canard.c" \
  "$root/components/airlink_can/dsdl_generated/src/uavcan.protocol.NodeStatus.c" \
  "$root/components/airlink_can/dsdl_generated/src/uavcan.protocol.GetNodeInfo_res.c" \
  "$root/components/airlink_can/dsdl_generated/src/uavcan.tunnel.Targetted.c" \
  -o "$out-dronecan"
"$out-dronecan"

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

# The CAN transceiver SILENT input is externally pulled high. Production
# firmware must explicitly drive GPIO8 low before the CAN service starts.
python3 - \
  "$root/components/airlink_board/include/airlink_board.h" \
  "$root/components/airlink_board/airlink_board.c" <<'PY'
from pathlib import Path
import sys

header, source = (Path(path).read_text() for path in sys.argv[1:])
assert '#define AIRLINK_GPIO_CAN_SILENT 8' in header
assert 'gpio_set_level(AIRLINK_GPIO_CAN_SILENT, 0)' in source
PY

# ESP-IDF TWAI v2 queues frame pointers. TX frames and their payload buffers
# must have static lifetime and be released only from the completion callback.
python3 - "$root/components/airlink_can/airlink_can.c" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
assert 'static twai_frame_t s_tx_frame;' in source
assert 'static uint8_t s_tx_data[8];' in source
assert '.on_tx_done = on_tx_done' in source
assert 'event->done_tx_frame == &s_tx_frame' in source
assert '.buffer = s_tx_data' in source
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
    'coredump_present=',
    'previous_boot_stage=',
    'can_rx_frames=',
    'ota_in_progress=',
]
for marker in required:
    assert marker in source, f"missing USB configuration guard: {marker}"
PY

# OTA rollback confirmation must be based on real service health and must not
# silently discard confirmation/rollback failures.
python3 - \
  "$root/components/airlink_ota/airlink_ota.c" \
  "$root/main/app_main.c" <<'PY'
from pathlib import Path
import sys

ota, main = (Path(path).read_text() for path in sys.argv[1:])
assert 'atomic_exchange(&s_services_ready, ready)' in ota
assert 'confirm_err = esp_ota_mark_app_valid_cancel_rollback()' in ota
assert 'rollback_err = esp_ota_mark_app_invalid_rollback_and_reboot()' in ota
assert 'strcmp(descriptor.version, s_stream.expected_version)' in ota
assert 'airlink_ota_health_heartbeat(true)' not in main
assert 'services_healthy(&wifi)' in main
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
assert 'slot->endpoint.direction == AIRLINK_ENDPOINT_DIRECTION_VEHICLE' in router
assert 'AIRLINK_ENDPOINT_DIRECTION_INTERNAL' in router
assert '.type = AIRLINK_ENDPOINT_BRIDGE' in wifi
assert '.direction = AIRLINK_ENDPOINT_DIRECTION_VEHICLE' in wifi
assert 'bridge_connect()' in wifi
assert 'replacing stale TCP client from reconnecting station' in wifi
assert '#define NET_PACKET_QUEUE 256' in wifi
assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in wifi
assert 'xQueueCreateStatic(NET_PACKET_QUEUE' in wifi
assert 'tcp_queue_alloc_failures' in wifi
assert '#define NETWORK_TASK_PRIORITY 19' in wifi
assert '#define TCP_TX_BATCH_PACKETS 32U' in wifi
assert '#define TCP_TX_BATCH_BYTES 1440U' in wifi
assert 'xQueuePeek(client->tx_queue' in wifi
assert 'esp_wifi_set_ps(WIFI_PS_NONE)' in wifi
assert 'xQueueSend(s_bridge_tx_queue, &packet, 0)' in wifi
assert 'xQueueSend(client->tx_queue, &packet, 0)' in wifi
assert 'void airlink_wifi_prepare_restart(void)' in wifi
assert 'shutdown(s_bridge_socket, SHUT_RDWR)' in wifi
assert 's_status.bridge_tx_queue_drops = increment_saturated' in wifi
assert 'uxQueueMessagesWaiting' in wifi
assert 'service_tcp_tx(client)' in wifi
assert 'airlink_stream_chunk_size(length - offset' in router
transparent = router[router.index('if (s_mode == AIRLINK_ROUTE_TRANSPARENT)'):]
transparent = transparent[:transparent.index('return ESP_OK;')]
assert 'observe_vehicle_safety(source, data, length);' in transparent
assert transparent.index('observe_vehicle_safety') < transparent.index('route(source')
observer = router[router.index('static void observe_vehicle_safety'):router.index('static void route(')]
assert 'airlink_mavlink_parse_byte' in observer
assert 'observe_vehicle_frame' in observer
fc_seen = router[router.index('bool airlink_router_fc_seen(void)'):router.index('bool airlink_router_fc_armed(void)')]
assert 'atomic_load(&s_fc_last_seen_us)' in fc_seen
assert 'portMAX_DELAY' not in fc_seen
assert 'esp_rom_software_reset_system();' in usb
assert 'airlink_usb_system_restart();' in usb
assert 'airlink_wifi_prepare_restart();' in usb
acceptance = Path(sys.argv[4]).parents[1] / 'tools' / 'airlink_cli_acceptance.py'
assert 'config.get("usb_mode") == "mavlink"' in acceptance.read_text()
assert '+++AIRLINK-CLI\\r\\n' in usb
assert '#define USB_QUEUE_DEPTH 64' in usb
assert '#define USB_TASK_PRIORITY 19' in usb
assert '#define USB_TASK_STACK_SIZE 8192' in usb
assert 'usb download' in usb
assert 'USB_DOWNLOAD_WINDOW_US' in usb
assert 'usb_queue_drops=' in usb
assert 'config->bridge_role = AIRLINK_BRIDGE_GROUND' in usb
assert 'hardware_ok && !recovery &&' in main
assert 'mesh_air || (!mesh_ground && !ground_bridge && !can_fc)' in main
assert 'airlink_usb_reset_guard_enable();' in main
assert 'ESP_ERROR_CHECK(airlink_usb_start' not in main
assert 'airlink_ota_health_heartbeat(healthy);' in main
assert 'airlink_diag_mark_boot_stage("healthy")' in main
PY

# V0.4 Mesh must remain separate from schema-v2 configuration and must use
# application-layer authenticated encryption because MESH_DATA_ENC is not an
# implemented ESP-IDF service.
python3 - \
  "$root/components/airlink_mesh/airlink_mesh_config.c" \
  "$root/components/airlink_mesh/airlink_mesh_codec.c" \
  "$root/components/airlink_mesh/airlink_mesh.c" \
  "$root/main/app_main.c" <<'PY'
from pathlib import Path
import sys

config, codec, mesh, main = (Path(path).read_text() for path in sys.argv[1:])
for marker in [
    'mesh_a', 'mesh_b', 'pending_a', 'pending_b', 'allow_a', 'allow_b',
    'AIRLINK_MESH_BAND_2G', 'AIRLINK_MESH_BAND_5G_RESERVED',
    'airlink_mesh_channel_allowed', 'airlink-mesh-provision/v1',
]:
    assert marker in config, f'missing Mesh persistence guard: {marker}'
for marker in [
    'PSA_ALG_HKDF(PSA_ALG_SHA_256)', 'psa_aead_encrypt', 'psa_aead_decrypt',
    'AIRLINK_MESH_SESSION_ID_SIZE', 'seen_bitmap', 'UINT64_MAX',
]:
    assert marker in codec, f'missing Mesh crypto guard: {marker}'
for marker in [
    'psa_crypto_init()', 'esp_wifi_set_mode(WIFI_MODE_STA)',
    'esp_mesh_fix_root(true)', 'MESH_ROOT', 'MESH_TOPO_TREE',
    'esp_mesh_set_capacity_num', 'WIFI_BAND_MODE_2G_ONLY', 'MESH_OPT_SEND_GROUP',
    'cfg.router.allow_router_switch = false',
    'AIRLINK_MESH_ISOLATION_DUPLICATE_SYSTEM_ID',
    'OTA_WINDOW_CHUNKS 32U', 'OTA_CHUNK_SIZE 1024U',
    'ota_ack_bitmap', 'root_ota_activation_task',
    'airlink_mesh_update_network', 'CONFIG_PHASE_PREPARE',
    'MESH_HIGH_QUEUE_DEPTH', 'MESH_NORMAL_QUEUE_DEPTH',
]:
    assert marker in mesh, f'missing Mesh runtime guard: {marker}'
assert 'airlink_mesh_config_init(&mesh_snapshot)' in main
assert 'configured_mesh ? AIRLINK_ROUTE_MAVLINK' in main
assert 'airlink_mesh_start(&mesh_snapshot.value' in main
PY

# DroneCAN tunneling must stay non-blocking, retain independent priorities, and
# recover a bus-off event without enabling data TX in diagnostic/factory mode.
python3 - \
  "$root/components/airlink_can/airlink_can.c" \
  "$root/components/airlink_config/airlink_config.c" <<'PY'
from pathlib import Path
import sys

can, config = (Path(path).read_text() for path in sys.argv[1:])
for marker in [
    'HIGH_QUEUE_DEPTH', 'NORMAL_QUEUE_DEPTH', 'HIGH_BURST_LIMIT',
    'xQueueSend(queue, &packet, 0)', 'high_queue_drops', 'normal_queue_drops',
    'TWAI_ERROR_BUS_OFF', 'twai_node_recover',
    'options->tunnel_enabled && !options->factory_mode',
    'UAVCAN_TUNNEL_PROTOCOL_MAVLINK2', 'NODE_STATUS_INTERVAL_US',
    'UAVCAN_PROTOCOL_GETNODEINFO_ID',
]:
    assert marker in can, f'missing DroneCAN production guard: {marker}'
for marker in [
    'config_record_v1_t', 'record_v1_crc', 'migrate_v1',
    'AIRLINK_FC_TRANSPORT_UART', 'schema_migrated', 'save_locked(&s_config)',
]:
    assert marker in config, f'missing config migration guard: {marker}'
PY
