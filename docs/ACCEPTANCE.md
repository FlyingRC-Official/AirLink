# Bench and flight acceptance

## V0.4 Mesh gate

- Build release, recovery and factory-test with ESP-IDF 6.0.2; run C host,
  configurator Node, helper Go and release-package checks. Confirm a V0.3.3
  downgrade still reads the unchanged schema-v2 configuration.
- With one root and eight distinct MAVLink system IDs, exercise direct,
  two-link and three-link paths, parent reselection, relay power loss, node
  restart, channel interference and root loss/restoration. No air node may
  become root.
- Run simultaneous parameter reads, mission uploads, commands, heartbeat and
  telemetry. A duplicate system ID must isolate the later node and must not
  silently transfer control ownership.
- Exercise OTA with 1/4/8 targets, 32-chunk windows, random loss, repair,
  browser disconnect, invalid images, write failures, lost nodes, arming during
  transfer, leaf-to-root activation and forced rollback. Before all targets
  verify, no target may select the staged boot partition.
- Require command RTT P95 <= 200 ms, valid MAVLink loss < 0.5%, non-root path
  reorganization <= 10 s, initial formation <= 60 s and a 24-hour full-load
  run without unexpected reset or persistent free-heap decline.
- After bench acceptance, perform incremental low-risk 2/4/8-aircraft flight
  validation. A software build alone is not RF or flight acceptance.

## Bench gate

- Run PRBS loopback for 10 minutes at each supported UART baud; zero errors.
- Validate Mission Planner and QGroundControl parameter, mission, command and
  telemetry exchange through UDP.
- Connect eight UDP and two TCP clients; stall one TCP client and verify command
  traffic continues. Drop both TCP peers without a clean close and verify the
  slots are reclaimed by keepalive; stop reading on one peer and verify the
  10-second send-stall limit reclaims it.
- At 921600 baud for 30 minutes, measure valid-frame loss below 0.1% and command
  round-trip P95 below 100ms under documented near-range RF conditions.
- Test AP, STA and AP+STA on both bands and perform 100 disconnect/reconnect
  cycles. AP+STA shares one band and channel; it is not simultaneous dual-band.
  Confirm UDP/TCP telemetry is unreachable through the STA address in AP+STA
  mode, and treat STA-only mode as trusted-LAN operation.
- Test first USB flash, LOG_CLI, MAVLink mode, BOOT recovery, successful OTA,
  rejected wrong metadata and forced rollback. Confirm a candidate image stays
  pending for the complete 30-second health window.
- Pair-test CAN at 125k, 250k, 500k and 1Mbit/s; create Bus-Off and confirm
  recovery and counters. Confirm DroneCAN NodeStatus visibility.
- For DroneCAN MAVLink, keep the aircraft disarmed with props removed and one
  5 V source. Configure the air unit for node 125, remote node 10, serial ID 0,
  1 Mbit/s and MAVLink mode. On ArduPilot enable `CAN_D1_UC_SER_EN`, reboot,
  then set `CAN_D1_UC_S1_NOD=125`, `CAN_D1_UC_S1_IDX=0`,
  `CAN_D1_UC_S1_BD=115` and `CAN_D1_UC_S1_PRO=2`, and reboot again.
- Verify NodeStatus/GetNodeInfo, online peer state, zero bit/form/stuff/ACK
  errors and zero bus-off. Arbitration loss is normal CAN bus access and is
  reported separately from errors.
- Through the ground unit USB, read every flight-controller parameter with no
  missing index and run at least three minutes of telemetry plus repeated named
  parameter requests. Require valid bidirectional heartbeats, zero MAVLink CRC
  errors, and no CAN/router queue drops. Reboot the air unit and require the
  wireless and DroneCAN MAVLink paths to recover automatically.
- If standard ArduPilot parameters do not expose a MAVLink backend (see
  ArduPilot issue 31212), stop the release; do not introduce a flight-controller
  patch or private CAN protocol.
- Run a 24-hour full-load soak with no unexpected reboot or declining heap.

## Flight gate

Bench acceptance, single-source 5V validation, UART voltage validation, CAN
termination, external antenna attachment and RF range checks are mandatory.
Start with props removed, then perform only a short, low-risk flight. Firmware
build proof is not physical-board or flight acceptance.
