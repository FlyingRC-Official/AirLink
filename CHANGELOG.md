# Changelog

## 0.3.0-dev - 2026-08-24

- Added a universal two-unit wireless bridge: the same image can be configured
  as `air`, `ground` or `off` later through physical USB or the authenticated
  Web UI.
- Added release USB configuration commands for route, UART, Wi-Fi, USB, CAN,
  credentials and bridge role, with CRC-protected A/B persistence and armed
  flight-controller write/reboot interlocks.
- Added temporary `+++AIRLINK-CLI` escape from ground-side USB MAVLink so local
  recovery and role changes never require Wi-Fi access.
- Added a ground-side TCP bridge client, automatic Wi-Fi/TCP reconnection and
  immediate stale-session replacement after a USB reset.
- Hardened parameter-list transfer with larger queues, corrected task
  priorities and burst TCP draining; status now exposes bridge connectivity and
  USB/TCP queue-drop counters.
- Added a no-reset USB bridge test tool that deasserts DTR/RTS and validates
  MAVLink heartbeat CRCs and complete parameter-index coverage.
- Bench-qualified the bridge bidirectionally with an ArduPilot flight
  controller: QGroundControl and Mission Planner both connected over the ground
  unit, and the automated test received all 1270 reported parameters with no
  missing indices, TCP drops, UART overflows or MAVLink parse errors.
- This remains a development prerelease; flight and long-duration RF acceptance
  remain required for each hardware installation.

## 0.2.1-dev - 2026-08-21

- Fixed the ESP-IDF 6.0.2 Wi-Fi initialization order so band mode is selected
  only after `esp_wifi_start()`, preventing `ESP_ERR_WIFI_NOT_STARTED` reset
  loops on normal startup.
- Replaced the Windows-only localhost flasher with a Windows/macOS single-file
  local Web Serial page that requires no Python or local server.
- Pinned the flasher to `v0.2.1-dev` and required firmware bytes to match both
  the manifest SHA-256 values and GitHub Release asset digests.
- Added Windows Edge/Chrome and macOS Chrome/Edge launchers while retaining
  chip, flash-capacity, identity, password and fixed-offset safety checks.
- This remains a development prerelease; software and CI validation do not
  constitute electrical, RF, interrupted-update or flight acceptance.

## 0.2.0-dev - 2026-08-21

- Fixed first boot on a blank NVS partition so the `airlink` namespace and
  default A/B configuration record are created instead of causing a reset loop.
- Added a local Windows Edge/Chrome Web Serial flasher with bundled, verified
  ESP32-C5 images, live chip/flash checks and protected one-time provisioning.
- Added generated or operator-supplied initial Wi-Fi/admin passwords with
  copy/download records; existing factory identity is detected and preserved.
- Restricted armed-state safety interlocks to the pinned flight-controller
  system's primary autopilot heartbeat so companion/GCS heartbeats cannot clear
  the lock.
- Hardened OTA with an image-embedded hardware marker, receive-timeout retries
  and a renewable health lease throughout the confirmation window.
- Serialized runtime configuration access and rejected unterminated factory
  identity records before any string operations.
- Made factory identity programming the last production step and retained
  in-progress credentials for safe test retries.
- Forced factory-test USB into `LOG_CLI` and bounded high-priority UART bursts
  so normal telemetry cannot be starved indefinitely.
- This is a development prerelease for controlled hardware and bench validation;
  physical-board, RF, endurance and flight acceptance remain pending.

## 0.1.0-dev - 2026-08-14

- Initial ESP32-C5-WROOM-1U-N8R8 firmware structure.
- MAVLink-aware and transparent UART routing to Wi-Fi UDP/TCP and optional USB.
- Authenticated bilingual web configuration, rollback OTA and recovery mode.
- Classic TWAI diagnostics with passive DroneCAN NodeStatus observation.
- Factory-test firmware and host-side per-board traceability tool.
- Safety and reliability hardening for armed-state interlocks, MAVLink dialect
  routing, TCP client recovery, OTA confirmation and release packaging.
- AP+STA telemetry is isolated to the private AirLink AP; STA-only telemetry is
  intended for trusted, isolated networks.
- BLE and Mesh operational transports remain disabled for this release.
- This is a development prerelease for controlled hardware and bench validation;
  physical-board, RF, endurance and flight acceptance remain pending.
