// Windows CEF bootstrap.exe layout: subprocess detection, helper working directories and the LPAC folder grant.
#include "test_harness.h"

#include "common/platform/cef_sandbox_folder_access.h"

#if defined(_WIN32)
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <fstream>

using namespace uam_test;

namespace
{
	/// Owns a security descriptor parsed from SDDL and exposes its DACL.
	struct SddlDacl
	{
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		PACL dacl = nullptr;

		explicit SddlDacl(const wchar_t* sddl)
		{
			UAM_ASSERT(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr));
			BOOL present = FALSE;
			BOOL defaulted = FALSE;
			UAM_ASSERT(GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted));
		}

		~SddlDacl()
		{
			LocalFree(descriptor);
		}
	};

	uam::platform::CefSandboxFolderGrant GrantFor(const wchar_t* sddl)
	{
		SddlDacl parsed(sddl);
		return uam::platform::EvaluateCefSandboxFolderGrant(parsed.dacl);
	}
}

UAM_TEST(WindowsCefSubprocessDetectionReadsOnlyUnquotedTypeSwitchesBeforeTheTerminator)
{
	using uam::platform::IsCefSubprocessCommandLine;
	UAM_ASSERT(IsCefSubprocessCommandLine(LR"("C:\Program Files\UAM\universal_agent_manager.exe" --type=renderer --lang=en-GB)"));
	UAM_ASSERT(IsCefSubprocessCommandLine(L"universal_agent_manager.exe\t--type=gpu-process"));
	UAM_ASSERT(!IsCefSubprocessCommandLine(LR"("C:\Program Files\UAM\universal_agent_manager.exe")"));
	UAM_ASSERT(!IsCefSubprocessCommandLine(L"universal_agent_manager.exe --management-node"));
	// A user value that merely contains --type= is not a switch.
	UAM_ASSERT(!IsCefSubprocessCommandLine(LR"(universal_agent_manager.exe "--type=renderer")"));
	UAM_ASSERT(!IsCefSubprocessCommandLine(LR"(universal_agent_manager.exe "C:\a dir --type=x\file")"));
	UAM_ASSERT(!IsCefSubprocessCommandLine(L"universal_agent_manager.exe --name=--type=renderer"));
	// Arguments after "--" belong to the wrapped command (OpenCode V2 bridge and worker).
	UAM_ASSERT(!IsCefSubprocessCommandLine(L"universal_agent_manager.exe --uam-opencode-v2-bridge -- opencode --type=renderer"));
	UAM_ASSERT(!IsCefSubprocessCommandLine(L"universal_agent_manager.exe --"));
	// "--x" is not the terminator.
	UAM_ASSERT(IsCefSubprocessCommandLine(L"universal_agent_manager.exe --x --type=utility"));
	UAM_ASSERT(!IsCefSubprocessCommandLine(L""));
}

UAM_TEST(WindowsLaunchWorkingDirectoryIsNamedOnlyForUamItself)
{
	using uam::platform::AddLaunchWorkingDirectory;
	const fs::path self = LR"(C:\Apps\UAM\universal_agent_manager.exe)";
	std::vector<std::pair<std::string, std::string>> environment{{"KEEP", "1"}};
	AddLaunchWorkingDirectory(environment, R"(c:\apps\uam\UNIVERSAL_AGENT_MANAGER.EXE)", LR"(D:\work\project)", self);
	UAM_ASSERT_EQ(environment.size(), std::size_t{2});
	UAM_ASSERT_EQ(environment.back().first, std::string(uam::platform::kLaunchWorkingDirectoryVariable));
	UAM_ASSERT_EQ(environment.back().second, std::string(R"(D:\work\project)"));

	std::vector<std::pair<std::string, std::string>> other;
	AddLaunchWorkingDirectory(other, R"(C:\Tools\opencode.exe)", LR"(D:\work\project)", self);
	UAM_ASSERT(other.empty());
	AddLaunchWorkingDirectory(other, R"(C:\Apps\UAM\universal_agent_manager.exe)", fs::path(), self);
	UAM_ASSERT(other.empty());
	AddLaunchWorkingDirectory(other, R"(C:\Apps\UAM\universal_agent_manager.exe)", LR"(D:\work)", fs::path());
	UAM_ASSERT(other.empty());
}

UAM_TEST(WindowsCefSandboxFolderGrantCountsSplitAndGenericEntries)
{
	UAM_ASSERT(GrantFor(L"D:(A;OICI;0x1200a9;;;S-1-15-2-2)").Complete());
	// Program Files: one entry for the folder, one inherit-only entry with generic rights for its contents.
	UAM_ASSERT(GrantFor(L"D:(A;;0x1200a9;;;S-1-15-2-2)(A;OICIIO;GXGR;;;S-1-15-2-2)").Complete());
	const uam::platform::CefSandboxFolderGrant files_only = GrantFor(L"D:(A;OI;0x1200a9;;;S-1-15-2-2)");
	UAM_ASSERT(files_only.folder && files_only.files && !files_only.subfolders);
	const uam::platform::CefSandboxFolderGrant inherit_only = GrantFor(L"D:(A;OICIIO;GXGR;;;S-1-15-2-2)");
	UAM_ASSERT(!inherit_only.folder && inherit_only.files && inherit_only.subfolders);
	// Other identities, ALL APPLICATION PACKAGES and read without execute do not count.
	UAM_ASSERT(!GrantFor(L"D:(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;S-1-15-2-1)").Complete());
	UAM_ASSERT(!GrantFor(L"D:(A;OICI;FR;;;S-1-15-2-2)").Complete());
	UAM_ASSERT(uam::platform::EvaluateCefSandboxFolderGrant(nullptr).Complete());
}

UAM_TEST(WindowsCefSandboxFolderGrantIsAddedOnlyToUamsOwnFolder)
{
	TempDir temp("uam-cef-sandbox-folder");
	std::string error;
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	UAM_ASSERT_EQ(GetNamedSecurityInfoW(temp.root.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor), static_cast<DWORD>(ERROR_SUCCESS));
	const bool granted_before = uam::platform::EvaluateCefSandboxFolderGrant(dacl).Complete();
	LocalFree(descriptor);
	if (!granted_before)
	{
		// Without the app's binaries the folder is left unchanged and the log names why.
		UAM_ASSERT(!uam::platform::EnsureCefSandboxFolderAccess(temp.root, error));
		UAM_ASSERT(error.find("only to its own folder") != std::string::npos);
		descriptor = nullptr;
		UAM_ASSERT_EQ(GetNamedSecurityInfoW(temp.root.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor), static_cast<DWORD>(ERROR_SUCCESS));
		const bool granted_after_refusal = uam::platform::EvaluateCefSandboxFolderGrant(dacl).Complete();
		LocalFree(descriptor);
		UAM_ASSERT(!granted_after_refusal);
	}

	for (const char* name : {"libcef.dll", "universal_agent_manager.dll"}) std::ofstream(temp.root / name) << "stand-in";
	fs::create_directories(temp.root / "locales");
	error.clear();
	UAM_ASSERT(uam::platform::EnsureCefSandboxFolderAccess(temp.root, error));
	UAM_ASSERT(error.empty());
	for (const fs::path& path : {temp.root, temp.root / "libcef.dll", temp.root / "locales"})
	{
		dacl = nullptr;
		descriptor = nullptr;
		UAM_ASSERT_EQ(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor), static_cast<DWORD>(ERROR_SUCCESS));
		const uam::platform::CefSandboxFolderGrant grant = uam::platform::EvaluateCefSandboxFolderGrant(dacl);
		LocalFree(descriptor);
		UAM_ASSERT(grant.folder); // inherited by the existing file and subfolder too
	}
	// A second start finds the grant and changes nothing.
	UAM_ASSERT(uam::platform::EnsureCefSandboxFolderAccess(temp.root, error));
}
#endif
