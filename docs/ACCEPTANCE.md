# Bench and flight acceptance

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
- Run a 24-hour full-load soak with no unexpected reboot or declining heap.

## Flight gate

Bench acceptance, single-source 5V validation, UART voltage validation, CAN
termination, external antenna attachment and RF range checks are mandatory.
Start with props removed, then perform only a short, low-risk flight. Firmware
build proof is not physical-board or flight acceptance.
