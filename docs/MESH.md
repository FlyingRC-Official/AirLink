# AirLink V0.4 Mesh

V0.4.0-DEV supports one fixed `ground_root` and up to eight approved `air`
nodes on 2.4 GHz. The configured maximum of three wireless links maps to
ESP-MESH root layer 1 through maximum layer 4. Root election is fixed: loss of
the ground root makes the airborne network rootless until that same network
returns.

Mesh mode and the V0.3.3 `bridge_role` are mutually exclusive. An air node uses
UART MAVLink and USB `LOG_CLI`; a ground root disables its flight-controller
UART/DroneCAN tunnel and normally exposes USB MAVLink. Recovery and
factory-test firmware always retain their V0.3.3 Wi-Fi/Web behavior.

## Provisioning

Create a network on the ground root over local USB, explicitly export the
`airlink-mesh-provision/v1` JSON, and import it locally on each air node. The
file contains the fleet key and must be protected like a password. Status,
diagnostic and remote configuration responses never contain that key. V0.4
rejects any band other than `2g`; the default country is `CN` and its supported
channels are 1–13.

Possessing the fleet key is not approval. The root must approve the serial and
STA MAC pair before UART data can enter the shared MAVLink bus. A changed
serial/MAC pair, removal, or a duplicate online MAVLink system ID isolates the
node. Duplicate-ID isolation persists until the operator approves the node
again.

## Local USB management

The ground root recognizes `+++AIRLINK-CLI\r\n` in its USB MAVLink stream,
flushes the frame boundary and switches to protocol-v1 COBS frames delimited by
zero. Control requests use UTF-8 JSON and OTA chunks use binary payloads. The
standalone configurator performs this automatically. A close control frame,
disconnect, 60 seconds of inactivity, or reboot restores USB MAVLink.

Human-readable rescue commands remain available in `LOG_CLI`, recovery and
factory-test images: `mesh show`, `mesh create`, `mesh import`, `mesh role`, and
`mesh reset`.

## Safety and recovery

Network changes and fleet OTA require every approved node to be online, have a
fresh known armed state, be disarmed and have no isolation reason. Network
changes are prepared on every node before a commit is sent; air nodes activate
and restart before the ground root activates and restarts.

If a key/channel/country update prevents a node from joining, connect USB to
that node and use `mesh import` with the current provisioning package or
`mesh reset`. There is no automatic key rollback and no airborne root election.

Mesh SoftAP interfaces accept Mesh peers and are not promised as ordinary PC
Wi-Fi access points. Use USB for management.
