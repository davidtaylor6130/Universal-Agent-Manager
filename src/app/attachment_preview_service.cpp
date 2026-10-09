#include "app/attachment_preview_service.h"
#include <algorithm>
#include <array>
#include <mutex>
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace uam::attachment_preview
{
	namespace
	{
		std::mutex s_decodeMutex;

		bool ReadRegular(const std::filesystem::path& path, std::size_t cap, std::string& bytes)
		{
			if (path.empty() || path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) return false;
#if defined(_WIN32)
			std::filesystem::path native_path;
			std::string path_error;
			if (!uam::io::NativeFilePath(path, native_path, path_error)) return false;
			HANDLE handle = CreateFileW(native_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (handle == INVALID_HANDLE_VALUE) return false;
			BY_HANDLE_FILE_INFORMATION info{};
			const bool admitted = GetFileInformationByHandle(handle, &info) &&
			    (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
			    info.nFileSizeHigh == 0 && info.nFileSizeLow > 0 && info.nFileSizeLow <= cap;
			bool ok = false;
			if (admitted)
			{
				bytes.resize(info.nFileSizeLow);
				DWORD read = 0;
				ok = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
			}
			CloseHandle(handle);
#else
			const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
			if (fd < 0) return false;
			struct stat info{};
			bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
			    static_cast<std::uintmax_t>(info.st_size) <= cap;
			if (ok)
			{
				bytes.resize(static_cast<std::size_t>(info.st_size));
				std::size_t offset = 0;
				while (offset < bytes.size())
				{
					const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
					if (count <= 0) { ok = false; break; }
					offset += static_cast<std::size_t>(count);
				}
			}
			close(fd);
#endif
			if (!ok) bytes.clear();
			return ok;
		}

		bool CacheDirectory(std::filesystem::path& root, bool create)
		{
#if defined(_WIN32)
			std::filesystem::path native_root;
			std::string path_error;
			if (!uam::io::NativeFilePath(root, native_root, path_error)) return false;
			root = std::move(native_root);
#endif
			std::error_code ec;
			if (create) std::filesystem::create_directory(root, ec);
			const std::filesystem::file_status status = std::filesystem::symlink_status(root, ec);
			if (ec || status.type() != std::filesystem::file_type::directory) return false;
#if defined(_WIN32)
			const DWORD attrs = GetFileAttributesW(root.c_str());
			if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
#endif
			return true;
		}

		bool WriteExclusive(const std::filesystem::path& path, std::string_view bytes)
		{
#if defined(_WIN32)
			std::filesystem::path native_path;
			std::string path_error;
			if (!uam::io::NativeFilePath(path, native_path, path_error)) return false;
			HANDLE handle = CreateFileW(native_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (handle == INVALID_HANDLE_VALUE) return false;
			DWORD written = 0;
			const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
			    written == bytes.size() && FlushFileBuffers(handle);
			CloseHandle(handle);
#else
			const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (fd < 0) return false;
			std::size_t offset = 0;
			while (offset < bytes.size())
			{
				const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
				if (count <= 0) break;
				offset += static_cast<std::size_t>(count);
			}
			const bool ok = offset == bytes.size() && fsync(fd) == 0;
			close(fd);
#endif
			// A partial own immutable entry stays unusable: every read validates its content digest.
			return ok;
		}
	}

	bool IsKey(std::string_view key)
	{
		return key.size() == 64 && std::all_of(key.begin(), key.end(), [](char ch)
		{
			return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
		});
	}

	std::string Store(const std::filesystem::path& data_root, std::string_view source)
	{
		if (source.empty() || source.size() > kMaxSourceBytes) return {};
		std::lock_guard<std::mutex> lock(s_decodeMutex);
		const Image image = Decode(source);
		if (image.png.empty() || image.png.size() > kMaxPreviewBytes) return {};
		const std::string key = Digest(image.png);
		std::filesystem::path root = data_root / "attachment-previews";
		if (!IsKey(key) || !CacheDirectory(root, true)) return {};
		const std::filesystem::path path = root / (key + ".png");
		if (WriteExclusive(path, image.png)) return key;
		std::string existing;
		return ReadRegular(path, kMaxPreviewBytes, existing) && Digest(existing) == key ? key : std::string{};
	}

	Image Read(const std::filesystem::path& data_root, const MessageAttachment& attachment, bool local_host)
	{
		std::string bytes;
		if (!attachment.preview_key.empty())
		{
			std::filesystem::path root = data_root / "attachment-previews";
			if (!IsKey(attachment.preview_key) || !CacheDirectory(root, false) ||
			    !ReadRegular(root / (attachment.preview_key + ".png"), kMaxPreviewBytes, bytes) ||
			    Digest(bytes) != attachment.preview_key) return {};
		}
		else
		{
			// Relative historical paths have no immutable workspace origin. Never rebase them.
			if (!local_host || (attachment.mime_type != "image/png" && attachment.mime_type != "image/jpeg")) return {};
			const std::filesystem::path path = uam::paths::PathFromUtf8(
			    attachment.preview_origin_path.empty() ? attachment.path : attachment.preview_origin_path);
			if (!path.is_absolute() || !ReadRegular(path, kMaxSourceBytes, bytes)) return {};
		}
		std::lock_guard<std::mutex> lock(s_decodeMutex);
		return Decode(bytes);
	}
}
