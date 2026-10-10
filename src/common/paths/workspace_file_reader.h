#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace uam::workspace_files
{
	inline constexpr std::size_t kMaxPreviewBytes = 1024 * 1024;

	struct TextFile
	{
		bool ok = false;
		std::string path;
		std::string text;
		std::string error;
	};

	namespace detail
	{
#if defined(_WIN32)
		struct CloseFile
		{
			void operator()(void* handle) const { if (handle != nullptr) CloseHandle(handle); }
		};
		using OwnedFile = std::unique_ptr<void, CloseFile>;

		inline OwnedFile Open(const std::filesystem::path& path, bool directory)
		{
			std::filesystem::path native;
			std::string error;
			if (!io::NativeFilePath(path, native, error)) return {};
			HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ,
			    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			    FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), nullptr);
			return OwnedFile(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
		}

		inline std::wstring FinalPath(HANDLE handle)
		{
			const DWORD length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
			if (length == 0 || length > 65536) return {};
			std::wstring result(length, L'\0');
			const DWORD count = GetFinalPathNameByHandleW(handle, result.data(), length, FILE_NAME_NORMALIZED);
			if (count == 0 || count >= length) return {};
			result.resize(count);
			while (!result.empty() && result.back() == L'\\') result.pop_back();
			return result;
		}
#else
		struct CloseFile
		{
			void operator()(int* descriptor) const { if (descriptor != nullptr) { close(*descriptor); delete descriptor; } }
		};
		using OwnedFile = std::unique_ptr<int, CloseFile>;
		inline OwnedFile Own(int descriptor) { return descriptor < 0 ? OwnedFile{} : OwnedFile(new int(descriptor)); }
#endif
	}

	/// Reads a bounded snapshot through owned regular-file handles inside the canonical workspace.
	inline TextFile Read(const std::filesystem::path& workspace, std::string_view requested)
	{
		TextFile result;
		result.error = "The file is missing or cannot be read.";
		if (workspace.empty() || workspace.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos || !workspace.is_absolute() || requested.empty() || requested.size() > 16384 ||
		    requested.find('\0') != std::string_view::npos || requested.find_first_of("\r\n") != std::string_view::npos)
		{ result.error = "Choose a file in this chat's workspace."; return result; }
		std::error_code error;
		const std::filesystem::path root = std::filesystem::canonical(workspace, error);
		if (error || !std::filesystem::is_directory(root, error) || error)
		{ result.error = "The chat workspace is unavailable."; return result; }
#if !defined(_WIN32)
		struct stat root_before{};
		if (stat(root.c_str(), &root_before) != 0 || !S_ISDIR(root_before.st_mode)) return result;
#endif
		const std::filesystem::path input = paths::PathFromUtf8(requested);
		const std::filesystem::path file = std::filesystem::canonical(input.is_absolute() ? input : root / input, error);
		if (error) return result;
		const std::filesystem::path relative = file.lexically_relative(root);
#if !defined(_WIN32)
		if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." || relative == ".")
		{ result.error = "This file is outside the chat workspace."; return result; }
		result.path = paths::Utf8PathString(relative);
#endif
#if defined(_WIN32)
		detail::OwnedFile root_handle = detail::Open(root, true);
		detail::OwnedFile handle = detail::Open(file, false);
		if (!root_handle || !handle) return result;
		const std::wstring root_path = detail::FinalPath(root_handle.get());
		const std::wstring file_path = detail::FinalPath(handle.get());
		if (root_path.empty() || file_path.size() <= root_path.size() || file_path[root_path.size()] != L'\\' ||
		    CompareStringOrdinal(root_path.data(), static_cast<int>(root_path.size()), file_path.data(), static_cast<int>(root_path.size()), TRUE) != CSTR_EQUAL)
		{ result.error = "The workspace or file path changed. Try again."; return result; }
		result.path = paths::Utf8PathString(std::filesystem::path(file_path.substr(root_path.size() + 1)));
		BY_HANDLE_FILE_INFORMATION before{};
		if (!GetFileInformationByHandle(handle.get(), &before) ||
		    (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
		{ result.error = "Only regular text files can be inspected."; return result; }
		if (before.nFileSizeHigh != 0 || before.nFileSizeLow > kMaxPreviewBytes)
		{ result.error = "File preview is limited to 1 MiB."; return result; }
		result.text.resize(before.nFileSizeLow);
		DWORD count = 0;
		char extra = 0;
		DWORD extra_count = 0;
		BY_HANDLE_FILE_INFORMATION after{};
		if ((!result.text.empty() && (!ReadFile(handle.get(), result.text.data(), static_cast<DWORD>(result.text.size()), &count, nullptr) || count != result.text.size())) ||
		    !ReadFile(handle.get(), &extra, 1, &extra_count, nullptr) || extra_count != 0 || !GetFileInformationByHandle(handle.get(), &after) ||
		    before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
		    CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0)
		{ result.text.clear(); result.error = "The file changed while loading. Try again."; return result; }
#else
		detail::OwnedFile directory = detail::Own(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
		if (!directory) return result;
		struct stat root_opened{};
		if (fstat(*directory, &root_opened) != 0 || root_before.st_dev != root_opened.st_dev || root_before.st_ino != root_opened.st_ino)
		{ result.error = "The workspace changed while loading. Try again."; return result; }
		std::filesystem::path::const_iterator part = relative.begin();
		while (part != relative.end())
		{
			const std::filesystem::path component = *part;
			++part;
			const bool final = part == relative.end();
			detail::OwnedFile next = detail::Own(openat(*directory, component.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY)));
			if (!next) return result;
			directory = std::move(next);
		}
		struct stat before{};
		if (fstat(*directory, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0)
		{ result.error = "Only regular text files can be inspected."; return result; }
		if (static_cast<std::uintmax_t>(before.st_size) > kMaxPreviewBytes)
		{ result.error = "File preview is limited to 1 MiB."; return result; }
		result.text.resize(static_cast<std::size_t>(before.st_size));
		std::size_t offset = 0;
		while (offset < result.text.size())
		{
			const ssize_t count = read(*directory, result.text.data() + offset, result.text.size() - offset);
			if (count <= 0) { result.text.clear(); return result; }
			offset += static_cast<std::size_t>(count);
		}
		char extra = 0;
		struct stat after{};
		if (read(*directory, &extra, 1) != 0 || fstat(*directory, &after) != 0 || before.st_size != after.st_size || before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime
#if defined(__APPLE__)
		    || before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec || before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec
#else
		    || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec
#endif
		    )
		{ result.text.clear(); result.error = "The file changed while loading. Try again."; return result; }
#endif
		if (result.text.find('\0') != std::string::npos)
		{ result.text.clear(); result.error = "Binary files cannot be shown as text."; return result; }
		try { (void)nlohmann::json(result.text).dump(); }
		catch (const nlohmann::json::exception&)
		{ result.text.clear(); result.error = "This file does not contain valid UTF-8 text."; return result; }
		result.ok = true;
		result.error.clear();
		return result;
	}
}
