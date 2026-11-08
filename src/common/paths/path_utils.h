#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace uam::paths
{
	/// Convert the already decoded absolute path portion of a local file URI.
	/// URI authority, escaping and content validation remain with the reader.
	inline std::filesystem::path PathFromDecodedLocalFileUri(std::string value)
	{
		const bool drive_uri = value.size() > 3 && value[0] == '/' && value[2] == ':' && value[3] == '/' &&
		    ((value[1] >= 'a' && value[1] <= 'z') || (value[1] >= 'A' && value[1] <= 'Z'));
#if defined(_WIN32)
		if (drive_uri)
			value.erase(0, 1);
#else
		// A foreign drive URI does not attest a different POSIX workspace.
		if (drive_uri)
			return {};
#endif
		const auto* begin = reinterpret_cast<const char8_t*>(value.data());
		return std::filesystem::path(std::u8string_view(begin, value.size()));
	}

	inline void StoreError(std::error_code* error_out, const std::error_code& error)
	{
		if (error_out != nullptr)
		{
			*error_out = error;
		}
	}

	inline bool PathExistsNoThrow(const std::filesystem::path& path)
	{
		std::error_code error;
		return std::filesystem::exists(path, error);
	}

	inline bool IsDirectoryNoThrow(const std::filesystem::path& path)
	{
		std::error_code error;
		return std::filesystem::is_directory(path, error);
	}

	inline bool IsRegularFileNoThrow(const std::filesystem::path& path)
	{
		std::error_code error;
		return std::filesystem::is_regular_file(path, error);
	}

	inline bool IsDirectoryEntryNoThrow(const std::filesystem::directory_entry& entry)
	{
		std::error_code error;
		return entry.is_directory(error) && !error;
	}

	inline bool IsRegularFileEntryNoThrow(const std::filesystem::directory_entry& entry)
	{
		std::error_code error;
		return entry.is_regular_file(error) && !error;
	}

	inline bool IsRegularFileWithExtensionNoThrow(const std::filesystem::directory_entry& entry, const std::filesystem::path& extension)
	{
		return IsRegularFileEntryNoThrow(entry) && entry.path().extension() == extension;
	}

	inline std::optional<std::uintmax_t> FileSizeNoThrow(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::uintmax_t size = std::filesystem::file_size(path, error);
		return error ? std::nullopt : std::optional<std::uintmax_t>{size};
	}

	inline bool CreateDirectoriesNoThrow(const std::filesystem::path& path, std::error_code* error_out = nullptr)
	{
		std::error_code error;
		std::filesystem::create_directories(path, error);
		StoreError(error_out, error);
		return !error;
	}

	inline bool RemoveFileNoThrow(const std::filesystem::path& path, std::error_code* error_out = nullptr)
	{
		std::error_code error;
		const bool removed = std::filesystem::remove(path, error);
		StoreError(error_out, error);
		return removed && !error;
	}

	inline bool RemoveAllNoThrow(const std::filesystem::path& path, std::error_code* error_out = nullptr)
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
		StoreError(error_out, error);
		return !error;
	}

	inline bool IsLinkOrReparsePointNoThrow(const std::filesystem::path& path)
	{
		std::error_code error;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, error))) return true;
#if defined(_WIN32)
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
		return false;
#endif
	}

	inline bool RemoveTreeWithoutFollowingLinksNoThrow(const std::filesystem::path& path, std::error_code* error_out = nullptr, int depth = 0)
	{
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
		if (error == std::errc::no_such_file_or_directory)
		{
			StoreError(error_out, {});
			return true;
		}
		if (error || depth > 128)
		{
			StoreError(error_out, error ? error : std::make_error_code(std::errc::too_many_symbolic_link_levels));
			return false;
		}
		if (IsLinkOrReparsePointNoThrow(path) || !std::filesystem::is_directory(status))
		{
			std::filesystem::remove(path, error);
			StoreError(error_out, error);
			return !error;
		}
		for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error))
		{
			if (!RemoveTreeWithoutFollowingLinksNoThrow(it->path(), &error, depth + 1)) break;
		}
		if (!error) std::filesystem::remove(path, error);
		StoreError(error_out, error);
		return !error;
	}

	inline bool RenameNoThrow(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code* error_out = nullptr)
	{
		std::error_code error;
		std::filesystem::rename(from, to, error);
		StoreError(error_out, error);
		return !error;
	}

	inline std::filesystem::path LexicallyNormalPath(const std::filesystem::path& path)
	{
		return path.lexically_normal();
	}

	inline std::filesystem::path AbsolutePathNoThrow(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(path, error);
		return LexicallyNormalPath(error ? path : absolute);
	}

	inline std::optional<std::filesystem::path> CurrentPathNoThrow(std::error_code* error_out = nullptr)
	{
		std::error_code error;
		const std::filesystem::path current = std::filesystem::current_path(error);
		StoreError(error_out, error);
		return error ? std::nullopt : std::optional<std::filesystem::path>{LexicallyNormalPath(current)};
	}

	inline std::filesystem::path CurrentPathOrDot()
	{
		return CurrentPathNoThrow().value_or(std::filesystem::path("."));
	}

	inline std::filesystem::path CurrentPathOrEmpty()
	{
		return CurrentPathNoThrow().value_or(std::filesystem::path{});
	}

	inline std::optional<std::filesystem::path> TempDirectoryPathNoThrow(std::error_code* error_out = nullptr)
	{
		std::error_code error;
		const std::filesystem::path temp_directory = std::filesystem::temp_directory_path(error);
		StoreError(error_out, error);
		return error ? std::nullopt : std::optional<std::filesystem::path>{LexicallyNormalPath(temp_directory)};
	}

	inline std::filesystem::path NormalizeExistingPath(const std::filesystem::path& path)
	{
		std::error_code ec;
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
		return LexicallyNormalPath(ec ? path : canonical);
	}

	inline std::filesystem::path NormalizeExistingOrAbsolutePath(const std::filesystem::path& path)
	{
		std::error_code ec;
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
		return ec ? AbsolutePathNoThrow(path) : LexicallyNormalPath(canonical);
	}

	inline std::string PortablePathString(const std::filesystem::path& path)
	{
		const std::u8string value = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(value.data()), value.size());
	}

	inline std::filesystem::path PathFromUtf8(std::string_view value)
	{
		if (value.empty()) return {};
		const auto* begin = reinterpret_cast<const char8_t*>(value.data());
		return std::filesystem::path(std::u8string(begin, begin + value.size()));
	}

	inline std::string Utf8PathString(const std::filesystem::path& path)
	{
		const std::u8string value = path.u8string();
		return std::string(reinterpret_cast<const char*>(value.data()), value.size());
	}

#if defined(_WIN32)
	inline constexpr bool kHostUsesWindowsPathRules = true;
#else
	inline constexpr bool kHostUsesWindowsPathRules = false;
#endif

	/// <summary>Allowlist for workspace paths from untrusted data (portable bundles): Windows accepts only a
	/// drive letter, ':' and a separator; POSIX accepts only one leading '/' not followed by another separator.
	/// Everything else (UNC, \\?\, \\.\, \??\, drive-relative, rooted-without-drive, URIs, relative, ~) is
	/// rejected, because probing it can make Windows contact a remote SMB server and expose credentials.</summary>
	inline bool IsPlainLocalAbsoluteWorkspacePath(std::string_view path, bool windows_rules = kHostUsesWindowsPathRules)
	{
		const auto separator = [](char value) { return value == '\\' || value == '/'; };
		if (windows_rules)
			return path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
			    path[1] == ':' && separator(path[2]);
		return !path.empty() && path[0] == '/' && (path.size() == 1 || !separator(path[1]));
	}

	inline std::string NormalizedPortablePathString(const std::filesystem::path& path)
	{
		return PortablePathString(LexicallyNormalPath(path));
	}

	inline std::string NormalizedNativePathString(const std::filesystem::path& path)
	{
		return Utf8PathString(LexicallyNormalPath(path));
	}

	inline std::string NormalizeExistingPortablePathString(const std::filesystem::path& path)
	{
		return PortablePathString(NormalizeExistingPath(path));
	}

	inline bool HasExactPathPrefix(const std::filesystem::path& path, const std::filesystem::path& prefix)
	{
		auto path_it = path.begin();
		auto prefix_it = prefix.begin();

		for (; prefix_it != prefix.end(); ++prefix_it)
		{
			if (path_it == path.end() || *path_it != *prefix_it)
			{
				return false;
			}

			++path_it;
		}

		return true;
	}

	struct NormalizedRootCandidate
	{
		std::filesystem::path root;
		std::filesystem::path candidate;
	};

	inline std::optional<NormalizedRootCandidate> NormalizeRootAndCandidate(const std::filesystem::path& root, const std::filesystem::path& candidate)
	{
		if (root.empty() || candidate.empty())
		{
			return std::nullopt;
		}

		return NormalizedRootCandidate{
		    .root = NormalizeExistingPath(root),
		    .candidate = NormalizeExistingPath(candidate),
		};
	}

	inline bool IsSameOrInsideRoot(const std::filesystem::path& root, const std::filesystem::path& candidate)
	{
		const std::optional<NormalizedRootCandidate> normalized = NormalizeRootAndCandidate(root, candidate);
		if (!normalized)
		{
			return false;
		}

		return HasExactPathPrefix(normalized->candidate, normalized->root);
	}

	inline std::optional<std::filesystem::path> RelativePathIfInsideRoot(const std::filesystem::path& root, const std::filesystem::path& candidate)
	{
		const std::optional<NormalizedRootCandidate> normalized = NormalizeRootAndCandidate(root, candidate);
		if (!normalized)
		{
			return std::nullopt;
		}

		if (!HasExactPathPrefix(normalized->candidate, normalized->root))
		{
			return std::nullopt;
		}

		std::error_code ec;
		const std::filesystem::path relative = std::filesystem::relative(normalized->candidate, normalized->root, ec);
		return ec || relative.empty() ? std::nullopt : std::optional<std::filesystem::path>{relative};
	}

	struct VcsMarkers
	{
		bool git = false;
		bool svn = false;
		/// Set by a probe to end the walk early, for example on an unreadable marker. The root stays empty.
		bool stop = false;
	};

	struct NearestVcsRoot : VcsMarkers
	{
		std::filesystem::path root;
	};

	/// GitOrSvn ends the walk at the first .git or .svn. GitOnly ends it only at Git, passing through SVN checkouts;
	/// the result's svn flag then reports whether any SVN marker was passed on the way.
	enum class VcsRootKinds { GitOrSvn, GitOnly };

	inline VcsMarkers ProbeVcsMarkers(const std::filesystem::path& directory)
	{
		return {PathExistsNoThrow(directory / ".git"), PathExistsNoThrow(directory / ".svn")};
	}

	/// The nearest directory holding a marker of the requested kinds owns the path, so by default Git nested in an SVN
	/// checkout is Git. A custom probe decides what one directory reports, for callers with stricter rules or their own early stop.
	template <typename Probe = decltype(&ProbeVcsMarkers)>
	inline NearestVcsRoot FindNearestVcsRoot(const std::filesystem::path& start, VcsRootKinds kinds = VcsRootKinds::GitOrSvn, Probe probe = &ProbeVcsMarkers)
	{
		NearestVcsRoot result;
		for (std::filesystem::path directory = start; !directory.empty(); directory = directory.parent_path())
		{
			const VcsMarkers seen = probe(directory);
			result.git = seen.git;
			result.svn = result.svn || seen.svn;
			result.stop = seen.stop;
			if (seen.stop) return result;
			if (seen.git || (seen.svn && kinds == VcsRootKinds::GitOrSvn))
			{
				result.root = directory;
				return result;
			}
			if (directory == directory.parent_path()) break;
		}
		return result;
	}
} // namespace uam::paths
