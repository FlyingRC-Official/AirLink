# Changelog

## 0.1.0-dev

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
