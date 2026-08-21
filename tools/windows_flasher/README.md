# AirLink Windows Local Flasher

Local Web Serial flasher for AirLink C5 Mesh V1. It is intentionally limited
to the bundled `v0.2.0-dev` ESP32-C5 N8R8 release. It verifies the connected
chip and 8 MB flash, preserves NVS and factory identity, and can create a
one-time initial Wi-Fi/admin password record for an unprovisioned device.

## Windows use

1. Extract the complete `windows_flasher` folder.
2. Double-click `start_flasher.bat`.
3. Use Microsoft Edge or Google Chrome if the default browser does not support
   Web Serial.
4. Disconnect every non-USB 5 V source. Hold BOOT, press RESET, release BOOT.
5. Generate or enter the initial password, then copy or download the credential
   record before flashing.
6. Follow the remaining instructions shown in the page.

The local server binds only to `127.0.0.1`. The browser talks directly to the
selected USB serial device; firmware and logs are not uploaded.

## Developer build

```sh
npm install
npm run build
```

The distributable site is generated in `www/`. Run `npm run dev` for local
development. The bundled firmware files come from the verified AirLink
`v0.2.0-dev` release package.

## Safety design

- Browser support and secure-context checks.
- Espressif USB VID filter and explicit ESP32-C5 chip-name gate.
- Live 8 MB flash-capacity check and MAC-derived Wi-Fi name preview.
- Manifest hardware/version/capacity checks and SHA-256 validation before use.
- Fixed offsets `0x2000`, `0x8000`, `0x19000`, and `0x30000`.
- `eraseAll` is always false; neither NVS `0x9000` nor identity `0x1C000` is
  included in the write set.
- Blank devices receive only a one-time provisioning record at `0x2C000`;
  firmware consumes and erases it on first normal boot. Existing factory
  identity always wins and is never overwritten.
- Passwords are never uploaded or stored in browser local storage. The operator
  must explicitly copy or download the credential record before flashing.
- Per-image MD5 verification after writing.
- Explicit physical-safety confirmation before the flash button is enabled.
