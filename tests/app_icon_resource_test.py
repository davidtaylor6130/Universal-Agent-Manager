"""Checks that assets/app_icon.ico was generated from the current app icon tile by scripts/make_windows_icon.py and,
when a Windows executable is given, that its embedded IDI_APP_ICON holds exactly those icon images.
Usage: python3 tests/app_icon_resource_test.py [universal_agent_manager.exe] [--extract-png out.png]"""
import hashlib, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
REGENERATE = 'run python3 scripts/make_windows_icon.py'


def ico_entries(data):
    _, kind, count = struct.unpack_from('<HHH', data)
    assert kind == 1, 'app_icon.ico is not an icon file'
    entries = {}
    for index in range(count):
        width, _, _, _, _, _, length, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * index)
        entries[width or 256] = data[offset:offset + length]
    assert len(entries) == count, f'app_icon.ico repeats a size; {REGENERATE}'
    return entries


def png_source_digest(png, size):
    assert png[:8] == b'\x89PNG\r\n\x1a\n', f'{size}px entry is not a PNG; {REGENERATE}'
    assert struct.unpack_from('>II', png, 16) == (size, size), f'{size}px entry has the wrong dimensions'
    position = 8
    while position < len(png):
        length, kind = struct.unpack_from('>I4s', png, position)
        body = png[position + 8:position + 8 + length]
        if kind == b'tEXt' and body.startswith(b'uam-source-sha256\0'):
            return body.split(b'\0', 1)[1].decode()
        position += 12 + length
    return None


def exe_icon_entries(exe):
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = ctypes.c_void_p
    kernel.LoadResource.argtypes = [wintypes.HMODULE, ctypes.c_void_p]
    kernel.LoadResource.restype = ctypes.c_void_p
    kernel.LockResource.argtypes = [ctypes.c_void_p]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = [wintypes.HMODULE, ctypes.c_void_p]
    module = kernel.LoadLibraryExW(str(exe), None, 0x22)  # LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE
    assert module, f'cannot load {exe} as a resource file'

    def resource(name, kind):
        found = kernel.FindResourceW(module, name, kind)
        assert found, f'{exe} has no resource {name!r} of type {kind}'
        return ctypes.string_at(kernel.LockResource(kernel.LoadResource(module, found)), kernel.SizeofResource(module, found))

    group = resource(ctypes.c_wchar_p('IDI_APP_ICON'), 14)  # RT_GROUP_ICON
    entries = {}
    for index in range(struct.unpack_from('<H', group, 4)[0]):
        width, _, _, _, _, _, _, icon_id = struct.unpack_from('<BBBBHHIH', group, 6 + 14 * index)
        entries[width or 256] = resource(icon_id, 3)  # RT_ICON
    return entries


def main(args):
    extract = args[args.index('--extract-png') + 1] if '--extract-png' in args else None
    exe = next((arg for arg in args if arg.endswith('.exe')), None)
    entries = ico_entries((ROOT / 'assets/app_icon.ico').read_bytes())
    assert sorted(entries) == SIZES, f'app_icon.ico sizes {sorted(entries)} != {SIZES}; {REGENERATE}'
    digest = hashlib.sha256((ROOT / 'assets/app_icon_macos.png').read_bytes()).hexdigest()
    for size, png in entries.items():
        assert png_source_digest(png, size) == digest, f'{size}px entry is not from the current app_icon_macos.png; {REGENERATE}'
    if exe:
        embedded = exe_icon_entries(exe)
        assert embedded == entries, f'{exe} embeds a different icon than assets/app_icon.ico; rebuild src/uam.rc'
        if extract:
            Path(extract).write_bytes(embedded[256])
    print(f'app icon current: {len(entries)} sizes from app_icon_macos.png {digest[:12]}' + (f', embedded in {exe}' if exe else ''))


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except AssertionError as error:
        sys.exit(f'FAIL: {error}')
