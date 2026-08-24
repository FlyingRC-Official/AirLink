# Factory test and sample acceptance

Flash the factory-test build, connect GPIO23 to GPIO24 for UART loopback, and
run:

```sh
python3 tools/airlink_factory.py --port /dev/cu.usbmodemXXXX \
  --peer-port /dev/cu.usbmodemPEER --serial ALC5V1-0001 \
  --operator NAME --output factory-results
```

For paired CAN testing connect CAN H/L/G between two boards with correct
termination. A missing `--peer-port` or `--skip-manual` run is diagnostic-only
and cannot produce PASS. The tool writes one JSON file and
updates `factory-results/results.csv`. Initial credentials are written to the
private per-board JSON but never to the CSV or normal console output.

Required physical checks not automated by firmware:

1. With power removed, measure resistance between CAN_H and CAN_L and record
   whether this board has an enabled 120-ohm terminator.
2. Confirm GPIO27 is high. Hold BOOT/GPIO28 low while applying power or reset;
   confirm USB/UART0 ROM output reports `DOWNLOAD(USB/UART0)`.
3. Verify only one 5V source is attached. Measure 3V3 before connecting a flight
   controller.
4. Inspect the ACT LED and RGB red, green, blue and white test states.
5. Run 2.4GHz and 5GHz scans with known reference APs in the fixture.
6. Use a BLE observer to confirm the factory advertisement when the BLE factory
   build is enabled. BLE is not an operational transport in v0.1.

Every automated and manual item must be PASS. A failed board must not receive a
PASS label. Board assignment should identify at least one retained reference
board and two CAN pair-test boards.

The output directory is forced to mode `0700`; each per-board JSON containing
the initial credential is created exclusively with mode `0600`. Preserve these
permissions when copying or archiving factory records.
