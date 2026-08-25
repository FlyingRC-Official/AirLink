# Recovery procedure

1. Remove propellers and disconnect every 5V source except the USB connection
   used for recovery.
2. If the application still accepts USB CLI commands, enter CLI with
   `+++AIRLINK-CLI`, run `usb download`, and start esptool or the single-file Web
   flasher within 15 seconds. The command is refused while the flight controller
   is armed, and the USB reset guard is restored automatically when the window
   expires.
3. If the application is unavailable, confirm GPIO27 is high; hold BOOT/GPIO28
   low while applying USB power or resetting the board.
4. Confirm the ESP32-C5 ROM downloader enumerates over USB Serial/JTAG.
5. Build with ESP-IDF 6.0.2 and flash the complete image using `idf.py -p PORT
   flash`, or write the release merged image at offset `0x0` with esptool.
6. Release BOOT and reset. Recovery forces USB `LOG_CLI`; UART/CAN telemetry
   endpoints are not started.

The Web flasher does not report success merely because flash writes completed.
It resets the ESP32-C5, waits for the application USB interface to re-enumerate,
enters CLI and verifies the expected firmware version. If this verification
times out, treat the write as unverified and use the physical BOOT procedure.

A hardware/Flash/PSRAM mismatch exposes authenticated status only. It is
read-only and refuses configuration changes, reboot, factory reset and OTA.
Do not bypass this check with an image built for another module.
