#pragma once

#include "common/models/app_models.h"
#include "common/utils/base64.h"
#if defined(_WIN32) || defined(__APPLE__)
#include "common/utils/io_utils.h"
#endif
#include "common/utils/uuid.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace uam::icons
{
	inline constexpr std::size_t kMaximumPngBytes = 256 * 1024;

	/// <summary>Rejects executable markup, control characters, and nonportable asset paths.</summary>
	inline bool IsValid(const CustomIcon& icon)
	{
		if (icon.type.empty()) return icon.value.empty();
		if (icon.type == "png") return icon.value.size() == 40 && icon.value.ends_with(".png") &&
		    uam::uuid::IsCanonicalUuid(std::string_view(icon.value).substr(0, 36));
		if (icon.type != "text" || icon.value.empty() || icon.value.size() > 64) return false;
		return std::ranges::none_of(icon.value, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
	}

	inline nlohmann::json Serialize(const CustomIcon& icon)
	{
		return IsValid(icon) && !icon.type.empty()
		    ? nlohmann::json{{"type", icon.type}, {"value", icon.value}} : nlohmann::json(nullptr);
	}

	inline CustomIcon Parse(const nlohmann::json& value)
	{
		if (!value.is_object() || !value.contains("type") || !value["type"].is_string() ||
		    !value.contains("value") || !value["value"].is_string()) return {};
		const CustomIcon icon{value["type"].get<std::string>(), value["value"].get<std::string>()};
		return IsValid(icon) ? icon : CustomIcon{};
	}

	/// <summary>Bounds PNG dimensions before passing bytes to Chromium's decoder.</summary>
	inline bool HasBoundedPngHeader(std::string_view bytes)
	{
		if (bytes.size() < 33 || bytes.size() > kMaximumPngBytes ||
		    bytes.substr(0, 8) != std::string_view("\x89PNG\r\n\x1a\n", 8) ||
		    bytes.substr(8, 8) != std::string_view("\0\0\0\rIHDR", 8)) return false;
		const std::function<std::uint32_t(std::size_t)> read = [&bytes](std::size_t offset) -> std::uint32_t
		{
			std::uint32_t value = 0;
			for (std::size_t i = offset; i < offset + 4; ++i)
				value = (value << 8) | static_cast<unsigned char>(bytes[i]);
			return value;
		};
		const std::uint32_t width = read(16);
		const std::uint32_t height = read(20);
		return width > 0 && height > 0 && width <= 256 && height <= 256;
	}

#if defined(_WIN32) || defined(__APPLE__)
	/// <summary>Reads only bounded, regular assets inside the owned icons directory.</summary>
	inline std::string PngDataUrl(const std::filesystem::path& data_root, const CustomIcon& icon)
	{
		if (icon.type != "png" || !IsValid(icon)) return {};
		std::error_code error;
		const std::filesystem::path directory = data_root / "icons";
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error)) || error) return {};
		const std::filesystem::path file = directory / icon.value;
		const std::filesystem::file_status status = std::filesystem::symlink_status(file, error);
		if (error || !std::filesystem::is_regular_file(status)) return {};
		const std::uintmax_t size = std::filesystem::file_size(file, error);
		if (error) return {};
		const std::filesystem::file_time_type modified = std::filesystem::last_write_time(file, error);
		if (error) return {};
		// State fingerprints run every streaming tick; reuse the encoded asset until its file changes.
		struct CachedPng
		{
			std::uintmax_t size = 0;
			std::filesystem::file_time_type modified;
			std::string data_url;
		};
		static std::mutex cache_mutex;
		static std::unordered_map<std::string, CachedPng> cache;
		const std::string key = file.string();
		{
			const std::lock_guard<std::mutex> lock(cache_mutex);
			if (const auto found = cache.find(key); found != cache.end() && found->second.size == size && found->second.modified == modified)
				return found->second.data_url;
		}
		std::string bytes;
		if (!uam::io::TryReadTextFile(file, bytes, kMaximumPngBytes) || !HasBoundedPngHeader(bytes)) return {};
		std::string data_url = "data:image/png;base64," + uam::base64::Encode(bytes);
		const std::lock_guard<std::mutex> lock(cache_mutex);
		if (cache.size() >= 256) cache.clear(); // ponytail: wholesale reset bounds stale entries; LRU only if icon counts grow.
		cache[key] = CachedPng{size, modified, data_url};
		return data_url;
	}

	inline void AddFrontendAsset(nlohmann::json& object, const std::filesystem::path& data_root,
	                             const CustomIcon& icon)
	{
		object["customIcon"] = Serialize(icon);
		if (icon.type == "png" && !object["customIcon"].is_null())
			object["customIcon"]["dataUrl"] = PngDataUrl(data_root, icon);
	}

	/// <summary>Bounds an ICO: 1-64 images, each inside the file, and embedded PNGs within the PNG pixel bounds.</summary>
	inline bool HasBoundedIcoHeader(std::string_view bytes)
	{
		if (bytes.size() < 22 || bytes.size() > kMaximumPngBytes || bytes.substr(0, 4) != std::string_view("\0\0\1\0", 4)) return false;
		const auto read = [&bytes](std::size_t offset, std::size_t width) -> std::size_t
		{
			std::size_t value = 0;
			for (std::size_t i = width; i-- > 0;)
				value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
			return value;
		};
		const std::size_t count = read(4, 2);
		const std::size_t directory_end = 6 + count * 16;
		if (count == 0 || count > 64 || bytes.size() < directory_end) return false;
		for (std::size_t entry = 6; entry < directory_end; entry += 16)
		{
			const std::size_t size = read(entry + 8, 4);
			const std::size_t offset = read(entry + 12, 4);
			if (offset < directory_end || offset > bytes.size() || size > bytes.size() - offset) return false;
			// Chromium picks PNG from the bytes at the offset, whatever the declared size, and decodes the largest image,
			// so an embedded PNG needs the same pixel bound as a plain one.
			if (bytes.substr(offset).starts_with("\x89PNG") && !HasBoundedPngHeader(bytes.substr(offset, size))) return false;
		}
		return true;
	}

	/// <summary>Accepts a bounded SVG document; it is only shown through an image element, where scripts never run.</summary>
	inline bool IsBoundedSvg(std::string_view bytes)
	{
		return !bytes.empty() && bytes.size() <= kMaximumPngBytes && bytes.substr(0, 1024).find("<svg") != std::string_view::npos;
	}

	/// <summary>Finds a website or app icon (PNG, SVG or ICO) in a local workspace, or returns empty when none fits the custom-icon bounds.</summary>
	inline std::string ProjectIconDataUrl(const std::filesystem::path& workspace)
	{
		// Common favicon, web-app, desktop and mobile launcher locations, sharpest formats first; the first bounded icon wins.
		static constexpr std::string_view kCandidates[] = {
		    "public/favicon.svg", "public/apple-touch-icon.png", "public/favicon.png", "public/icon.png",
		    "public/icon.svg", "public/logo192.png", "public/favicon-96x96.png", "public/favicon-32x32.png",
		    "static/favicon.svg", "static/apple-touch-icon.png", "static/favicon.png", "app/apple-icon.png",
		    "app/icon.svg", "app/icon.png", "src/app/apple-icon.png", "src/app/icon.svg", "src/app/icon.png",
		    "web/favicon.png", "web/icons/Icon-192.png", "apple-touch-icon.png", "favicon.svg", "favicon.png",
		    "icon.png", "assets/icon.png", "src-tauri/icons/128x128.png", "build/icon.png", "resources/icon.png",
		    "android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png", "app/src/main/res/mipmap-xxxhdpi/ic_launcher.png",
		    "public/favicon.ico", "static/favicon.ico", "app/favicon.ico", "src/app/favicon.ico", "favicon.ico"};
		if (workspace.empty()) return {};
		// Folder state serializes on many pushes; probe each workspace once per run.
		// ponytail: icons added or changed mid-run show after restart; add an mtime check if that matters.
		static std::mutex cache_mutex;
		static std::unordered_map<std::string, std::string> cache;
		const std::string key = workspace.string();
		{
			const std::lock_guard<std::mutex> lock(cache_mutex);
			if (const auto found = cache.find(key); found != cache.end()) return found->second;
		}
		std::string data_url;
		for (const std::string_view candidate : kCandidates)
		{
			std::error_code error;
			const std::filesystem::path file = workspace / std::filesystem::path(candidate);
			if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file, error)) || error) continue;
			std::string bytes;
			if (!uam::io::TryReadTextFile(file, bytes, kMaximumPngBytes)) continue;
			const char* mime = candidate.ends_with(".svg") ? (IsBoundedSvg(bytes) ? "image/svg+xml" : nullptr)
			    : candidate.ends_with(".ico") ? (HasBoundedIcoHeader(bytes) ? "image/x-icon" : nullptr)
			    : (HasBoundedPngHeader(bytes) ? "image/png" : nullptr);
			if (mime)
			{
				data_url = std::string("data:") + mime + ";base64," + uam::base64::Encode(bytes);
				break;
			}
		}
		const std::lock_guard<std::mutex> lock(cache_mutex);
		if (cache.size() >= 256) cache.clear();
		cache[key] = data_url;
		return data_url;
	}
#endif
}
