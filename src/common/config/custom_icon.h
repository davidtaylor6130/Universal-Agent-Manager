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
#include <string_view>

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
		std::string bytes;
		if (!uam::io::TryReadTextFile(file, bytes, kMaximumPngBytes) || !HasBoundedPngHeader(bytes)) return {};
		return "data:image/png;base64," + uam::base64::Encode(bytes);
	}

	inline void AddFrontendAsset(nlohmann::json& object, const std::filesystem::path& data_root,
	                             const CustomIcon& icon)
	{
		object["customIcon"] = Serialize(icon);
		if (icon.type == "png" && !object["customIcon"].is_null())
			object["customIcon"]["dataUrl"] = PngDataUrl(data_root, icon);
	}
#endif
}
