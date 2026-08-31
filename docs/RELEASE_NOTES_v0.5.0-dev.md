# AirLink v0.5.0-dev

This development release replaces the legacy Factory Mode and browser
Configurator with a smaller production firmware and stable management
protocols for AirLink-GS.

## Management changes

- USB configuration now uses config protocol 1 and schema 2 transactions:
  `config begin`, one or more `config stage`, `config validate`, then
  `config commit`. A timeout, disconnect, validation failure or abort leaves
  the active configuration unchanged.
- The page-free management API v2 is reachable only through the private
  AirLink AP. It uses challenge-response authentication, per-request HMAC,
  increasing counters and ten-minute idle sessions. It never returns
  passwords or the Mesh fleet key.
- Factory Mode, factory BLE, factory UART/CAN controls, the old Configurator,
  its helper application and the device-hosted HTML UI are removed.
- Release and Recovery are the only firmware variants built by CI.

## Identity and flashing

- Provisioning v2 carries a validated serial number and initial administrator
  password. Identity is committed before configuration, and the provisioning
  sector is erased only after both writes succeed.
- Existing permanent identity is never overwritten. Provisioning v1 remains
  readable for recovery of devices flashed with the previous tool.
- The standalone Web Serial flasher remains the supported path for first ROM
  flashing and recovery flashing.

## Compatibility

The removal of the legacy Configurator and embedded pages is intentional.
AirLink-GS can identify and back up v0.4 devices before upgrading them. Mesh
provisioning, node approval and cluster OTA continue to use the existing COBS
RPC; ordinary module settings use the text config protocol.

This is a development prerelease and is not a flight-qualified production
release. Complete Release/Recovery builds and hardware acceptance are required
before promotion.
