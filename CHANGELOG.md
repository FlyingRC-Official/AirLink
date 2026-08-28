# Changelog

## 0.3.3-dev - 2026-08-28

- Added bidirectional MAVLink byte tunneling over standard
  `uavcan.tunnel.Targetted`, with 120-byte fragmentation, MAVLink2 metadata,
  500 ms keepalives and independent non-blocking priority queues.
- Added a configurable static DroneCAN node with 1 Hz NodeStatus,
  GetNodeInfo response (`com.flyingrc.airlink`), source/target/serial filtering,
  peer state and tunnel traffic/drop diagnostics.
- Added `fc_transport` selection so the flight-controller endpoint is either
  UART or DroneCAN, preserving MAVLink heartbeat and armed-state safety
  interlocks without duplicate command paths.
- Upgraded the persisted configuration schema to v2 with atomic migration of
  valid v1 A/B records; upgrades and factory defaults remain on UART.
- Added Web, USB CLI and standalone configurator controls for local/remote CAN
  node IDs and virtual serial ID, plus host tests for DroneCAN DSDL vectors,
  multi-frame CRC/transfer IDs, filtering, fragmentation and keepalives.
- Vendored libcanard at commit
  `601ed35467e0ac38819df17cd7c918de19f62d58` and generated types from the
  standard DroneCAN DSDL so release builds are reproducible without downloads.
- Included the GPIO8 CAN SILENT fix: firmware drives the externally pulled-up
  transceiver input low before starting TWAI.

## 0.3.2-dev - 2026-08-25

- Drive the externally pulled-up CAN transceiver SILENT input on GPIO8 low
  during board initialization; physical DroneCAN reception now starts in
  normal mode instead of leaving the transceiver silent.
- Fixed ESP32-C5 ECO2 browser flashing by bundling Espressif's v2 C5 stub,
  selecting the SPIMEM1 register base at `0x60003000`, retrying JEDEC reads and
  refusing invalid IDs instead of silently treating them as 4 MB.
- Fixed direct and Helper-proxied Wi-Fi OTA uploads to forward the hardware,
  Flash, PSRAM and SHA-256 headers required by firmware; extended the local-file
  CORS allowlist for the same verified metadata.
- Kept transparent forwarding byte-for-byte while passively parsing valid
  vehicle-side MAVLink heartbeats, so armed-flight-controller interlocks also
  apply in transparent mode.
- Added an exact-tag release quality gate: release artifacts and the GitHub
  Prerelease cannot be built or published until every `firmware-ci` job for the
  same tag commit succeeds.
- Made `master` the explicit Release target and documented switching the GitHub
  default branch from the obsolete V0.2.1 `main` baseline.
- Added persistent crash diagnostics, safer degraded startup, monotonic bridge
  queue statistics and repeatable two-module bridge acceptance automation.
- Fixed a bridge-congestion task-watchdog reset by making router output queues
  non-blocking and the watchdog-supervised FC activity read lock-free.
- Moved 256-frame bridge/TCP queue storage to PSRAM so the air unit can retain
  complete ArduPilot parameter bursts without exhausting contiguous internal
  RAM; socket errno, listener, accept/disconnect and allocation counters are
  now available through USB and Web diagnostics.
- Batched TCP output up to one MSS and disabled STA modem sleep for the powered
  real-time bridge, preventing a marginal link from stalling its TCP window
  during telemetry and parameter bursts.
- Replaced the C5/lwIP premature non-blocking-connect check with a bounded TCP
  handshake and added reconnect backoff plus transient socket-error handling.
- Replaced the C5 CPU-only application restart with a controlled ROM system
  reset so native USB reliably disconnects and re-enumerates on Windows.
- Hardened bridge automation so an interrupted temporary USB CLI session is
  restored from persisted mode and reconnect checks require a real boot-count
  increment instead of accepting buffered pre-reboot heartbeats.
- Bench-verified 1,270/1,270 ArduPilot parameters with valid MAVLink CRCs, then
  711,913 telemetry bytes and 298 heartbeats over five minutes at -75 dBm with
  zero new queue/overflow drops; an air-unit restart recovered automatically.

## 0.3.1-dev - 2026-08-25

- Added persistent boot-stage breadcrumbs, flash coredump visibility and
  watchdog-to-coredump handling for diagnosing panics and stalled USB tasks.
- Added secret-safe crash collection and repeatable two-unit Wi-Fi bridge
  acceptance tools with machine-readable PASS/FAIL reports.
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
