# AirLink V0.4.0-DEV

This development release adds the 2.4 GHz fixed-root Mesh mode described in
[MESH.md](MESH.md), based on ESP-IDF 6.0.2. It retains all V0.3.3 gateway,
air/ground bridge, UART/DroneCAN, Web API, USB CLI and local OTA modes when the
Mesh role is off.

The Mesh security boundary is a shared fleet key plus root-managed operational
approval. It does not provide per-device cryptographic isolation. 5 GHz,
multiple ground stations, root failover/election, transparent serial Mesh,
system-ID rewriting, in-flight OTA and non-Mesh clients on the Mesh SoftAP are
outside this release.

This is a development prerelease. A successful software build is not RF or
flight acceptance; release qualification requires the bench and incremental
2/4/8-aircraft tests in `docs/ACCEPTANCE.md`.
