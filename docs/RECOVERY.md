# Recovery procedure

1. Remove propellers and disconnect every 5V source.
2. Confirm GPIO27 is high; hold BOOT/GPIO28 low while applying USB power or
   resetting the board.
3. Confirm the ESP32-C5 ROM downloader enumerates over USB Serial/JTAG.
4. Build with ESP-IDF 6.0.2 and flash the complete image using `idf.py -p PORT
   flash`, or write the release merged image at offset `0x0` with esptool.
5. Release BOOT and reset. Recovery forces USB `LOG_CLI`; UART/CAN telemetry
   endpoints are not started.

A hardware/Flash/PSRAM mismatch exposes authenticated status only. It is
read-only and refuses configuration changes, reboot, factory reset and OTA.
Do not bypass this check with an image built for another module.
