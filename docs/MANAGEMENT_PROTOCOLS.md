# AirLink v0.5 management protocols

## USB config protocol 1

The public line-oriented CLI is:

```text
help
status
config help
config show
config begin
config stage KEY VALUE
config validate
config commit
config abort
config reset
wifi scan
mesh show/create/import/role/reset
ota begin SIZE SHA256
reboot
usb download
```

`config begin` copies the active configuration into RAM for 30 seconds.
`config stage` changes only that copy, `config validate` checks the complete
combination, and `config commit` performs one NVS save. Disconnect, timeout,
failure and `config abort` discard the copy. Commit, reset, reboot, OTA and
download are rejected while an armed flight-controller heartbeat is latched.

The machine-readable response to `config show` is bounded exactly as follows:

```text
OK config protocol=1 schema=2 generation=N
key=value
...
END config
```

Writable keys are `route_mode`, `fc_transport`, `uart_baud`, `wifi_mode`,
`wifi_band`, `ap_ssid`, `ap_password`, `sta_ssid`, `sta_password`, `udp_port`,
`tcp_port`, `usb_mode`, `bridge_role`, `can_bitrate`, `can_node_id`,
`can_remote_node_id`, `can_serial_id`, `led_brightness` and
`admin_password`. `config help` remains the authoritative device-side list.

In persistent USB MAVLink mode, the exact sequence
`+++AIRLINK-CLI\r\n` opens a temporary CLI session. A reboot restores the
persisted USB mode.

## Management API v2 authentication

The only base URL is `http://192.168.4.1`, accepted on the private AP
interface. The STA interface does not serve management requests.

1. Generate a random 32-byte client nonce and POST
   `/api/v2/session/challenge` with `X-AirLink-Client-Nonce` as lower-case hex.
2. The device returns a 16-byte session ID and 32-byte server nonce as hex.
3. Authenticate with
   `HMAC-SHA256(admin_password, "AIRLINK-AUTH-V2" || session_id ||
   client_nonce || server_nonce)` in `X-AirLink-Proof`.
4. Derive the temporary key as
   `HMAC-SHA256(admin_password, "AIRLINK-SESSION-V2" || client_nonce ||
   server_nonce)` and verify the returned server proof over
   `"AIRLINK-SERVER-V2"`.
5. Sign every request with a strictly increasing decimal counter. The canonical
   bytes are `METHOD + "\n" + PATH + "\n" + COUNTER + "\n" +
   lower_hex(SHA256(body))`. Send the session, counter, body hash and lower-case
   HMAC signature in their corresponding `X-AirLink-*` headers.

Unauthenticated challenges expire after 30 seconds. Authenticated sessions
expire after ten idle minutes. Reused counters, bad signatures, body hash
mismatches and requests arriving on another interface are rejected.

API resources are `/api/v2/capabilities`, `/status`, `/config`,
`/config/validate`, `/wifi/scan`, `/clients`, `/can`, `/diagnostics`, `/reboot`,
`/reset` and `/ota`. Secret configuration values and Mesh fleet keys are never
returned.
