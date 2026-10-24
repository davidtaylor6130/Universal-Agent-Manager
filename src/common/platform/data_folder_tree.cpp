#include "data_folder_tree.h"
#include "common/platform/platform_incremental_sha256.h"
#include "common/platform/retained_directory_identity.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#if !defined(_WIN32)
#include <dirent.h>
#endif

namespace fs = std::filesystem;

namespace
{
#if defined(_WIN32)
	using Handle = HANDLE;
	const Handle kInvalid = INVALID_HANDLE_VALUE;
	void Close(Handle h)
	{
		if (h != kInvalid)
			CloseHandle(h);
	}
#else
	using Handle = int;
	constexpr Handle kInvalid = -1;
	void Close(Handle h)
	{
		if (h >= 0)
			close(h);
	}
#endif
	struct Object
	{
		Handle handle = kInvalid;
		fs::path path;
		std::vector<std::shared_ptr<Object>> parents;
		~Object()
		{
			Close(handle);
		}
	};
	bool Named(const Object& object)
	{
		if (object.parents.empty())
			return true;
		const auto& parent = *object.parents.front();
		if (!Named(parent))
			return false;
#if !defined(_WIN32)
		struct stat retained{}, observed{};
		return fstat(object.handle, &retained) == 0 && fstatat(parent.handle, object.path.filename().c_str(), &observed, AT_SYMLINK_NOFOLLOW) == 0 && retained.st_dev == observed.st_dev && retained.st_ino == observed.st_ino && (retained.st_mode & S_IFMT) == (observed.st_mode & S_IFMT);
#else
		// Every retained Windows object denies delete sharing, including all ancestors.
		return true;
#endif
	}
	bool Safe(const fs::path& relative, bool allow_empty = false)
	{
		if (relative.is_absolute() || relative.has_root_path() || (!allow_empty && relative.empty()) || relative.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
			return false;
		for (const auto& component : relative)
		{
			if (component.empty() || component == "." || component == "..")
				return false;
#if defined(_WIN32)
			if (component.native().find(L':') != std::wstring::npos)
				return false;
#endif
		}
		return true;
	}
	std::string Stamp(const Object& object, bool directory, std::uint64_t* bytes = nullptr, std::uint32_t* mode = nullptr)
	{
#if defined(_WIN32)
		BY_HANDLE_FILE_INFORMATION info{};
		if (!GetFileInformationByHandle(object.handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory || (!directory && info.nNumberOfLinks != 1))
			return {};
		if (bytes)
			*bytes = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
		if (mode)
			*mode = 0;
		std::string stamp = std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow);
		if (!directory)
			stamp += ":" + std::to_string(info.nFileSizeHigh) + ":" + std::to_string(info.nFileSizeLow) + ":" + std::to_string(info.ftLastWriteTime.dwHighDateTime) + ":" + std::to_string(info.ftLastWriteTime.dwLowDateTime);
		return stamp;
#else
		struct stat info{};
		if (fstat(object.handle, &info) != 0 || (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) || (!directory && info.st_nlink != 1) || info.st_size < 0)
			return {};
		if (bytes)
			*bytes = static_cast<std::uint64_t>(info.st_size);
		if (mode)
			*mode = info.st_mode & 0777;
		std::string stamp = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#if defined(__APPLE__)
		const auto modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
		const auto modified = info.st_mtim, changed = info.st_ctim;
#endif
		if (!directory)
			stamp += ":" + std::to_string(info.st_size) + ":" + std::to_string(modified.tv_sec) + ":" + std::to_string(modified.tv_nsec) + ":" + std::to_string(changed.tv_sec) + ":" + std::to_string(changed.tv_nsec);
		return stamp;
#endif
	}
	std::string Modified(const Object& object)
	{
#if defined(_WIN32)
		FILETIME modified{};
		if (!GetFileTime(object.handle, nullptr, nullptr, &modified))
			return {};
		return std::to_string(modified.dwHighDateTime) + ":" + std::to_string(modified.dwLowDateTime);
#else
		struct stat info{};
		if (fstat(object.handle, &info) != 0 || !S_ISREG(info.st_mode))
			return {};
#if defined(__APPLE__)
		const auto modified = info.st_mtimespec;
#else
		const auto modified = info.st_mtim;
#endif
		return std::to_string(modified.tv_sec) + ":" + std::to_string(modified.tv_nsec);
#endif
	}
	bool PreserveModified(const Object& source, const Object& target, const std::string& expected)
	{
		if (expected.empty() || Modified(source) != expected)
			return false;
#if defined(_WIN32)
		FILETIME modified{};
		if (!GetFileTime(source.handle, nullptr, nullptr, &modified) || !SetFileTime(target.handle, nullptr, nullptr, &modified))
			return false;
#else
		struct stat info{};
		if (fstat(source.handle, &info) != 0)
			return false;
#if defined(__APPLE__)
		const auto modified = info.st_mtimespec;
#else
		const auto modified = info.st_mtim;
#endif
		const timespec times[2]{{0, UTIME_OMIT}, modified};
		if (futimens(target.handle, times) != 0)
			return false;
#endif
		return Modified(source) == expected && Modified(target) == expected;
	}
	bool WriteAll(Handle handle, std::string_view bytes)
	{
		while (!bytes.empty())
		{
#if defined(_WIN32)
			DWORD count = 0;
			if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536)), &count, nullptr) || !count)
				return false;
#else
			const auto count = write(handle, bytes.data(), bytes.size());
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				return false;
#endif
			bytes.remove_prefix(static_cast<std::size_t>(count));
		}
		return true;
	}
	bool Sync(Handle handle)
	{
#if defined(_WIN32)
		return FlushFileBuffers(handle) != 0;
#else
		return fsync(handle) == 0;
#endif
	}
	bool Consume(Object& file, const std::function<bool(std::string_view)>& consume)
	{
		std::array<char, 65536> buffer{};
		for (;;)
		{
#if defined(_WIN32)
			DWORD count = 0;
			if (!ReadFile(file.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
				return false;
#else
			const auto count = read(file.handle, buffer.data(), buffer.size());
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0)
				return false;
#endif
			if (!count)
				return true;
			if (!consume(std::string_view(buffer.data(), static_cast<std::size_t>(count))))
				return false;
		}
	}
} // namespace

struct uam::platform::DataFolderTree::Impl
{
	std::shared_ptr<Object> root;
	std::shared_ptr<const RetainedDirectoryIdentity> named;
	std::string identity;
	ReadObserver read_observer;
	bool Current() const
	{
		return named->MatchesDirectory(root->handle) && named->Current() && Stamp(*root, true) == identity;
	}

	std::shared_ptr<Object> Directory(const fs::path& relative, bool create) const
	{
		if (!Safe(relative, true) || !Current())
			return {};
		auto parent = root;
		for (const auto& component : relative)
		{
			auto next = std::make_shared<Object>();
			next->path = parent->path / component;
			next->parents.push_back(parent);
#if defined(_WIN32)
			fs::path native;
			std::string error;
			if (!io::NativeFilePath(next->path, native, error, true))
				return {};
			if (create && !CreateDirectoryW(native.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
				return {};
			next->handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
			if (create && mkdirat(parent->handle, component.c_str(), 0700) != 0 && errno != EEXIST)
				return {};
			if (create && !Sync(parent->handle))
				return {};
			next->handle = openat(parent->handle, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#endif
			if (next->handle == kInvalid || Stamp(*next, true).empty() || !Named(*next))
				return {};
			parent = std::move(next);
		}
		return parent;
	}
	std::shared_ptr<Object> File(const fs::path& relative, bool create) const
	{
		if (!Safe(relative))
			return {};
		const auto parent = Directory(relative.parent_path(), create);
		if (!parent)
			return {};
		auto file = std::make_shared<Object>();
		file->path = parent->path / relative.filename();
		file->parents.push_back(parent);
#if defined(_WIN32)
		fs::path native;
		std::string error;
		if (!io::NativeFilePath(file->path, native, error, true))
			return {};
		file->handle = CreateFileW(native.c_str(), create ? GENERIC_WRITE | GENERIC_READ : GENERIC_READ, create ? 0 : FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
		file->handle = openat(parent->handle, relative.filename().c_str(), O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY), 0600);
#endif
		return file->handle != kInvalid && !Stamp(*file, false).empty() ? file : std::shared_ptr<Object>{};
	}
	bool Walk(const fs::path& relative, Inventory& entries, unsigned depth, std::size_t& remaining) const
	{
		if (depth > 64 || entries.size() > 100000)
			return false;
		const auto directory = Directory(relative, false);
		if (!directory)
			return false;
		std::vector<fs::path> children;
		const auto append = [&](const fs::path& name)
		{
			if (name == "." || name == ".." || (relative.empty() && (name == ".uam-data-root.lock" || name == ".uam-data-writers.lock" || name == ".data-folder-control" || name == ".uam-data-folder")))
				return true;
			// Charge every discovered child once, including siblings not walked yet.
			// The sum of all retained listings and inventory entries is bounded.
			if (!remaining || relative == fs::path("worktrees"))
				return false;
			--remaining;
			children.push_back(name);
			return true;
		};
#if defined(_WIN32)
		fs::path native;
		std::string error;
		if (!io::NativeFilePath(directory->path / "*", native, error, true))
			return false;
		WIN32_FIND_DATAW data{};
		const HANDLE search = FindFirstFileW(native.c_str(), &data);
		if (search == INVALID_HANDLE_VALUE)
			return GetLastError() == ERROR_FILE_NOT_FOUND;
		do
		{
			if (!append(fs::path(data.cFileName)))
			{
				FindClose(search);
				return false;
			}
		} while (FindNextFileW(search, &data));
		const bool complete = GetLastError() == ERROR_NO_MORE_FILES;
		FindClose(search);
		if (!complete)
			return false;
#else
		const int listing_fd = openat(directory->handle, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (listing_fd < 0)
			return false;
		DIR* listing = fdopendir(listing_fd);
		if (!listing)
		{
			close(listing_fd);
			return false;
		}
		errno = 0;
		while (const auto* entry = readdir(listing))
		{
			if (!append(fs::path(entry->d_name)))
			{
				closedir(listing);
				return false;
			}
			errno = 0;
		}
		const int error = errno;
		closedir(listing);
		if (error)
			return false;
#endif
		// Managed worktrees need their own relocation owner. Enumerate their immediate
		// entries only; never hash the private project/evidence trees before refusal.
		if (relative == fs::path("worktrees") && !children.empty())
			return false;
		std::sort(children.begin(), children.end());
		for (const auto& child : children)
		{
			if (relative.empty() && (child == ".uam-data-root.lock" || child == ".uam-data-writers.lock" || child == ".data-folder-control" || child == ".uam-data-folder"))
				continue;
			const fs::path path = relative / child;
			if (!Safe(path) || entries.size() >= 100000)
				return false;
			if (const auto dir = Directory(path, false))
			{
				Entry entry;
				entry.relative = path;
				entry.directory = true;
				entry.object = Stamp(*dir, true);
				entries.push_back(std::move(entry));
				if (!Walk(path, entries, depth + 1, remaining))
					return false;
			}
			else
			{
				if (path == fs::path("worktrees"))
					return false;
				auto file = File(path, false);
				if (!file)
					return false;
				Entry entry;
				entry.relative = path;
				entry.object = Stamp(*file, false, &entry.bytes, &entry.mode);
				IncrementalSha256 hash;
				std::uint64_t read = 0;
				if (!Consume(*file,
				             [&](std::string_view bytes)
				             {
					             if (bytes.size() > entry.bytes - read)
						             return false;
					             read += bytes.size();
					             return hash.Add(bytes);
				             }) ||
				    read != entry.bytes || Stamp(*file, false) != entry.object)
					return false;
				entry.sha256 = hash.Finish();
				entry.modified = Modified(*file);
				if (entry.sha256.size() != 64 || entry.modified.empty())
					return false;
				entries.push_back(std::move(entry));
			}
		}
		return Current() && Named(*directory);
	}
};

uam::platform::DataFolderTree::DataFolderTree(std::unique_ptr<Impl> impl) : m_impl(std::move(impl))
{
}
uam::platform::DataFolderTree::~DataFolderTree() = default;

std::shared_ptr<uam::platform::DataFolderTree> uam::platform::DataFolderTree::Open(const fs::path& root, std::string& error, const OpenObserver& observer, const ReadObserver& read_observer)
{
	if (!root.is_absolute())
	{
		error = "A stopped data root must be absolute.";
		return {};
	}
	auto impl = std::make_unique<Impl>();
	impl->read_observer = read_observer;
	impl->named = RetainedDirectoryIdentity::Capture(root);
	if (!impl->named)
	{
		error = "Cannot pin the data-root directory.";
		return {};
	}
	if (observer)
		observer(OpenStage::IdentityRetained);
	impl->root = std::make_shared<Object>();
	impl->root->path = root;
#if defined(_WIN32)
	fs::path native;
	if (!io::NativeFilePath(root, native, error, true))
		return {};
	impl->root->handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
	impl->root->handle = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#endif
	if (observer)
		observer(OpenStage::DirectoryOpened);
	if (impl->root->handle == kInvalid || (impl->identity = Stamp(*impl->root, true)).empty() || !impl->Current())
	{
		error = "Cannot retain the stopped data root.";
		return {};
	}
	return std::shared_ptr<DataFolderTree>(new DataFolderTree(std::move(impl)));
}
bool uam::platform::DataFolderTree::Current() const
{
	return m_impl->Current();
}
std::string uam::platform::DataFolderTree::Identity() const
{
	return m_impl->identity;
}
fs::path uam::platform::DataFolderTree::RootPath() const
{
	return m_impl->root->path;
}
bool uam::platform::DataFolderTree::Scan(Inventory& entries, std::string& error) const
{
	entries.clear();
	std::size_t remaining = 100000;
	if (!Current() || !m_impl->Walk({}, entries, 0, remaining))
	{
		error = "Inventory changed, exceeded its bounds or contains an unreadable, linked or special entry.";
		return false;
	}
	return true;
}
bool uam::platform::DataFolderTree::MakeDirectory(const fs::path& relative, std::string& error) const
{
	if (m_impl->Directory(relative, true))
		return true;
	error = "Cannot create a plain destination directory.";
	return false;
}
bool uam::platform::DataFolderTree::Read(const fs::path& relative, std::size_t limit, std::string& bytes, std::string& error, std::shared_ptr<const ReadWitness>* retained) const
{
	bytes.clear();
	if (retained)
		retained->reset();
	auto file = m_impl->File(relative, false);
	if (file && m_impl->read_observer)
		m_impl->read_observer(relative);
	std::uint64_t size = 0;
	const auto stamp = file ? Stamp(*file, false, &size) : std::string{};
	if (stamp.empty() || size > limit ||
	    !Consume(*file,
	             [&](std::string_view part)
	             {
		             if (part.size() > limit - bytes.size())
			             return false;
		             bytes.append(part);
		             return true;
	             }) ||
	    Stamp(*file, false) != stamp || !Named(*file) || !Current())
	{
		bytes.clear();
		error = "Cannot read a bounded retained regular record.";
		return false;
	}
	if (retained)
	{
		const auto named = m_impl->named;
		const auto root = m_impl->root;
		const auto identity = m_impl->identity;
		*retained = std::shared_ptr<const ReadWitness>(new ReadWitness([file, named, root, identity, stamp]()
		{
			return named->MatchesDirectory(root->handle) && named->Current() && Stamp(*root, true) == identity && Named(*file) && Stamp(*file, false) == stamp;
		}, file->parents.empty() ? std::string{} : Stamp(*file->parents.front(), true)));
	}
	return true;
}
bool uam::platform::DataFolderTree::CopyFile(const Entry& entry, const DataFolderTree& destination, std::string& error) const
{
	auto source = m_impl->File(entry.relative, false);
	if (!source || Stamp(*source, false) != entry.object)
	{
		error = "Source publication changed before copy.";
		return false;
	}
	auto target = destination.m_impl->File(entry.relative, true);
	if (!target)
	{
		error = "Destination entry already exists or cannot be created without following links.";
		return false;
	}
	IncrementalSha256 hash;
	std::uint64_t read = 0;
	if (!Consume(*source,
	             [&](std::string_view bytes)
	             {
		             if (bytes.size() > entry.bytes - read)
			             return false;
		             read += bytes.size();
		             return hash.Add(bytes) && WriteAll(target->handle, bytes);
	             }) ||
	    read != entry.bytes || hash.Finish() != entry.sha256 || Stamp(*source, false) != entry.object || !Current() || !destination.Current())
	{
		error = "Copy verification failed. The destination remains passive.";
		return false;
	}
#if !defined(_WIN32)
	if (fchmod(target->handle, entry.mode) != 0)
	{
		error = "Cannot preserve destination file permissions.";
		return false;
	}
#endif
	if (!PreserveModified(*source, *target, entry.modified) || !Sync(target->handle) || Modified(*target) != entry.modified || !Named(*target) || !Named(*source))
	{
		error = "Destination file durability or directory ownership is unconfirmed.";
		return false;
	}
#if !defined(_WIN32)
	if (target->parents.empty() || !Sync(target->parents.front()->handle))
	{
		error = "Destination entry durability is unconfirmed.";
		return false;
	}
#endif
	return true;
}
uam::platform::DataFolderTree::Publication uam::platform::DataFolderTree::Publish(const fs::path& relative, const std::string& bytes, const WriteGate& gate) const
{
	Publication result;
	if (!Safe(relative) || !Current())
	{
		result.error = "Publication directory changed.";
		return result;
	}
	const auto parent = m_impl->Directory(relative.parent_path(), true);
	if (!parent)
	{
		result.error = "Cannot retain publication directory.";
		return result;
	}
#if defined(_WIN32)
	const auto written = io::AtomicWriteFileDetailed(parent->path / relative.filename(), bytes, [&](io::AtomicWriteStage stage) { return (!gate || gate(stage)) && Current() && !Stamp(*parent, true).empty(); });
	result.confirmed = written.success && Current();
	result.replaced = written.primary_committed;
	result.error = written.error;
#else
	const fs::path temporary = io::MakeTempWritePath(relative.filename());
	const int handle = openat(parent->handle, temporary.c_str(), O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY | O_CLOEXEC, 0600);
	if (handle < 0)
	{
		result.error = "Cannot create private publication temporary.";
		return result;
	}
	const bool synced = WriteAll(handle, bytes) && Sync(handle);
	close(handle);
	if (!synced || (gate && !gate(io::AtomicWriteStage::BeforeReplace)) || !Current() || !Named(*parent))
	{
		result.error = "Publication preparation is unconfirmed; temporary retained.";
		return result;
	}
	if (renameat(parent->handle, temporary.c_str(), parent->handle, relative.filename().c_str()) != 0)
	{
		result.error = "Publication rename failed; temporary retained.";
		return result;
	}
	result.replaced = true;
	if ((gate && !gate(io::AtomicWriteStage::AfterReplaceBeforeDirectorySync)) || !Current() || !Named(*parent) || !Sync(parent->handle))
	{
		result.error = "Publication directory durability is unconfirmed.";
		return result;
	}
	result.confirmed = true;
#endif
	return result;
}
std::string uam::platform::DataFolderTree::Digest(const Inventory& entries)
{
	IncrementalSha256 hash;
	for (const auto& entry : entries)
	{
		const std::string path = paths::PortablePathString(entry.relative);
		const std::string row = std::to_string(path.size()) + ":" + path + (entry.directory ? "D" : "F") + ":" + std::to_string(entry.bytes) + ":" + std::to_string(entry.mode) + ":" + entry.sha256 + ":" + entry.modified + "\n";
		if (!hash.Add(row))
			return {};
	}
	return hash.Finish();
}

std::string uam::platform::DataFolderTree::ObjectDigest(const Inventory& entries)
{
	IncrementalSha256 hash;
	if (!hash.Add(Digest(entries)))
		return {};
	for (const auto& entry : entries)
	{
		if (!hash.Add(std::to_string(entry.object.size()) + ":" + entry.object))
			return {};
	}
	return hash.Finish();
}
