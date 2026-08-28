# AirLink C5 Mesh V1 Pinout

| Function | ESP32-C5 GPIO | Direction | External label |
|---|---:|---|---|
| CAN TX | 0 | Output | CAN transceiver TXD |
| CAN RX | 1 | Input | CAN transceiver RXD |
| CAN SILENT | 8 | Output, active high | CAN transceiver SILENT |
| WS2812 data | 4 | Output | On-board RGB |
| ACT LED | 9 | Output | On-board green LED |
| UART0 TX | 11 | Output | Debug `T` |
| UART0 RX | 12 | Input | Debug `R` |
| USB D- | 13 | Bidirectional | USB-C D- / `DN` |
| USB D+ | 14 | Bidirectional | USB-C D+ / `DP` |
| Flight-controller TX | 23 | Output | UART `R`, connect to FC RX |
| Flight-controller RX | 24 | Input | UART `T`, connect to FC TX |
| BOOT | 28 | Input/strap | Button to GND |

The module exposed pad, module ground pads and all connector grounds must share
a low-impedance ground. `ANT2` is unused on the standard WROOM-1U-N8R8; the
module's factory ANT1 U.FL/IPEX connection is used.

## Connectors

- UART `5 R T G`: 5V, AirLink TX, AirLink RX, GND.
- CAN `5 H L G`: 5V, CAN_H, CAN_L, GND.
- USB test `G DN DP 5`: GND, D-, D+, VBUS.
- UART0 test `3 R T G`: 3V3, UART0 RX, UART0 TX, GND.

CAN termination must only be enabled at the two physical ends of the bus. GPIO8
has an external pull-up that keeps the transceiver silent during reset; firmware
drives it low during board initialization to enter normal mode.
