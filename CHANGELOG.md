# Changelog

## 0.3.1-dev - 2026-08-25

- Added the Windows/macOS single-file configurator and loopback-only native Go
  helper for discovery, safe proxying, profiles, batch deployment and pairing.
- Added atomic USB configuration transactions, capability/config validation,
  Wi-Fi scan and UDP discovery interfaces.
- Added change review, reboot/reconnect verification, bilingual help,
  redacted diagnostics, passive link tests and local/GitHub OTA workflows.
- Fixed transparent byte streams being dropped above 280 bytes by chunking
  UART, TCP and bridge reads while preserving byte order.
- Fixed factory credentials being overwritten on every boot after a user
  changed AP or administrator passwords.
- Replaced the USB CLI escape check with a timeout-aware streaming matcher that
  handles every split point, merged reads and overlapping plus prefixes.
- Added stable ground/air queue-drop fields and monotonic Wi-Fi reconnect totals.
- Fixed USB startup reset loops by increasing the USB task stack, serializing
  log transmission through its queue and degrading non-core service failures
  into reported health state instead of unconditional panic/reboot.
- Added a disarmed-only, 15-second `usb download` window so esptool and the Web
  flasher can enter the ESP32-C5 ROM downloader without permanently disabling
  USB reset recovery.
- Made the Web flasher issue an ESP32-C5 watchdog reset and verify the running
  application/version after USB re-enumeration before reporting success.
- Hardened OTA confirmation by checking return values and gating image validity
  on the real USB, Wi-Fi, Web, UART and CAN service state.
- Bench-verified the final image on two ESP32-C5 modules through USB CLI,
  esptool, USB OTA, reboot/reconnect and image-valid confirmation.
- This development prerelease is bench-oriented. Flight tests, a 24-hour soak
  and validation on physical Mac hardware remain outside its release gate.

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
