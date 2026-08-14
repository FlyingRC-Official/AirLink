# Power and wiring safety

## Power

USB VBUS, UART 5V and CAN 5V may be directly connected on the V1 sample. Use
exactly one 5V input. Do not connect USB power and a flight-controller 5V rail
at the same time unless reverse-current protection has been confirmed from the
released schematic and measured on that board.

Before first power-up, inspect for shorts and measure the 3V3 rail. With power
removed, measure CAN_H to CAN_L and record whether the optional 120-ohm
termination is fitted. Only the two physical ends of a CAN bus may be
terminated.

## Connectors

- UART `5 R T G`: AirLink `R` is GPIO23 TX and connects to flight-controller
  RX; AirLink `T` is GPIO24 RX and connects to flight-controller TX.
- CAN `5 H L G`: connect CAN_H, CAN_L and reference ground; use only one 5V
  source.
- UART0 test pads `3 R T G`: 3V3-level production and rescue console only.
- USB test pads `G DN DP 5`: keep any fixture stubs short and symmetric.

Attach and inspect the external U.FL/IPEX antenna before enabling prolonged RF
transmission.
