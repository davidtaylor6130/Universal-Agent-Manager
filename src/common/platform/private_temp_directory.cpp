#include "common/platform/private_temp_directory.h"
#include "common/paths/path_utils.h"
#include <random>
#include <vector>
#if defined(_WIN32)
#include <sddl.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace uam::platform
{
	std::optional<std::filesystem::path> CreatePrivateHistoryTempDirectory()
	{
		const auto parent = uam::paths::TempDirectoryPathNoThrow();
		if (!parent)
			return {};
#if defined(_WIN32)
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
			return {};
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<unsigned char> buffer(size);
		const bool identity_ok = size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
		CloseHandle(token);
		if (!identity_ok)
			return {};
		LPWSTR sid = nullptr;
		if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
			return {};
		const std::wstring dacl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + std::wstring(sid) + L")";
		LocalFree(sid);
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
			return {};
		struct SecurityCleanup
		{
			PSECURITY_DESCRIPTOR value;
			~SecurityCleanup()
			{
				LocalFree(value);
			}
		} cleanup{descriptor};
		SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
#endif
		std::random_device random;
		for (unsigned attempt = 0; attempt < 8; ++attempt)
		{
			const auto candidate = *parent / ("uam-antigravity-history-" + std::to_string(random()) + '-' + std::to_string(random()));
#if defined(_WIN32)
			if (CreateDirectoryW(candidate.c_str(), &attributes))
				return candidate;
			if (GetLastError() != ERROR_ALREADY_EXISTS)
				return {};
#else
			if (mkdir(candidate.c_str(), 0700) == 0)
				return candidate;
			if (errno != EEXIST)
				return {};
#endif
		}
		return {};
	}
} // namespace uam::platform
