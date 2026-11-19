#include "common/secrets/secret_drop.h"

#include "common/secrets/secret_drop_internal.h"
#include "common/secrets/secret_redactor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

// Windows cannot observe a read, so the owner-only file is removed after its TTL (approved: 60 s).
namespace uam::secrets
{
	namespace
	{
		using namespace drop_internal;

		struct LocalMemory
		{
			void* pointer = nullptr;
			~LocalMemory() { if (pointer != nullptr) LocalFree(pointer); }
		};

		// Protected DACL granting only the current user; "OICI" lets the folder pass it to new files.
		bool OwnerOnlyDescriptor(bool directory, LocalMemory& descriptor)
		{
			HANDLE token = nullptr;
			if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
			DWORD size = 0;
			(void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
			std::string buffer(size, '\0');
			const bool have_user = size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
			CloseHandle(token);
			if (!have_user) return false;
			LocalMemory sid_text;
			if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, reinterpret_cast<LPWSTR*>(&sid_text.pointer))) return false;
			const std::wstring sddl = std::wstring(directory ? L"D:P(A;OICI;FA;;;" : L"D:P(A;;FA;;;") + static_cast<const wchar_t*>(sid_text.pointer) + L")";
			return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor.pointer, nullptr) != FALSE;
		}

		void RemoveAfter(const std::filesystem::path& path, std::chrono::steady_clock::time_point deadline, std::stop_token stop)
		{
			while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			// A reader can hold the file briefly; retry for up to ten seconds.
			for (int attempt = 0; attempt < 200; ++attempt)
			{
				if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	}

	DropResult Drop(const std::filesystem::path& directory, std::string_view value, std::chrono::seconds ttl)
	{
		DropResult result;
		if (value.empty() || value.size() > kMaxSecretBytes)
		{
			result.error = "The secret is empty or larger than 8 KB.";
			return result;
		}
		LocalMemory folder_descriptor;
		LocalMemory file_descriptor;
		if (!OwnerOnlyDescriptor(true, folder_descriptor) || !OwnerOnlyDescriptor(false, file_descriptor))
		{
			result.error = "The private secret permissions could not be prepared.";
			return result;
		}
		SECURITY_ATTRIBUTES folder_attributes{sizeof(SECURITY_ATTRIBUTES), folder_descriptor.pointer, FALSE};
		if (!CreateDirectoryW(directory.c_str(), &folder_attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			result.error = "The private secret folder could not be created.";
			return result;
		}
		const DWORD attributes = GetFileAttributesW(directory.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			result.error = "The private secret folder must be a real folder.";
			return result;
		}
		const std::filesystem::path path = directory / RandomDropName();
		SECURITY_ATTRIBUTES file_attributes{sizeof(SECURITY_ATTRIBUTES), file_descriptor.pointer, FALSE};
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &file_attributes, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			result.error = "The private secret handoff could not be created.";
			return result;
		}
		DWORD written = 0;
		const bool complete = WriteFile(file, value.data(), static_cast<DWORD>(value.size()), &written, nullptr) && written == value.size();
		CloseHandle(file);
		if (!complete)
		{
			DeleteFileW(path.c_str());
			result.error = "The private secret handoff could not be written.";
			return result;
		}
		try
		{
			StartWriter([path, deadline = std::chrono::steady_clock::now() + ttl](std::stop_token stop) { RemoveAfter(path, deadline, stop); });
		}
		catch (const std::system_error&)
		{
			DeleteFileW(path.c_str());
			result.error = "The private secret handoff could not start.";
			return result;
		}
		result.ok = true;
		result.path = uam::paths::Utf8PathString(path);
		return result;
	}

	std::size_t SweepStale(const std::filesystem::path& directory)
	{
		return SweepDropDirectory(directory);
	}

	void StopAllDrops()
	{
		StopAllWriters();
	}
	LocalDropKind LocalDrop()
	{
		return {kWindowsDropTtl, false, "windows"};
	}
}
