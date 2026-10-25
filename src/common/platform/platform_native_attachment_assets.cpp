#include "common/chat/native_attachment_assets.h"

#include "common/platform/platform_services.h"
#include "common/utils/base64.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(_WIN32)
#include <wincrypt.h>
#endif

namespace uam::native_assets
{
	namespace
	{
		constexpr std::size_t kChunk = 64U * 1024U;
		constexpr std::size_t kReceiptLimit = 64U * 1024U;
		using Sink = std::function<bool(std::string_view)>;
		using Producer = std::function<bool(const Sink&)>;

		bool Fail(std::string& error, const char* message)
		{
			error = message;
			return false;
		}

		bool Cancelled(std::stop_token cancel, std::string& error)
		{
			if (!cancel.stop_requested()) return false;
			Fail(error, "Native asset operation cancelled.");
			return true;
		}

		bool LowerHex(std::string_view value, std::size_t size)
		{
			return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
		}

		bool ValidLimits(const Limits& limits)
		{
			return limits.original_bytes > 0 && limits.original_bytes <= kMaxOriginalBytes &&
			    limits.aggregate_bytes > 0 && limits.aggregate_bytes <= kMaxBatchBytes && limits.count > 0 && limits.count <= kMaxAssetCount;
		}

		// Narrow private crypto adapter: same native SHA256 providers as attachment
		// previews, without a common -> app dependency or new crypto implementation.
		class DigestBuilder
		{
		  public:
			DigestBuilder()
			{
#if defined(__APPLE__)
				m_ok = CC_SHA256_Init(&m_context) == 1;
#elif defined(_WIN32)
				m_ok = CryptAcquireContextW(&m_provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
				    CryptCreateHash(m_provider, CALG_SHA_256, 0, 0, &m_hash);
#endif
			}
			~DigestBuilder()
			{
#if defined(_WIN32)
				if (m_hash) CryptDestroyHash(m_hash);
				if (m_provider) CryptReleaseContext(m_provider, 0);
#endif
			}
			DigestBuilder(const DigestBuilder&) = delete;
			DigestBuilder& operator=(const DigestBuilder&) = delete;
			bool Add(std::string_view bytes)
			{
				if (!m_ok || bytes.size() > kChunk) return false;
#if defined(__APPLE__)
				m_ok = CC_SHA256_Update(&m_context, bytes.data(), static_cast<CC_LONG>(bytes.size())) == 1;
#elif defined(_WIN32)
				m_ok = CryptHashData(m_hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0) != 0;
#endif
				return m_ok;
			}
			std::string Finish()
			{
				std::array<unsigned char, 32> bytes{};
				if (!m_ok) return {};
#if defined(__APPLE__)
				m_ok = CC_SHA256_Final(bytes.data(), &m_context) == 1;
#elif defined(_WIN32)
				DWORD size = static_cast<DWORD>(bytes.size());
				m_ok = CryptGetHashParam(m_hash, HP_HASHVAL, bytes.data(), &size, 0) && size == bytes.size();
#endif
				if (!m_ok) return {};
				std::string hex;
				hex.reserve(64);
				for (unsigned char byte : bytes)
				{
					hex += hashing::kLowerHexDigits[byte >> 4];
					hex += hashing::kLowerHexDigits[byte & 15];
				}
				m_ok = false;
				return hex;
			}
		  private:
			bool m_ok = false;
#if defined(__APPLE__)
			CC_SHA256_CTX m_context{};
#elif defined(_WIN32)
			HCRYPTPROV m_provider = 0;
			HCRYPTHASH m_hash = 0;
#endif
		};

		std::string DigestBytes(std::string_view bytes, std::stop_token cancel = {})
		{
			DigestBuilder digest;
			for (std::size_t offset = 0; offset < bytes.size(); offset += kChunk)
			{
				if (cancel.stop_requested() || !digest.Add(bytes.substr(offset, kChunk))) return {};
			}
			return cancel.stop_requested() ? std::string{} : digest.Finish();
		}

		std::string OwnerHash(std::string_view owner)
		{
			if (owner.empty() || owner.size() > 4096 || owner.find('\0') != std::string_view::npos) return {};
			DigestBuilder digest;
			if (!digest.Add(std::string_view("uam-native-asset-owner-v1\0", 26)) || !digest.Add(owner)) return {};
			return digest.Finish();
		}

		bool ValidateBase64(std::string_view encoded, std::size_t limit, std::size_t& size, std::string& error, std::stop_token cancel)
		{
			if (Cancelled(cancel, error)) return false;
			if (encoded.empty()) { size = 0; return true; } // Canonical encoding of a genuine zero-byte original.
			if (encoded.size() > ((limit + 2) / 3) * 4) return Fail(error, "Native original exceeds the encoded size limit (at most 25 MiB decoded).");
			if (encoded.size() % 4 != 0) return Fail(error, "Native original requires canonical padded base64.");
			const std::size_t padding = encoded.back() == '=' ? (encoded.size() > 1 && encoded[encoded.size() - 2] == '=' ? 2 : 1) : 0;
			size = encoded.size() / 4 * 3 - padding;
			if (size > limit) return Fail(error, "Native original exceeds the decoded size limit (at most 25 MiB).");
			const std::size_t data_chars = encoded.size() - padding;
			for (std::size_t index = 0; index < data_chars; ++index)
			{
				if (index % kChunk == 0 && Cancelled(cancel, error)) return false;
				if (base64::DecodeValue(encoded[index]) < 0) return Fail(error, "Native original is not canonical base64.");
			}
			if (!base64::HasZeroUnusedTrailingBits(data_chars, base64::DecodeValue(encoded[data_chars - 1])))
				return Fail(error, "Native original has noncanonical base64 padding bits.");
			return true;
		}

		// Input was completely validated before this bounded decoding pass. Never use
		// the general Decode() helper, which reserves before checking its input.
		bool DecodeChunks(std::string_view encoded, const Sink& sink, std::stop_token cancel)
		{
			std::array<char, kChunk> buffer{};
			std::size_t used = 0;
			for (std::size_t offset = 0; offset < encoded.size(); offset += 4)
			{
				if (used + 3 > buffer.size())
				{
					if (cancel.stop_requested() || !sink(std::string_view(buffer.data(), used))) return false;
					used = 0;
				}
				const unsigned a = static_cast<unsigned>(base64::DecodeValue(encoded[offset]));
				const unsigned b = static_cast<unsigned>(base64::DecodeValue(encoded[offset + 1]));
				buffer[used++] = static_cast<char>(((a << 2) | (b >> 4)) & 0xffU);
				if (encoded[offset + 2] == '=') continue;
				const unsigned c = static_cast<unsigned>(base64::DecodeValue(encoded[offset + 2]));
				buffer[used++] = static_cast<char>(((b << 4) | (c >> 2)) & 0xffU);
				if (encoded[offset + 3] == '=') continue;
				const unsigned d = static_cast<unsigned>(base64::DecodeValue(encoded[offset + 3]));
				buffer[used++] = static_cast<char>(((c << 6) | d) & 0xffU);
			}
			return !cancel.stop_requested() && (used == 0 || sink(std::string_view(buffer.data(), used)));
		}

		struct Key
		{
			std::string owner;
			std::string generation;
			std::string digest;
			std::size_t size = 0;
		};

		bool ParseKey(std::string_view text, Key& key)
		{
			// Fixed grammar, not a relative path. No normalization/URI decoding occurs.
			if (text.size() < 168 || text.size() > 175 || !text.starts_with("na1:")) return false;
			if (text[68] != ':' || text[101] != ':' || text[166] != ':') return false;
			const auto owner = text.substr(4, 64), generation = text.substr(69, 32), digest = text.substr(102, 64), size = text.substr(167);
			if (!LowerHex(owner, 64) || !LowerHex(generation, 32) || !LowerHex(digest, 64) || size.empty() || (size.size() > 1 && size.front() == '0')) return false;
			std::size_t count = 0;
			const auto parsed = std::from_chars(size.data(), size.data() + size.size(), count);
			if (parsed.ec != std::errc{} || parsed.ptr != size.data() + size.size() || count > kMaxOriginalBytes) return false;
			key = {std::string(owner), std::string(generation), std::string(digest), count};
			return true;
		}

		std::string MakeKey(const Key& key)
		{
			return "na1:" + key.owner + ':' + key.generation + ':' + key.digest + ':' + std::to_string(key.size);
		}

		bool ResolveRoot(const std::filesystem::path& input, std::filesystem::path& root, std::string& error)
		{
			if (input.empty() || !input.is_absolute() || input.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
				return Fail(error, "Native assets require an existing resolved absolute data root.");
			std::error_code ec;
			const auto canonical = std::filesystem::canonical(input, ec);
			if (ec || !std::filesystem::is_directory(canonical, ec) || ec) return Fail(error, "The native asset data root is unavailable.");
			// Canonicalize the trusted root once: /tmp and user-selected root aliases
			// are legitimate. Reject links only inside the controlled private subtree.
			return io::NativeFilePath(canonical, root, error, true);
		}

		// Platform implementation behind common/chat/native_attachment_assets.h.
		// Public payload/store contracts contain no OS
		// handles. POSIX uses pinned openat directories; Windows pins the resolved
		// ancestor chain without delete sharing and uses the existing synced writer.
#if defined(_WIN32)
		class AssetFiles
		{
		  public:
			~AssetFiles() { for (auto it = m_handles.rbegin(); it != m_handles.rend(); ++it) CloseHandle(*it); }
			static std::unique_ptr<AssetFiles> Open(const std::filesystem::path& root, const std::string& owner, const std::string& generation, bool create, std::string& error)
			{
				auto files = std::unique_ptr<AssetFiles>(new AssetFiles);
				// Resolve junctions in the admitted data-root/OS ancestry before pinning.
				HANDLE initial = CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
				if (initial == INVALID_HANDLE_VALUE) { Fail(error, "Cannot open native asset data root."); return {}; }
				struct CloseInitial { HANDLE value; ~CloseInitial() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } initial_owner{initial};
				const DWORD required = GetFinalPathNameByHandleW(initial, nullptr, 0, FILE_NAME_NORMALIZED);
				if (required == 0 || required >= 65536) { Fail(error, "Native asset root path exceeds its bound."); return {}; }
				std::wstring resolved(required, L'\0');
				const DWORD count = GetFinalPathNameByHandleW(initial, resolved.data(), required, FILE_NAME_NORMALIZED);
				CloseHandle(initial);
				initial_owner.value = INVALID_HANDLE_VALUE;
				if (count == 0 || count >= required) { Fail(error, "Cannot resolve native asset data root."); return {}; }
				resolved.resize(count);
				// Parse the already normalized extended DOS/UNC root explicitly rather
				// than treating \\?\UNC as a standalone share during ancestor pinning.
				std::size_t root_end = std::wstring::npos;
				if (resolved.starts_with(L"\\\\?\\UNC\\"))
				{
					const auto server_end = resolved.find(L'\\', 8);
					if (server_end != std::wstring::npos)
					{
						root_end = resolved.find(L'\\', server_end + 1);
						if (root_end == std::wstring::npos && server_end + 1 < resolved.size())
						{
							root_end = resolved.size();
							resolved += L'\\';
						}
					}
				}
				else if (resolved.starts_with(L"\\\\?\\")) root_end = resolved.find(L'\\', 4);
				if (root_end == std::wstring::npos) { Fail(error, "Cannot identify the resolved native asset volume root."); return {}; }
				std::filesystem::path current(resolved.substr(0, root_end + 1));
				if (!files->Pin(current, error)) return {};
				for (std::size_t begin = root_end + 1; begin < resolved.size();)
				{
					const auto end = resolved.find(L'\\', begin);
					current /= resolved.substr(begin, end == std::wstring::npos ? end : end - begin);
					if (!files->Pin(current, error)) return {};
					if (end == std::wstring::npos) break;
					begin = end + 1;
				}
				for (const std::string& component : {std::string(kStoreDirectory), owner, generation})
				{
					current /= component;
					if (create && !CreateDirectoryW(current.c_str(), nullptr) && (component == generation || GetLastError() != ERROR_ALREADY_EXISTS))
					{ Fail(error, "Cannot exclusively create private native asset directory."); return {}; }
					if (!files->Pin(current, error)) return {};
				}
				files->m_path = current;
				return files;
			}
			bool Current() const
			{
				if (m_handles.empty()) return false;
				for (HANDLE handle : m_handles)
				{
					BY_HANDLE_FILE_INFORMATION info{};
					if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
					    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
				}
				return true; // Pins deny delete sharing (rename/removal) throughout the resolved path.
			}
			bool Write(const std::string& name, const Producer& producer, std::string& error, std::stop_token cancel = {})
			{
				if (Cancelled(cancel, error)) return false;
				if (!Current()) return Fail(error, "Native asset directory ownership changed.");
				io::AtomicWriteResult result;
				const bool ok = io::WriteAndSyncAtomicTemp(m_path / name, producer, {}, result);
				if (!ok && error.empty()) error = "Failed to write and sync native original or receipt.";
				return ok;
			}
			bool Read(const std::string& name, std::size_t cap, std::string& bytes, std::stop_token cancel, std::string& error) const
			{
				if (!Current()) return Fail(error, "Native asset directory ownership changed.");
				HANDLE file = CreateFileW((m_path / name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				if (file == INVALID_HANDLE_VALUE) return Fail(error, "Native original or receipt is unavailable.");
				struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{file};
				BY_HANDLE_FILE_INFORMATION before{};
				if (!GetFileInformationByHandle(file, &before) || (before.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
				    GetFileType(file) != FILE_TYPE_DISK || before.nNumberOfLinks != 1 || before.nFileSizeHigh != 0 || before.nFileSizeLow > cap)
					return Fail(error, "Native original or receipt is not an admitted bounded regular file.");
				bytes.resize(before.nFileSizeLow);
				for (std::size_t offset = 0; offset < bytes.size();)
				{
					if (Cancelled(cancel, error)) return false;
					DWORD read = 0;
					const DWORD count = static_cast<DWORD>(std::min(kChunk, bytes.size() - offset));
					if (!ReadFile(file, bytes.data() + offset, count, &read, nullptr) || read == 0) return Fail(error, "Native original read failed.");
					offset += read;
				}
				char extra = 0;
				DWORD extra_count = 0;
				BY_HANDLE_FILE_INFORMATION after{};
				return ReadFile(file, &extra, 1, &extra_count, nullptr) && extra_count == 0 && GetFileInformationByHandle(file, &after) &&
				    before.nFileSizeLow == after.nFileSizeLow && CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0 &&
				    !Cancelled(cancel, error) ? true : Fail(error, "Native original changed during its read.");
			}
			bool Sync(std::string& error) const { return Current() ? true : Fail(error, "Native asset directory ownership changed."); } // Files use FlushFileBuffers before close; no portable Win32 directory fsync.
			void Cleanup(const std::vector<std::string>& names) noexcept
			{
				try
				{
					if (!Current()) return;
					for (const auto& name : names) DeleteFileW((m_path / name).c_str());
					if (!m_handles.empty())
					{
						BY_HANDLE_FILE_INFORMATION original{}, current{};
						const bool observed = GetFileInformationByHandle(m_handles.back(), &original) != 0;
						CloseHandle(m_handles.back());
						m_handles.pop_back();
						HANDLE own = CreateFileW(m_path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
						    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
						if (own != INVALID_HANDLE_VALUE)
						{
							if (observed && GetFileInformationByHandle(own, &current) && original.dwVolumeSerialNumber == current.dwVolumeSerialNumber &&
							    original.nFileIndexHigh == current.nFileIndexHigh && original.nFileIndexLow == current.nFileIndexLow &&
							    (current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
							{
								FILE_DISPOSITION_INFO disposition{TRUE};
								SetFileInformationByHandle(own, FileDispositionInfo, &disposition, sizeof(disposition));
							}
							CloseHandle(own);
						}
					}
				}
				catch (...) {} // Cleanup failure retains only this private generation.
			}
		  private:
			bool Pin(const std::filesystem::path& path, std::string& error)
			{
				// An attribute-only open bypasses share-access checks, so it would pin nothing. List access
				// enforces the missing FILE_SHARE_DELETE; write sharing keeps renames into these directories working.
				HANDLE handle = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
				if (handle == INVALID_HANDLE_VALUE) return Fail(error, "Cannot pin private native asset directory.");
				BY_HANDLE_FILE_INFORMATION info{};
				if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				{ CloseHandle(handle); return Fail(error, "Native asset private directories must not be links or reparse points."); }
				try { m_handles.push_back(handle); }
				catch (...) { CloseHandle(handle); throw; }
				return true;
			}
			std::vector<HANDLE> m_handles;
			std::filesystem::path m_path;
		};
#else
		class AssetFiles
		{
		  public:
			~AssetFiles() { for (int descriptor : m_fds) close(descriptor); }
			static std::unique_ptr<AssetFiles> Open(const std::filesystem::path& root, const std::string& owner, const std::string& generation, bool create, std::string& error)
			{
				auto files = std::unique_ptr<AssetFiles>(new AssetFiles);
				files->m_fds.reserve(4);
				files->m_root = root;
				files->m_names = {std::string(kStoreDirectory), owner, generation};
				const int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
				if (fd < 0) { Fail(error, "Cannot pin native asset data root."); return {}; }
				files->m_fds.push_back(fd);
				for (const auto& name : files->m_names)
				{
					const int parent = files->m_fds.back();
					if (create && mkdirat(parent, name.c_str(), 0700) != 0 && (name == generation || errno != EEXIST))
					{ Fail(error, "Cannot exclusively create private native asset directory."); return {}; }
					const int next = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
					if (next < 0) { Fail(error, "Native asset private directories must be real directories, never links."); return {}; }
					files->m_fds.push_back(next);
					struct stat info{};
					if (fstat(next, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 0077) != 0)
					{ Fail(error, "Native asset private directories must belong exclusively to the current user."); return {}; }
				}
				if (!files->Current()) { Fail(error, "Native asset directory changed during admission."); return {}; }
				return files;
			}
			bool Current() const
			{
				if (m_fds.size() != 4) return false;
				struct stat named{}, pinned{};
				if (lstat(m_root.c_str(), &named) != 0 || fstat(m_fds.front(), &pinned) != 0 || !SameDirectory(named, pinned)) return false;
				for (std::size_t index = 0; index < m_names.size(); ++index)
				{
					if (fstatat(m_fds[index], m_names[index].c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
					    fstat(m_fds[index + 1], &pinned) != 0 || !SameDirectory(named, pinned) || pinned.st_uid != geteuid() || (pinned.st_mode & 0077) != 0) return false;
				}
				return true;
			}
			bool Write(const std::string& name, const Producer& producer, std::string& error, std::stop_token cancel = {})
			{
				if (Cancelled(cancel, error)) return false;
				if (!Current()) return Fail(error, "Native asset directory ownership changed.");
				const int fd = openat(m_fds.back(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				if (fd < 0) return Fail(error, "Cannot exclusively create native original or receipt.");
				struct Close { int fd; ~Close() { if (fd >= 0) close(fd); } } file{fd};
				const Sink append = [&](std::string_view chunk)
				{
					if (chunk.size() > kChunk) return Fail(error, "Native asset write chunk exceeds its bound.");
					for (std::size_t offset = 0; offset < chunk.size();)
					{
						if (Cancelled(cancel, error)) return false;
						const ssize_t written = write(fd, chunk.data() + offset, chunk.size() - offset);
						if (written < 0 && errno == EINTR) continue;
						if (written <= 0) return Fail(error, "Native original write failed.");
						offset += static_cast<std::size_t>(written);
					}
					return true;
				};
				if (!producer(append)) return false;
#if defined(__APPLE__)
				const bool synced = fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
#else
				const bool synced = fsync(fd) == 0;
#endif
				const bool closed = close(fd) == 0;
				file.fd = -1;
				return synced && closed && Current() ? true : Fail(error, "Native original sync or directory validation failed.");
			}
			bool Read(const std::string& name, std::size_t cap, std::string& bytes, std::stop_token cancel, std::string& error) const
			{
				if (!Current()) return Fail(error, "Native asset directory ownership changed.");
				const int fd = openat(m_fds.back(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
				if (fd < 0) return Fail(error, "Native original or receipt is unavailable.");
				struct Close { int fd; ~Close() { close(fd); } } file{fd};
				struct stat before{};
				if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > cap)
					return Fail(error, "Native original or receipt is not an admitted bounded regular file.");
				bytes.resize(static_cast<std::size_t>(before.st_size));
				for (std::size_t offset = 0; offset < bytes.size();)
				{
					if (Cancelled(cancel, error)) return false;
					const ssize_t count = read(fd, bytes.data() + offset, std::min(kChunk, bytes.size() - offset));
					if (count < 0 && errno == EINTR) continue;
					if (count <= 0) return Fail(error, "Native original read failed.");
					offset += static_cast<std::size_t>(count);
				}
				char extra = 0;
				struct stat after{};
				if (read(fd, &extra, 1) != 0 || fstat(fd, &after) != 0 || before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
				    before.st_ctime != after.st_ctime || !Current() || Cancelled(cancel, error)) return Fail(error, "Native original changed during its read.");
				return true;
			}
			bool Sync(std::string& error) const
			{
				for (auto it = m_fds.rbegin(); it != m_fds.rend(); ++it)
				{
					if (fsync(*it) != 0) return Fail(error, "Native asset directory sync failed.");
				}
				return Current() ? true : Fail(error, "Native asset directory ownership changed.");
			}
			void Cleanup(const std::vector<std::string>& names) noexcept
			{
				if (!Current()) return; // A replaced/aliased tree is never cleanup authority.
				for (const auto& name : names) unlinkat(m_fds.back(), name.c_str(), 0);
				unlinkat(m_fds[2], m_names.back().c_str(), AT_REMOVEDIR); // Never recurse or remove an owner/store directory.
				(void)fsync(m_fds[2]);
			}
		  private:
			static bool SameDirectory(const struct stat& a, const struct stat& b)
			{
				return S_ISDIR(a.st_mode) && S_ISDIR(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
			}
			std::filesystem::path m_root;
			std::vector<std::string> m_names;
			std::vector<int> m_fds;
		};
#endif

		std::string Marker(std::string_view state, const PublicationReceipt& receipt)
		{
			return "uam-native-assets-v1\n" + std::string(state) + '\n' + receipt.owner_hash + '\n' + receipt.generation + '\n';
		}

		bool WriteText(AssetFiles& files, const std::string& name, const std::string& text, std::string& error)
		{
			return files.Write(name, [&](const Sink& append) { return append(text); }, error);
		}
	}

	PayloadBudget::PayloadBudget(Limits limits) : m_limits(limits) {}
	std::size_t PayloadBudget::UsedBytes() const { const std::lock_guard lock(m_mutex); return m_bytes; }
	std::size_t PayloadBudget::UsedCount() const { const std::lock_guard lock(m_mutex); return m_count; }
	bool PayloadBudget::Acquire(std::size_t bytes, std::string& error)
	{
		const std::lock_guard lock(m_mutex);
		if (!ValidLimits(m_limits)) return Fail(error, "Invalid native asset payload limits.");
		if (m_count >= m_limits.count) return Fail(error, "Native asset payload count limit exceeded.");
		if (bytes > m_limits.aggregate_bytes - m_bytes) return Fail(error, "Native asset aggregate decoded byte limit exceeded.");
		m_bytes += bytes;
		++m_count;
		return true;
	}
	void PayloadBudget::Release(std::size_t bytes)
	{
		const std::lock_guard lock(m_mutex);
		m_bytes -= bytes;
		--m_count;
	}

	InlinePayload::InlinePayload(std::string&& encoded, std::string mime, std::string digest, std::size_t size, std::shared_ptr<PayloadBudget> budget)
	    : m_encoded(std::move(encoded)), m_mime(std::move(mime)), m_digest(std::move(digest)), m_size(size), m_budget(std::move(budget)) {}
	InlinePayload::~InlinePayload() { m_budget->Release(m_size); }

	Payload MakeInlinePayload(std::string&& encoded, std::string mime, const std::shared_ptr<PayloadBudget>& budget, std::string& error, std::stop_token cancel)
	{
		error.clear();
		if (!budget || !ValidLimits(budget->m_limits)) { Fail(error, "A valid shared native asset payload budget is required."); return {}; }
		if (mime.empty() || mime.size() > 255 || mime.find('/') == std::string::npos || std::any_of(mime.begin(), mime.end(), [](unsigned char ch) { return ch < 32 || ch >= 127; }))
		{ Fail(error, "Native original requires a bounded MIME type."); return {}; }
		std::size_t size = 0;
		if (!ValidateBase64(encoded, budget->m_limits.original_bytes, size, error, cancel) || !budget->Acquire(size, error)) return {};
		const auto release = [budget, size](PayloadBudget*) { budget->Release(size); };
		std::unique_ptr<PayloadBudget, decltype(release)> reservation(budget.get(), release);
		try
		{
			DigestBuilder digest;
			if (!DecodeChunks(encoded, [&](std::string_view chunk) { return digest.Add(chunk); }, cancel))
			{ if (!Cancelled(cancel, error)) Fail(error, "Native original SHA256 is unavailable on this platform."); return {}; }
			std::string hash = digest.Finish();
			if (!LowerHex(hash, 64)) { Fail(error, "Native original SHA256 failed."); return {}; }
			auto payload = std::unique_ptr<InlinePayload>(new InlinePayload(std::move(encoded), std::move(mime), std::move(hash), size, budget));
			reservation.release();
			return Payload(std::move(payload));
		}
		catch (...) { Fail(error, "Native original payload allocation failed."); return {}; }
	}

	struct PreparedBatch::Impl
	{
		std::unique_ptr<AssetFiles> files;
		std::vector<AssetReference> assets;
		std::vector<std::string> created_names;
		PublicationReceipt receipt;
		bool attempt_file_synced = false;
		bool published_file_synced = false;
		~Impl()
		{
			if (files && receipt.state == PublicationState::PreparedUnpublished) files->Cleanup(created_names);
		}
	};

	PreparedBatch::PreparedBatch(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
	PreparedBatch::~PreparedBatch() = default;
	PreparedBatch::PreparedBatch(PreparedBatch&&) noexcept = default;
	PreparedBatch& PreparedBatch::operator=(PreparedBatch&&) noexcept = default;
	const std::vector<AssetReference>& PreparedBatch::Assets() const { return m_impl->assets; }
	PublicationReceipt PreparedBatch::Receipt() const { return m_impl->receipt; }

	std::unique_ptr<PreparedBatch> PreparedBatch::Prepare(const std::filesystem::path& resolved_data_root, std::string_view owner,
	    const std::vector<Payload>& originals, std::string& error, std::stop_token cancel, const PreparationHook& hook)
	{
		error.clear();
		try
		{
			if (Cancelled(cancel, error)) return {};
			if (originals.empty() || originals.size() > kMaxAssetCount) { Fail(error, "Native asset batch requires 1 to 256 originals."); return {}; }
			std::size_t total = 0;
			for (const auto& original : originals)
			{
				if (!original) { Fail(error, "Native asset batch contains a missing payload."); return {}; }
				if (original->DecodedSize() > kMaxOriginalBytes || original->DecodedSize() > kMaxBatchBytes - total)
				{ Fail(error, "Native asset batch exceeds 50 MiB decoded or 25 MiB per original."); return {}; }
				total += original->DecodedSize();
			}
			auto impl = std::make_unique<Impl>();
			impl->receipt.owner_hash = OwnerHash(owner);
			if (!LowerHex(impl->receipt.owner_hash, 64)) { Fail(error, "Native asset owner identity or SHA256 is unavailable."); return {}; }
			std::string generation = PlatformServicesFactory::Instance().process_service.GenerateUuid();
			generation.erase(std::remove(generation.begin(), generation.end(), '-'), generation.end());
			if (!LowerHex(generation, 32)) { Fail(error, "Cannot generate a private native asset generation."); return {}; }
			impl->receipt.generation = generation;
			std::filesystem::path root;
			if (!ResolveRoot(resolved_data_root, root, error)) return {};
			impl->files = AssetFiles::Open(root, impl->receipt.owner_hash, generation, true, error);
			if (!impl->files) return {};
			const auto checkpoint = [&](PreparationPoint point, std::size_t index)
			{
				if (Cancelled(cancel, error)) return false;
				return !hook || hook(point, index) ? true : Fail(error, "Native asset preparation interrupted.");
			};
			if (!checkpoint(PreparationPoint::GenerationCreated, 0)) return {};
			for (std::size_t index = 0; index < originals.size(); ++index)
			{
				const auto& original = *originals[index];
				const std::string name = original.Digest() + ".bin";
				if (!checkpoint(PreparationPoint::BeforeOriginal, index)) return {};
				if (std::find(impl->created_names.begin(), impl->created_names.end(), name) == impl->created_names.end())
				{
					impl->created_names.push_back(name); // Includes partial own files on failure.
					if (!impl->files->Write(name, [&](const Sink& append)
					{
						return DecodeChunks(original.m_encoded, [&](std::string_view chunk)
						{
							return checkpoint(PreparationPoint::OriginalChunk, index) && append(chunk);
						}, cancel);
					}, error, cancel)) { if (error.empty()) Fail(error, "Native original preparation failed."); return {}; }
				}
				impl->assets.push_back({MakeKey({impl->receipt.owner_hash, generation, original.Digest(), original.DecodedSize()}), original.MimeType(), original.Digest(), original.DecodedSize()});
				if (!checkpoint(PreparationPoint::OriginalSynced, index)) return {};
			}
			std::string manifest = Marker("prepared", impl->receipt);
			for (const auto& asset : impl->assets) manifest += asset.key + '\n';
			impl->created_names.push_back("prepared.receipt");
			if (manifest.size() > kReceiptLimit) { Fail(error, "Native asset manifest exceeds its bound."); return {}; }
			if (!WriteText(*impl->files, "prepared.receipt", manifest, error) || !impl->files->Sync(error) ||
			    !checkpoint(PreparationPoint::BatchSynced, originals.size())) return {};
			return std::unique_ptr<PreparedBatch>(new PreparedBatch(std::move(impl)));
		}
		catch (...) { Fail(error, "Native asset preparation failed with an exception."); return {}; }
	}

	bool PreparedBatch::MarkPublicationAttempted(std::string& error)
	{
		error.clear();
		if (!m_impl) return Fail(error, "Native asset batch has been moved.");
		// The primary may be visible even if its atomic writer reports false or
		// throws. No destructor after this line is allowed to roll back originals.
		if (m_impl->receipt.state == PublicationState::PublishedAcknowledged) return true;
		m_impl->receipt.state = PublicationState::AttemptedUncertain;
		try
		{
			const std::string expected = Marker("attempted", m_impl->receipt);
			std::string previous, read_error;
			if (m_impl->files->Read("attempted.receipt", kReceiptLimit, previous, {}, read_error) && previous == expected)
			{
				// Readability does not prove that an earlier file sync succeeded.
				if (!m_impl->attempt_file_synced) return Fail(error, "Native asset attempt receipt has no successful file sync; originals retained.");
				return m_impl->files->Sync(error);
			}
			if (!WriteText(*m_impl->files, "attempted.receipt", expected, error)) return false;
			m_impl->attempt_file_synced = true;
			return m_impl->files->Sync(error);
		}
		catch (...) { return Fail(error, "Native asset publication attempt receipt failed; originals retained."); }
	}

	bool PreparedBatch::MarkPublished(std::string& error)
	{
		error.clear();
		if (!m_impl || m_impl->receipt.state == PublicationState::PreparedUnpublished) return Fail(error, "Native asset publication was not marked attempted.");
		if (m_impl->receipt.state == PublicationState::PublishedAcknowledged) return true;
		try
		{
			std::string attempted, prior, read_error;
			if (!m_impl->attempt_file_synced || !m_impl->files->Read("attempted.receipt", kReceiptLimit, attempted, {}, error) || attempted != Marker("attempted", m_impl->receipt))
				return Fail(error, "Native asset publication attempt receipt is not valid; originals retained.");
			const std::string expected = Marker("published", m_impl->receipt);
			const bool already_written = m_impl->files->Read("published.receipt", kReceiptLimit, prior, {}, read_error) && prior == expected;
			if (already_written && !m_impl->published_file_synced)
				return Fail(error, "Native asset acknowledgement receipt has no successful file sync; originals retained.");
			if (!already_written)
			{
				if (!WriteText(*m_impl->files, "published.receipt", expected, error)) return false;
				m_impl->published_file_synced = true;
			}
			if (!m_impl->files->Sync(error)) return false;
			m_impl->receipt.state = PublicationState::PublishedAcknowledged;
			return true;
		}
		catch (...) { return Fail(error, "Native asset publication acknowledgement receipt failed; originals retained."); }
	}

	bool IsKeyForOwner(std::string_view owner, std::string_view key)
	{
		Key parsed;
		return ParseKey(key, parsed) && parsed.owner == OwnerHash(owner);
	}

	bool IsReferenceValid(const AssetReference& reference, std::string_view owner)
	{
		Key parsed;
		return ParseKey(reference.key, parsed) && parsed.digest == reference.digest &&
		    parsed.size == reference.size_bytes && (owner.empty() || parsed.owner == OwnerHash(owner));
	}

	ReadResult ReadOwned(const std::filesystem::path& data_root, std::string_view owner, std::string_view key, std::size_t max_bytes, std::stop_token cancel)
	{
		ReadResult result;
		try
		{
			if (Cancelled(cancel, result.error)) return result;
			Key parsed;
			if (!ParseKey(key, parsed) || parsed.owner != OwnerHash(owner)) { Fail(result.error, "Invalid native asset key or foreign owner."); return result; }
			if (max_bytes == 0 || parsed.size > std::min(max_bytes, kMaxOriginalBytes)) { Fail(result.error, "Native original exceeds the read bound."); return result; }
			std::filesystem::path root;
			if (!ResolveRoot(data_root, root, result.error)) return result;
			auto files = AssetFiles::Open(root, parsed.owner, parsed.generation, false, result.error);
			if (!files) return result;
			std::string attempted, manifest;
			const PublicationReceipt receipt{parsed.owner, parsed.generation, PublicationState::AttemptedUncertain};
			const std::string prefix = Marker("prepared", receipt);
			if (!files->Read("attempted.receipt", kReceiptLimit, attempted, cancel, result.error) || attempted != Marker("attempted", receipt) ||
			    !files->Read("prepared.receipt", kReceiptLimit, manifest, cancel, result.error) || !manifest.starts_with(prefix))
			{ if (result.error.empty()) Fail(result.error, "Native original has no valid publication-attempt receipt."); return result; }
			const std::string declared = '\n' + std::string(key) + '\n';
			if (manifest.find(declared, prefix.size() - 1) == std::string::npos) { Fail(result.error, "Native original was not declared in this generation."); return result; }
			if (!files->Read(parsed.digest + ".bin", parsed.size, result.bytes, cancel, result.error) || result.bytes.size() != parsed.size ||
			    DigestBytes(result.bytes, cancel) != parsed.digest || !files->Current() || Cancelled(cancel, result.error))
			{ result.bytes.clear(); if (!Cancelled(cancel, result.error) && result.error.empty()) Fail(result.error, "Native original failed size or SHA256 verification."); return result; }
			result.ok = true;
		}
		catch (...) { result.bytes.clear(); Fail(result.error, "Native original read failed with an exception."); }
		return result;
	}
}
