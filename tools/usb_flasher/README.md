# AirLink Cross-Platform Local USB Flasher

Local Web Serial flasher for AirLink C5 Mesh V1 on Windows and macOS. It is
fixed to the `v0.3.2-dev` ESP32-C5 N8R8 release. The page verifies the GitHub
Release metadata, downloads the same bytes from the corresponding Git tag, and
requires every image to match both the release digest and firmware manifest.

## Windows

1. Extract the complete ZIP.
2. Double-click `start_flasher.bat`. It opens Microsoft Edge first, then Google
   Chrome if Edge is unavailable.
3. Disconnect every non-USB 5 V source. If AirLink is running, select its USB
   port directly; the page requests a bounded downloader window. If it cannot
   connect, hold BOOT, press RESET, then release BOOT and select the port again.
4. Generate or enter the initial password, then copy or download the credential
   record before flashing.
5. Follow the remaining instructions shown in the page.

## macOS

1. Extract the complete ZIP.
2. Double-click `start_flasher.command`. If macOS blocks the launcher, right-click
   `AirLink-Flasher-v0.3.2-dev.html`, choose **Open With**, then select Google
   Chrome or Microsoft Edge.
3. Disconnect every non-USB 5 V source. If AirLink is running, select its USB
   port directly; the page requests a bounded downloader window. If it cannot
   connect, hold BOOT, press RESET, then release BOOT and select the port again.
4. Save the generated or entered initial credentials before flashing.

Safari is not supported. The local HTML requires internet access to GitHub for
approximately 1.1 MB of firmware data. Passwords, logs and USB traffic remain
inside the browser and are never uploaded or stored in local storage.

## Developer build

```sh
npm install
npm run build
```

The distributable single-file page is generated at
`www/AirLink-Flasher-v0.3.2-dev.html`. Run `npm run dev` for local development.
The source firmware copies under `public/firmware/v0.3.2-dev` exist only so the
release tag can provide CORS-readable bytes; they are not embedded in the HTML.

## Safety design

- Windows/macOS Chrome/Edge Web Serial and secure-context checks.
- Espressif USB VID filter and explicit ESP32-C5 chip-name gate.
- Live 8 MB flash-capacity check and MAC-derived Wi-Fi name preview.
- Fixed `v0.3.2-dev` GitHub Release metadata and prerelease checks.
- Manifest plus GitHub Release digest SHA-256 validation before use.
- Fixed offsets `0x2000`, `0x8000`, `0x19000`, and `0x30000`.
- `eraseAll` is always false; neither NVS `0x9000` nor identity `0x1C000` is
  included in the write set.
- Blank devices receive only a one-time provisioning record at `0x2C000`;
  firmware consumes and erases it on first normal boot. Existing factory
  identity always wins and is never overwritten.
- Passwords are never uploaded or stored in browser local storage. The operator
  must explicitly copy or download the credential record before flashing.
- Per-image MD5 verification after writing.
- ESP32-C5 LP-WDT reset followed by application USB re-enumeration and exact
  firmware-version verification; a successful flash write alone is not shown
  as a successful boot.
- Explicit physical-safety confirmation before the flash button is enabled.
