"""Builds assets/app_icon.ico from the approved app icon tile (assets/app_icon_macos.png, written by
scripts/compose_macos_icon.swift). Every entry is a PNG that records the tile's SHA-256 in a tEXt chunk, so
tests/app_icon_resource_test.py can prove the icon was generated from the current artwork.
Usage: python3 scripts/make_windows_icon.py"""
import hashlib, io, struct, zlib
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SOURCE, OUTPUT = ROOT / 'assets/app_icon_macos.png', ROOT / 'assets/app_icon.ico'
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
SOURCE_KEY = b'uam-source-sha256'


def tagged_png(image, digest):
    buffer = io.BytesIO()
    image.save(buffer, 'PNG', optimize=True)
    png = buffer.getvalue()
    body = SOURCE_KEY + b'\0' + digest.encode()
    chunk = struct.pack('>I', len(body)) + b'tEXt' + body + struct.pack('>I', zlib.crc32(b'tEXt' + body))
    return png[:33] + chunk + png[33:]  # after the signature and IHDR


if __name__ == '__main__':
    digest = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    tile = Image.open(SOURCE).convert('RGBA')
    entries = [tagged_png(tile.resize((size, size), Image.LANCZOS), digest) for size in SIZES]
    out = struct.pack('<HHH', 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for size, png in zip(SIZES, entries):
        out += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
    OUTPUT.write_bytes(out + b''.join(entries))
    print(f'Wrote {OUTPUT.relative_to(ROOT)} with {len(SIZES)} sizes from {SOURCE.relative_to(ROOT)}.')
