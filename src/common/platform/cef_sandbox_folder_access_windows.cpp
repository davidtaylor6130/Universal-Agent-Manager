#include "common/platform/cef_sandbox_folder_access.h"

#include <windows.h>
#include <aclapi.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>

#include <cwchar>
#include <string>

namespace uam::platform
{
	namespace
	{
		// CEF's SET_LPAC_ACLS grants *S-1-15-2-2:(OI)(CI)(RX) on its output folder.
		constexpr DWORD kReadExecute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;

		bool SameFolder(const std::filesystem::path& left, const std::filesystem::path& right)
		{
			return !left.empty() && !right.empty() && _wcsicmp(left.lexically_normal().c_str(), right.lexically_normal().c_str()) == 0;
		}

		/// A shared user folder holds far more than UAM; granting it would widen access and rewrite a large tree.
		bool IsSharedUserFolder(const std::filesystem::path& folder)
		{
			for (const KNOWNFOLDERID& id : {FOLDERID_Profile, FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads})
			{
				PWSTR known = nullptr;
				const bool same = SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &known)) && known != nullptr && SameFolder(folder, std::filesystem::path(known));
				CoTaskMemFree(known);
				if (same) return true;
			}
			return false;
		}
	}

	CefSandboxFolderGrant EvaluateCefSandboxFolderGrant(const void* dacl_value)
	{
		CefSandboxFolderGrant grant;
		PACL dacl = static_cast<PACL>(const_cast<void*>(dacl_value));
		if (dacl == nullptr) return {true, true, true}; // A NULL DACL grants everyone full access.
		PSID sid = nullptr;
		if (!ConvertStringSidToSidW(L"S-1-15-2-2", &sid)) return grant;
		ACL_SIZE_INFORMATION information{};
		if (GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation))
		{
			for (DWORD index = 0; index < information.AceCount; ++index)
			{
				void* ace = nullptr;
				if (!GetAce(dacl, index, &ace)) continue;
				const auto* header = static_cast<const ACE_HEADER*>(ace);
				if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
				auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
				// Inherit-only entries often hold generic rights (Program Files uses GR GX); compare file rights.
				ACCESS_MASK mask = allowed->Mask;
				GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
				MapGenericMask(&mask, &mapping);
				if ((mask & kReadExecute) != kReadExecute || !EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), sid)) continue;
				if ((header->AceFlags & INHERIT_ONLY_ACE) == 0) grant.folder = true;
				if ((header->AceFlags & OBJECT_INHERIT_ACE) != 0) grant.files = true;
				if ((header->AceFlags & CONTAINER_INHERIT_ACE) != 0) grant.subfolders = true;
			}
		}
		LocalFree(sid);
		return grant;
	}

	bool EnsureCefSandboxFolderAccess(const std::filesystem::path& folder, std::string& error)
	{
		PACL dacl = nullptr;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		const std::wstring path = folder.wstring();
		DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
		if (status == ERROR_SUCCESS && EvaluateCefSandboxFolderGrant(dacl).Complete())
		{
			LocalFree(descriptor);
			return true;
		}
		std::error_code ignored;
		const bool app_folder = std::filesystem::is_regular_file(folder / "libcef.dll", ignored) && std::filesystem::is_regular_file(folder / "universal_agent_manager.dll", ignored) &&
		                        !IsSharedUserFolder(folder);
		bool granted = false;
		PSID sid = nullptr;
		if (status == ERROR_SUCCESS && app_folder && ConvertStringSidToSidW(L"S-1-15-2-2", &sid))
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
			LocalFree(sid);
		}
		if (descriptor != nullptr) LocalFree(descriptor);
		if (granted) return true;
		// The path is private data; the status code is enough to diagnose the folder permission.
		error = app_folder
		    ? "The app folder does not grant the Chromium sandbox read access and UAM could not add it (error " + std::to_string(status) +
		          "). Sandboxed GPU and network processes may fail to start; extract UAM into its own folder or grant *S-1-15-2-2:(OI)(CI)(RX)."
		    : "The app folder does not grant the Chromium sandbox read access, and UAM grants it only to its own folder holding libcef.dll and "
		      "universal_agent_manager.dll, outside the profile, Desktop, Documents and Downloads folders. Extract UAM into its own folder.";
		return false;
	}
}
