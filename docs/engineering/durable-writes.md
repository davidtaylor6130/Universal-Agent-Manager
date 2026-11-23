# Durable writes

Saved records (chats, settings, folders, memory entries, instruction files and their `.bak` companions) are written through `uam::io` in [`io_utils.h`](../../src/common/utils/io_utils.h): a complete private temporary sibling is written and synced (`F_FULLFSYNC` on macOS), then published, then the directory is synced before the write reports success. A failed save leaves the previous file in place; success is reported only after the commit. The [data authority agreement](../architecture/source-of-truth.md) owns which file is authoritative.

## Replacing an existing file

The first write of a file is a plain rename. Replacing an existing file uses an atomic exchange (`renameatx_np(RENAME_SWAP)` on macOS, `renameat2(RENAME_EXCHANGE)` on Linux, `ReplaceFileW` on Windows), which leaves the previous file at the temporary name for the `.bak` companion or the conditional publication evidence path.

Some volumes do not implement the exchange: SMB shares (`smbfs` reports `VOL_CAP_INT_RENAME_SWAP` unsupported), exFAT, NFS and some FUSE filesystems. When the exchange fails with `ENOTSUP`, `EOPNOTSUPP`, `EINVAL`, `ENOSYS` or `ENOTTY`, the volume (by `st_dev`) is remembered for the rest of the process and that replacement and later ones on it use a fallback:

1. Keep the previous file first, at a fresh `.tmp.` sibling (or the reserved private evidence path for conditional chat publication): a hard link where the volume supports one, otherwise a synced copy that keeps mode and times where the volume allows.
2. Sync the directory, then `rename(temporary, destination)`.
3. Move the kept file to the temporary name, so callers find it exactly where an exchange leaves it; the caller then syncs the directory as usual.

Any other exchange error still fails the save without a fallback. If the installing rename fails, the kept copy is removed while the destination still exists; if a non-atomic client removed the destination name first, the kept file is renamed back, or kept as the `.bak` companion when that also fails.

**Weaker guarantee.** The fallback never exposes a partial destination, but it is only as atomic as the volume's own rename-over-existing: an SMB client or server may remove the old name before installing the new one. A crash between the two directory syncs can leave the kept previous file under its `.tmp.` name, which the abandoned-temporary sweep removes after 24 hours, and on a hard-link volume briefly leaves the destination with a second link. A copied predecessor is a new file identity. The data folder itself is refused on network volumes; see [Storage](../settings/storage.md#application-data-folder).

**Implementation: Partial.** Fallback, capability cache and errno-injected cases (`tests/durable_replace_fallback_tests.cpp`) are in source; the Linux runner build compiles the shared POSIX path. The macOS branch is checked by reading and macOS CI only; a save on a real SMB share and an exFAT volume on a Mac remains unverified.
