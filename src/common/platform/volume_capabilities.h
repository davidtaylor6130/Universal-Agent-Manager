#pragma once

#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/vfs.h>
#endif

namespace uam::platform
{
	/// <summary>What the volume holding a folder reports about itself; unknown fields stay empty.</summary>
	struct VolumeCapabilities
	{
		/// False for a network volume (SMB, NFS, AFP, WebDAV and similar); empty when the platform could not tell.
		std::optional<bool> local;
		/// macOS only: whether the volume implements renameatx_np(RENAME_SWAP), which durable saves prefer.
		std::optional<bool> rename_swap;
		/// Filesystem type name where the platform reports one (for example "apfs", "smbfs", "exfat").
		std::string filesystem;
	};

	/// Test-only answer for QueryVolumeCapabilities on the calling thread.
	inline thread_local std::optional<VolumeCapabilities> t_volume_capabilities_for_tests;

	/// <summary>Asks the volume holding an existing path whether it is local and supports exchange renames.</summary>
	/// <remarks>Read-only and lock-free. A failed query returns all fields empty; callers treat that as unknown, not remote.</remarks>
	inline VolumeCapabilities QueryVolumeCapabilities(const std::filesystem::path& path)
	{
		if (t_volume_capabilities_for_tests) return *t_volume_capabilities_for_tests;
		VolumeCapabilities result;
#if defined(__APPLE__)
		struct statfs volume{};
		if (statfs(path.c_str(), &volume) != 0) return result;
		result.local = (volume.f_flags & MNT_LOCAL) != 0;
		result.filesystem = volume.f_fstypename;
#if defined(VOL_CAP_INT_RENAME_SWAP)
		// Volume attributes must be read from the volume's root directory.
		struct attrlist request{};
		request.bitmapcount = ATTR_BIT_MAP_COUNT;
		request.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
		struct
		{
			u_int32_t length;
			vol_capabilities_attr_t capabilities;
		} __attribute__((aligned(4), packed)) reply{};
		if (getattrlist(volume.f_mntonname, &request, &reply, sizeof(reply), 0) == 0 &&
		    (reply.capabilities.valid[VOL_CAPABILITIES_INTERFACES] & VOL_CAP_INT_RENAME_SWAP) != 0)
			result.rename_swap = (reply.capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] & VOL_CAP_INT_RENAME_SWAP) != 0;
#endif
#elif defined(__linux__)
		struct statfs volume{};
		if (statfs(path.c_str(), &volume) != 0) return result;
		switch (static_cast<unsigned long>(volume.f_type))
		{
		case 0x6969UL:     // NFS
		case 0x517BUL:     // SMB
		case 0xFF534D42UL: // CIFS
		case 0xFE534D42UL: // SMB2
		case 0x5346414FUL: // AFS
		case 0x564CUL:     // NCP
		case 0x01021997UL: // 9P
		case 0x00C36400UL: // Ceph
			result.local = false;
			break;
		default:
			result.local = true;
			break;
		}
#elif defined(_WIN32)
		std::wstring volume(32768, L'\0');
		if (!GetVolumePathNameW(path.c_str(), volume.data(), static_cast<DWORD>(volume.size()))) return result;
		volume.resize(wcslen(volume.c_str()));
		const UINT type = GetDriveTypeW(volume.c_str());
		if (type != DRIVE_UNKNOWN && type != DRIVE_NO_ROOT_DIR) result.local = type != DRIVE_REMOTE;
#endif
		return result;
	}
} // namespace uam::platform
