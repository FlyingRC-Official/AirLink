#!/usr/bin/env python3
"""Host vectors and source guards for the AirLink management API v2."""

from __future__ import annotations

import hashlib
import hmac
from pathlib import Path


def derive(password: bytes, session_id: bytes, client_nonce: bytes, server_nonce: bytes):
    proof = hmac.new(
        password,
        b"AIRLINK-AUTH-V2" + session_id + client_nonce + server_nonce,
        hashlib.sha256,
    ).digest()
    key = hmac.new(
        password,
        b"AIRLINK-SESSION-V2" + client_nonce + server_nonce,
        hashlib.sha256,
    ).digest()
    return proof, key


def sign(key: bytes, method: str, uri: str, counter: int, body: bytes) -> tuple[str, str]:
    body_hash = hashlib.sha256(body).hexdigest()
    canonical = f"{method}\n{uri}\n{counter}\n{body_hash}".encode()
    return body_hash, hmac.new(key, canonical, hashlib.sha256).hexdigest()


def main() -> None:
    password = b"AirLink-Test_2026!"
    session_id = bytes(range(16))
    client_nonce = bytes(range(32))
    server_nonce = bytes(reversed(range(32)))
    proof, key = derive(password, session_id, client_nonce, server_nonce)
    assert proof.hex() == "78e5e0ebfce77870a9c2d2873033b6d4de7e6304f01a9ead38de4df0c7fe70b9"
    assert key.hex() == "0ac04daac34e13774e5635ca514caa00ecd85034c432044216bf2080afb248d4"

    body = b'{"uart_baud":921600}'
    body_hash, signature = sign(key, "PUT", "/api/v2/config", 1, body)
    assert body_hash == hashlib.sha256(body).hexdigest()
    assert hmac.compare_digest(signature, sign(key, "PUT", "/api/v2/config", 1, body)[1])
    assert not hmac.compare_digest(signature, sign(key, "PUT", "/api/v2/config", 1, body + b" ")[1])
    assert not hmac.compare_digest(signature, sign(key, "PUT", "/api/v2/config", 2, body)[1])

    source = (Path(__file__).parents[1] / "components/airlink_api/airlink_api.c").read_text()
    for marker in [
        "AIRLINK_AP_ADDRESS", "API_SESSION_IDLE_TIMEOUT_US", "parsed_counter <= session->counter",
        "X-AirLink-Body-SHA256", "constant_time_equal", 'URI("/api/v2/session/challenge"',
        'URI("/api/v2/session/auth"', 'URI("/api/v2/config"', 'URI("/api/v2/diagnostics"',
        "ota_body_hash_mismatch", "management_ap_only",
    ]:
        assert marker in source, f"missing API-v2 guard: {marker}"
    for forbidden in ["Basic realm", '"Authorization"', "/api/v1/", "index_html_gz"]:
        assert forbidden not in source, f"legacy API/UI path remains: {forbidden}"


if __name__ == "__main__":
    main()
