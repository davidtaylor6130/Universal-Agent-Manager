#pragma once

#include "cef/cef_includes.h"
#include "common/paths/path_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace uam::cef
{
	inline constexpr std::string_view kFileUrlPrefix = "file://";
	inline constexpr std::string_view kLocalhostFilePrefix = "localhost/";
	inline constexpr std::string_view kLocalhostHost = "localhost";

	inline std::string StripUrlQueryAndFragment(std::string url)
	{
		const std::size_t query_pos = url.find('?');
		const std::size_t fragment_pos = url.find('#');
		const std::size_t cut_pos = std::min(query_pos, fragment_pos);

		if (cut_pos != std::string::npos)
		{
			url.resize(cut_pos);
		}

		return url;
	}

	inline std::string PercentDecode(std::string_view value)
	{
		std::string out;
		out.reserve(value.size());

		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const char ch = value[i];
			if (ch == '%' && i + 2 < value.size())
			{
				const int hi = uam::strings::HexDigitValue(static_cast<unsigned char>(value[i + 1]));
				const int lo = uam::strings::HexDigitValue(static_cast<unsigned char>(value[i + 2]));
				if (hi >= 0 && lo >= 0)
				{
					out.push_back(static_cast<char>((hi << 4) | lo));
					i += 2;
					continue;
				}
			}

			out.push_back(ch);
		}

		return out;
	}

	inline std::string UrlScheme(const std::string& url)
	{
		const std::size_t colon_pos = url.find(':');
		if (colon_pos == std::string::npos)
		{
			return {};
		}

		return uam::strings::ToLowerAscii(url.substr(0, colon_pos));
	}

	inline std::string RemoveLocalhostFileAuthority(std::string path)
	{
		if (uam::strings::StartsWithIgnoreCase(path, kLocalhostFilePrefix))
		{
			path.erase(0, kLocalhostHost.size());
		}

		return path;
	}

	inline std::string DecodeFileUrlPathForComparison(const std::string& stripped_file_url)
	{
		if (!uam::strings::StartsWithIgnoreCase(stripped_file_url, kFileUrlPrefix))
		{
			return stripped_file_url;
		}

		return RemoveLocalhostFileAuthority(PercentDecode(stripped_file_url.substr(kFileUrlPrefix.size())));
	}

	inline std::string NormalizeFileUrlForComparison(const std::string& url)
	{
		std::string stripped = StripUrlQueryAndFragment(url);
		if (UrlScheme(stripped) != "file")
		{
			return stripped;
		}

		if (!uam::strings::StartsWithIgnoreCase(stripped, kFileUrlPrefix))
		{
			return stripped;
		}

		std::string path = DecodeFileUrlPathForComparison(stripped);

#if defined(_WIN32)
		if (path.size() >= 3 && path[0] == '/' && uam::strings::IsAsciiAlpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
		{
			path.erase(0, 1);
		}
#endif

		const std::filesystem::path fs_path = uam::paths::PathFromUtf8(path);
		const std::filesystem::path final_path = uam::paths::NormalizeExistingPath(fs_path);
		return "file://" + uam::paths::PortablePathString(final_path);
	}

	inline bool IsTrustedUiUrl(const std::string& url, const std::string& trusted_index_url)
	{
		const std::string stripped_url = StripUrlQueryAndFragment(url);
		const std::string stripped_trusted = StripUrlQueryAndFragment(trusted_index_url);

		if (stripped_url.empty() || stripped_trusted.empty())
		{
			return false;
		}

		if (UrlScheme(stripped_url) == "file" && UrlScheme(stripped_trusted) == "file")
		{
			return NormalizeFileUrlForComparison(stripped_url) == NormalizeFileUrlForComparison(stripped_trusted);
		}

		return stripped_url == stripped_trusted;
	}

	inline bool IsTrustedMainFrame(CefRefPtr<CefFrame> frame, const std::string& trusted_index_url)
	{
		return frame != nullptr && frame->IsMain() && IsTrustedUiUrl(frame->GetURL().ToString(), trusted_index_url);
	}

	inline bool ShouldOpenExternally(const std::string& url)
	{
		constexpr std::array<std::string_view, 5> kExternalUrlSchemes = {
		    "http",
		    "https",
		    "mailto",
		    "ftp",
		    "tel",
		};

		const std::string scheme = UrlScheme(url);
		return uam::ranges::Contains(kExternalUrlSchemes, scheme);
	}

	inline bool OpenUrlExternally(const std::string& url, std::string* error_out = nullptr)
	{
#if defined(_WIN32)
		int wide_len = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
		if (wide_len <= 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to convert URL to UTF-16.";
			}
			return false;
		}

		std::wstring wide_url(static_cast<std::size_t>(wide_len), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wide_url.data(), wide_len);
		wide_url.resize(static_cast<std::size_t>(wide_len - 1));
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<std::uintptr_t>(result) <= 32)
		{
			if (error_out != nullptr)
			{
				*error_out = "ShellExecuteW failed to open URL.";
			}
			return false;
		}
		return true;
#elif defined(__APPLE__)
		const pid_t pid = fork();
		if (pid < 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to launch the macOS open command.";
			}
			return false;
		}

		if (pid == 0)
		{
			execl("/usr/bin/open", "open", url.c_str(), static_cast<char*>(nullptr));
			_exit(127);
		}

		int status = 0;
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			if (error_out != nullptr)
			{
				*error_out = "Failed to wait for the macOS open command.";
			}
			return false;
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "The macOS open command failed.";
			}
			return false;
		}
		return true;
#else
		(void)url;
		if (error_out != nullptr)
		{
			*error_out = "Opening external URLs is unsupported on this platform.";
		}
		return false;
#endif
	}

	inline std::string FileUrlFromPath(const std::filesystem::path& path)
	{
		const std::filesystem::path final_path = uam::paths::NormalizeExistingOrAbsolutePath(path);
		return "file://" + std::string(final_path.has_root_name() ? "/" : "") + uam::paths::PortablePathString(final_path);
	}

	inline std::optional<std::filesystem::path> FindTrustedUiIndexPath(const std::filesystem::path& exe_dir)
	{
		std::filesystem::path current = exe_dir;
		while (!current.empty())
		{
			const auto resources_dist = current / "Resources" / "UI-V2" / "dist" / "index.html";
			if (uam::paths::PathExistsNoThrow(resources_dist))
			{
				return resources_dist;
			}

			const auto flat_dist = current / "UI-V2" / "dist" / "index.html";
			if (uam::paths::PathExistsNoThrow(flat_dist))
			{
				return flat_dist;
			}

			const std::filesystem::path parent = current.parent_path();
			if (parent == current)
			{
				break;
			}
			current = parent;
		}

		return std::nullopt;
	}

	inline std::string ResolveTrustedUiIndexUrl(const std::filesystem::path& exe_dir)
	{
		if (const std::optional<std::filesystem::path> bundled_index = FindTrustedUiIndexPath(exe_dir))
		{
			return FileUrlFromPath(*bundled_index);
		}

		return FileUrlFromPath("UI-V2/dist/index.html");
	}

	inline std::string ResolveTrustedUiIndexUrl()
	{
		CefString exe_dir_str;
		if (CefGetPath(PK_DIR_EXE, exe_dir_str))
		{
			return ResolveTrustedUiIndexUrl(uam::paths::PathFromUtf8(exe_dir_str.ToString()));
		}

		return ResolveTrustedUiIndexUrl(".");
	}
} // namespace uam::cef
