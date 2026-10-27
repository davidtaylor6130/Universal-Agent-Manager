"""Regenerate synthetic native envelopes with Python cryptography, independently of Mbed TLS.

Requires the development-only cryptography package. Reads the public fixed format
key from source; never reads provider stores, credentials or existing histories.
"""
from pathlib import Path
import re

from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def varint(value):
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def number(field, value):
    return varint(field * 8) + varint(value)


def data(field, value):
    if isinstance(value, str):
        value = value.encode()
    return varint(field * 8 + 2) + varint(len(value)) + value


def envelope(uri, key, equal_identity=False, render=None):
    session = "3dc412af-b91a-4d9d-82f0-6cd3f991ce2c"
    trajectory = "fe61a3f1-c177-49aa-ab0b-e266e7ec5be6"
    if equal_identity:
        trajectory = session
    stamp = number(1, 1792756800) + number(2, 123456789)
    metadata = (data(1, data(1, uri) + data(2, uri)) + data(2, stamp)
                + data(6, session) + data(7, uri))
    steps = []
    call = data(1, "legacy-render-call") + data(2, "synthetic_tool") + data(3, "{}")
    records = [
        (0, 14, 4, 19, data(2, "Legacy synthetic request")),
        (1, 15, 2, 20, data(1, "Legacy synthetic response")
         + data(3, "Legacy native reasoning") + (data(7, call) if render is not None else b"")),
    ]
    if render is not None:
        records.append((2, 132, 2, 140, data(2, data(1, "Legacy generic output") + data(3, render))))
    for index, kind, source, field, body in records:
        step_metadata = (data(1, stamp) + number(3, source) + (data(4, call) if kind == 132 else b"")
                         + data(20, data(1, trajectory) + number(2, index) + data(4, session)))
        steps.append(number(1, kind) + number(4, 3) + data(5, step_metadata) + data(field, body))
    native = (data(1, trajectory) + data(6, session) + number(4, 4) + number(8, 17)
              + data(7, metadata) + b"".join(data(2, step) for step in steps))
    nonce = bytes(range(12))  # Deterministic fixture only, never production encryption.
    return nonce + AESGCM(key).encrypt(nonce, native, None)


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/common/provider/antigravity/cli/antigravity_legacy_format_key.h").read_text()
    array = source.split("kLegacyFormatKey{", 1)[1].split("}", 1)[0]
    key = bytes(int(value, 16) for value in re.findall(r"0x[0-9a-f]{2}", array))
    assert len(key) == 32
    lines = ["#pragma once", "#include <array>", "",
             "// Independently generated synthetic fixtures: Python cryptography AESGCM.",
             "// Fixed nonce is fixture-only. Plaintext contains the documented native IDs,",
             "// workspace, request/response/reasoning literals below; no real history.",
             "namespace uam_test::legacy_golden", "{"]
    for name, uri in [("posix", "file:///synthetic/native%20workspace"),
                      ("windows", "file:///C:/synthetic/native%20workspace"),
                      ("equal_posix", "file:///synthetic/native%20workspace"),
                      ("equal_windows", "file:///C:/synthetic/native%20workspace"),
                      ("render_posix", "file:///synthetic/native%20workspace"),
                      ("render_windows", "file:///C:/synthetic/native%20workspace"),
                      ("empty_render_posix", "file:///synthetic/native%20workspace"),
                      ("empty_render_windows", "file:///C:/synthetic/native%20workspace")]:
        render = b"" if name.startswith("empty_render_") else data(1, "Native title") + data(2, "Native markdown") if name.startswith("render_") else None
        blob = envelope(uri, key, name.startswith("equal_"), render)
        values = ", ".join(f"0x{value:02x}" for value in blob)
        lines.append(f"\tinline constexpr std::array<unsigned char, {len(blob)}> {name}{{{values}}};")
    lines.append("}")
    Path(__file__).with_name("legacy_golden.h").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
