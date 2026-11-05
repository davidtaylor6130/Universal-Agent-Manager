#pragma once

#include "common/paths/path_utils.h"
#include "common/utils/string_utils.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

/// Agent-published HTML views: one self-contained page stored as a dedicated message block.
/// The page is display content only; it never grants bridge, file, network or chat authority.
namespace uam::html_view
{
	inline constexpr std::string_view kBlockType = "html_view";
	inline constexpr std::size_t kMaxHtmlBytes = 128U * 1024U;
	inline constexpr std::size_t kMaxTitleBytes = 120U;
	/// Interim default (open decision): views one turn may publish, which also bounds what read-only publishing can add.
	inline constexpr std::size_t kMaxViewsPerTurn = 8U;
	/// Envelope for the publish/preview control requests: JSON escaping can roughly double the page.
	inline constexpr std::size_t kMaxRequestBytes = 1024U * 1024U;
	/// Same policy as the sandboxed frame host; also prepended to copies opened outside UAM.
	inline constexpr std::string_view kContentSecurityPolicy =
	    "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
	    "img-src data: blob:; font-src data:; media-src data: blob:; connect-src 'none'; "
	    "frame-src 'none'; child-src 'none'; worker-src 'none'; object-src 'none'; "
	    "form-action 'none'; base-uri 'none'";

	/// <summary>Empty when the view can be published; otherwise a user-readable rejection.</summary>
	inline std::string ValidationError(std::string_view title, std::string_view html)
	{
		if (uam::strings::IsBlank(std::string(title))) return "HTML view title is required.";
		if (title.size() > kMaxTitleBytes) return "HTML view title exceeds " + std::to_string(kMaxTitleBytes) + " bytes.";
		for (const char ch : title)
			if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) return "HTML view title must be a single line of text.";
		if (title.find("\xE2\x80\xA8") != std::string_view::npos || title.find("\xE2\x80\xA9") != std::string_view::npos)
			return "HTML view title must be a single line of text."; // U+2028 LINE SEPARATOR, U+2029 PARAGRAPH SEPARATOR
		if (uam::strings::IsBlank(std::string(html))) return "HTML view document is empty.";
		if (html.size() > kMaxHtmlBytes)
			return "HTML view document is " + std::to_string(html.size()) + " bytes; the limit is " + std::to_string(kMaxHtmlBytes) + " bytes. Inline less data or simplify the page.";
		if (html.find('\0') != std::string_view::npos) return "HTML view document contains a NUL byte.";
		return {};
	}

	/// <summary>Static checks for content the sandbox will block, so the agent can fix it before publishing.</summary>
	/// ponytail: substring heuristics, not a renderer; add a real offscreen render if agents need runtime errors.
	inline std::vector<std::string> Diagnostics(std::string_view html)
	{
		const std::string lower = uam::strings::ToLowerAscii(std::string(html));
		std::vector<std::string> out;
		const auto has = [&](std::string_view needle) { return lower.find(needle) != std::string::npos; };
		const auto any = [&](std::initializer_list<std::string_view> needles)
		{
			for (std::string_view needle : needles)
				if (has(needle)) return true;
			return false;
		};
		if (any({"src=\"http", "src='http", "src=http", "src=\"//", "src='//", "href=\"http", "href='http", "href=\"//", "url(http", "url(\"http", "url('http", "url(//", "@import"}))
			out.push_back("Remote resources (scripts, styles, images, fonts) are blocked: network access is off. Inline them or use data: URLs.");
		if (any({"fetch(", "xmlhttprequest", "websocket", "eventsource", "sendbeacon", "rtcpeerconnection"}))
			out.push_back("Network APIs (fetch, XMLHttpRequest, WebSocket, EventSource, sendBeacon, RTCPeerConnection) are blocked inside the view.");
		if (any({"<iframe", "<frame", "<object", "<embed"}))
			out.push_back("Frames loaded from a URL, objects and embeds are blocked inside the view; only an inline <iframe srcdoc> works, under the same restrictions.");
		if (any({"<form"}))
			out.push_back("Form submission is blocked; handle input with inline script instead.");
		if (any({"localstorage", "sessionstorage", "indexeddb", "document.cookie"}))
			out.push_back("Storage and cookies are unavailable: the view runs in an opaque sandboxed origin.");
		if (any({"window.open", "target=\"_blank\"", "target='_blank'", "top.location", "parent.location"}))
			out.push_back("Pop-ups and top-level navigation are blocked; links cannot leave the view.");
		if (any({"new worker(", "serviceworker", "new sharedworker("}))
			out.push_back("Workers are blocked inside the view.");
		if (!has("<html") && !has("<body") && !has("<!doctype"))
			out.push_back("The document has no <!doctype html>, <html> or <body>; it will still render but a complete document is more predictable.");
		return out;
	}

	/// <summary>Copy for a browser outside the sandbox, with the same restrictive policy kept in front.</summary>
	inline std::string DocumentWithPolicy(std::string_view html)
	{
		// The copy is always UTF-8; declaring it first keeps the page's own charset tag from being pushed past the browser's scan.
		const std::string meta = "<meta charset=\"utf-8\"><meta http-equiv=\"Content-Security-Policy\" content=\"" +
		                         std::string(kContentSecurityPolicy) + "\">";
		// Only a leading doctype stays first (as prepareHtmlViewDocument does); anything else goes after the policy.
		const std::size_t bom = html.starts_with("\xEF\xBB\xBF") ? 3 : 0;
		std::size_t insert = html.find_first_not_of(" \t\r\n\f", bom);
		if (insert == std::string_view::npos || !uam::strings::StartsWithIgnoreCase(html.substr(insert), "<!doctype"))
			insert = 0;
		else if (const std::size_t end = html.find('>', insert); end != std::string_view::npos)
			insert = end + 1;
		else
			insert = 0;
		std::string out(html.substr(0, insert));
		out += meta;
		out += html.substr(insert);
		return out;
	}

	/// <summary>Creates or checks the temp folder for copies opened in a browser: refuses a link, a non-folder or
	/// (outside Windows) a folder another user owns, then deletes view copies older than a day.</summary>
	/// ponytail: no Windows owner check; %TEMP% is already per user there.
	inline bool PrepareOpenedCopyFolder(const std::filesystem::path& folder,
	                                    std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now())
	{
		if (!uam::paths::CreateDirectoriesNoThrow(folder) || uam::paths::IsLinkOrReparsePointNoThrow(folder) ||
		    !uam::paths::IsDirectoryNoThrow(folder))
			return false;
#if !defined(_WIN32)
		struct stat info{};
		if (::lstat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) return false;
#endif
		std::error_code error;
		for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
		{
			const std::filesystem::directory_entry& entry = *it;
			const std::string name = uam::paths::Utf8PathString(entry.path().filename());
			if (!name.starts_with("view-") || !name.ends_with(".html") || !uam::paths::IsRegularFileEntryNoThrow(entry) ||
			    uam::paths::IsLinkOrReparsePointNoThrow(entry.path()))
				continue;
			std::error_code time_error;
			const auto written = std::filesystem::last_write_time(entry.path(), time_error);
			if (!time_error && now - written > std::chrono::hours(24)) (void)uam::paths::RemoveFileNoThrow(entry.path());
		}
		return true;
	}

	/// <summary>Portable file name for a saved view.</summary>
	inline std::string FileName(std::string_view title)
	{
		std::string out;
		for (const char ch : title)
		{
			const unsigned char byte = static_cast<unsigned char>(ch);
			if (byte < 0x20 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) out.push_back('-');
			else out.push_back(ch);
		}
		while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
		while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
		if (out.empty()) out = "HTML view";
		// Windows reserves these device names with or without an extension, in any case.
		std::string base = uam::strings::ToLowerAscii(out.substr(0, out.find('.')));
		while (!base.empty() && base.back() == ' ') base.pop_back();
		const bool numbered = base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9';
		if (numbered || base == "con" || base == "prn" || base == "aux" || base == "nul") out.insert(out.begin(), '_');
		return out + ".html";
	}
} // namespace uam::html_view
