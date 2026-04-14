#pragma once

#include "cef/cef_includes.h"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#if defined(_WIN32)
	#include <shellapi.h>
	#include <windows.h>
#endif

namespace uam::cef
{
inline std::string ToLowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
	{
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

inline std::string StripUrlQueryAndFragment(std::string url)
{
	const std::size_t query_pos    = url.find('?');
	const std::size_t fragment_pos = url.find('#');
	const std::size_t cut_pos = std::min(query_pos, fragment_pos);

	if (cut_pos != std::string::npos)
		url.resize(cut_pos);

	return url;
}

inline int HexDigitValue(const char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return 10 + (ch - 'a');
	if (ch >= 'A' && ch <= 'F')
		return 10 + (ch - 'A');
	return -1;
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
			const int hi = HexDigitValue(value[i + 1]);
			const int lo = HexDigitValue(value[i + 2]);
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
		return {};

	return ToLowerAscii(url.substr(0, colon_pos));
}

inline std::string NormalizeFileUrlForComparison(const std::string& url)
{
	std::string stripped = StripUrlQueryAndFragment(url);
	if (UrlScheme(stripped) != "file")
	{
		return stripped;
	}

	const std::string prefix = "file://";
	if (stripped.rfind(prefix, 0) != 0)
	{
		return stripped;
	}

	std::string path = PercentDecode(stripped.substr(prefix.size()));
	if (path.rfind("localhost/", 0) == 0)
	{
		path.erase(0, std::string("localhost").size());
	}

#if defined(_WIN32)
	if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
	{
		path.erase(0, 1);
	}
#endif

	std::filesystem::path fs_path(path);
	std::error_code ec;
	const std::filesystem::path normalized = std::filesystem::weakly_canonical(fs_path, ec);
	const std::filesystem::path final_path = ec ? fs_path.lexically_normal() : normalized.lexically_normal();
	return "file://" + final_path.generic_string();
}

inline bool IsTrustedUiUrl(const std::string& url, const std::string& trusted_index_url)
{
	const std::string stripped_url = StripUrlQueryAndFragment(url);
	const std::string stripped_trusted = StripUrlQueryAndFragment(trusted_index_url);

	if (UrlScheme(stripped_url) == "file" && UrlScheme(stripped_trusted) == "file")
	{
		return NormalizeFileUrlForComparison(stripped_url) == NormalizeFileUrlForComparison(stripped_trusted);
	}

	return stripped_url == stripped_trusted;
}

inline bool ShouldOpenExternally(const std::string& url)
{
	const std::string scheme = UrlScheme(url);
	return scheme == "http" ||
	       scheme == "https" ||
	       scheme == "mailto" ||
	       scheme == "ftp" ||
	       scheme == "tel";
}

inline std::string ShellEscapeSingleQuoted(const std::string& value)
{
	std::string out;
	out.reserve(value.size() + 2);
	out.push_back('\'');
	for (const char ch : value)
	{
		if (ch == '\'')
			out += "'\\''";
		else
			out.push_back(ch);
	}
	out.push_back('\'');
	return out;
}

inline bool OpenUrlExternally(const std::string& url, std::string* error_out = nullptr)
{
#if defined(_WIN32)
	int wide_len = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
	if (wide_len <= 0)
	{
		if (error_out != nullptr)
			*error_out = "Failed to convert URL to UTF-16.";
		return false;
	}

	std::wstring wide_url(static_cast<std::size_t>(wide_len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wide_url.data(), wide_len);
	wide_url.resize(static_cast<std::size_t>(wide_len - 1));
	const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<std::uintptr_t>(result) <= 32)
	{
		if (error_out != nullptr)
			*error_out = "ShellExecuteW failed to open URL.";
		return false;
	}
	return true;
#elif defined(__APPLE__)
	const std::string command = "/usr/bin/open " + ShellEscapeSingleQuoted(url);
	const int rc = std::system(command.c_str());
	if (rc != 0)
	{
		if (error_out != nullptr)
			*error_out = "The macOS open command failed.";
		return false;
	}
	return true;
#else
	(void)url;
	if (error_out != nullptr)
		*error_out = "Opening external URLs is unsupported on this platform.";
	return false;
#endif
}

inline std::string FileUrlFromPath(const std::filesystem::path& path)
{
	std::error_code ec;
	const auto normalized = std::filesystem::weakly_canonical(path, ec);
	const auto final_path = ec ? std::filesystem::absolute(path) : normalized;
	return "file://" + final_path.generic_string();
}

inline std::string ResolveTrustedUiIndexUrl(const std::filesystem::path& exe_dir)
{
	for (std::filesystem::path current = exe_dir; !current.empty(); current = current.parent_path())
	{
		const auto resources_dist = current / "Resources" / "UI-V2" / "dist" / "index.html";
		if (std::filesystem::exists(resources_dist))
		{
			return FileUrlFromPath(resources_dist);
		}

		const auto flat_dist = current / "UI-V2" / "dist" / "index.html";
		if (std::filesystem::exists(flat_dist))
		{
			return FileUrlFromPath(flat_dist);
		}
	}

	return FileUrlFromPath(std::filesystem::absolute("UI-V2/dist/index.html"));
}

inline std::string ResolveTrustedUiIndexUrl()
{
	CefString exe_dir_str;
	if (CefGetPath(PK_DIR_EXE, exe_dir_str))
	{
		return ResolveTrustedUiIndexUrl(std::filesystem::path(exe_dir_str.ToString()));
	}

	return ResolveTrustedUiIndexUrl(std::filesystem::absolute("."));
}
} // namespace uam::cef
