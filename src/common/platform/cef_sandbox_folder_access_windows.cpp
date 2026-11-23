#include "common/platform/cef_sandbox_folder_access.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

namespace uam::platform
{
	namespace
	{
		// CEF's SET_LPAC_ACLS grants *S-1-15-2-2:(OI)(CI)(RX) on its output folder.
		constexpr DWORD kReadExecute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
		constexpr BYTE kInheritedByContents = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;

		bool GrantsReadExecuteToContents(PACL dacl, PSID sid)
		{
			if (dacl == nullptr) return true; // A NULL DACL grants everyone full access.
			ACL_SIZE_INFORMATION information{};
			if (!GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation)) return false;
			for (DWORD index = 0; index < information.AceCount; ++index)
			{
				void* ace = nullptr;
				if (!GetAce(dacl, index, &ace)) continue;
				const auto* header = static_cast<const ACE_HEADER*>(ace);
				if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0) continue;
				if ((header->AceFlags & kInheritedByContents) != kInheritedByContents) continue;
				auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
				if ((allowed->Mask & kReadExecute) == kReadExecute && EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), sid)) return true;
			}
			return false;
		}
	}

	bool EnsureCefSandboxFolderAccess(const std::filesystem::path& folder, std::string& error)
	{
		PSID sid = nullptr;
		if (!ConvertStringSidToSidW(L"S-1-15-2-2", &sid))
		{
			error = "Could not resolve the restricted application package identity for the Chromium sandbox (error " + std::to_string(GetLastError()) + ").";
			return false;
		}
		PACL dacl = nullptr;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		const std::wstring path = folder.wstring();
		DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
		bool granted = status == ERROR_SUCCESS && GrantsReadExecuteToContents(dacl, sid);
		if (status == ERROR_SUCCESS && !granted)
		{
			EXPLICIT_ACCESS_W access{};
			access.grfAccessPermissions = kReadExecute;
			access.grfAccessMode = GRANT_ACCESS;
			access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
			access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
			access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
			access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
			PACL updated = nullptr;
			status = SetEntriesInAclW(1, &access, dacl, &updated);
			if (status == ERROR_SUCCESS)
			{
				// Applies the inheritable entry to the folder's existing files and subfolders as well.
				status = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
				granted = status == ERROR_SUCCESS;
			}
			if (updated != nullptr) LocalFree(updated);
		}
		if (descriptor != nullptr) LocalFree(descriptor);
		LocalFree(sid);
		if (!granted)
		{
			// The path is private data; the status code is enough to diagnose the folder permission.
			error = "The app folder does not grant the Chromium sandbox read access and UAM could not add it (error " + std::to_string(status) +
			        "). Sandboxed GPU and network processes may fail to start; run once from a folder you own or grant *S-1-15-2-2:(OI)(CI)(RX).";
			return false;
		}
		return true;
	}
}
