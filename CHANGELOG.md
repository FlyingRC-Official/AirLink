# Changelog

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
