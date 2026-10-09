#pragma once
#include "common/models/app_models.h"
#include <filesystem>
#include <string>
#include <string_view>

namespace uam::attachment_preview
{
	inline constexpr std::size_t kMaxSourceBytes = 25 * 1024 * 1024;
	inline constexpr std::size_t kMaxPreviewBytes = 512 * 1024;
	inline constexpr unsigned kMaxSourceSide = 16384;
	inline constexpr unsigned kMaxSourcePixels = 24 * 1000 * 1000;
	inline constexpr unsigned kMaxPreviewSide = 1024;

	struct Image
	{
		std::string png;
		unsigned width = 0;
		unsigned height = 0;
	};

	/// Bounded OS decoder. Only PNG/JPEG first frames are admitted; EXIF orientation is applied.
	Image Decode(std::string_view source);
	std::string Digest(std::string_view bytes);
	bool IsKey(std::string_view key);

	/// Cache an immutable thumbnail from bytes already owned by attachment staging. Failure is optional.
	std::string Store(const std::filesystem::path& data_root, std::string_view source);
	/// Resolve a native saved attachment, never a renderer-supplied filesystem path or execution host.
	Image Read(const std::filesystem::path& data_root, const MessageAttachment& attachment, bool local_host);
}
