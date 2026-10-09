#pragma once
#include "common/models/app_models.h"
#include <optional>
#include <filesystem>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

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

	/// Identity and storage metadata captured before an asynchronous preview lookup.
	struct ChatSnapshot
	{
		std::string chat_id;
		std::string execution_host_id;
		std::string native_identity_key;
		std::size_t message_count = 0;
		std::string messages_digest;
		bool messages_loaded = false;
	};

	struct AttachmentSnapshot
	{
		MessageAttachment attachment;
	};

	ChatSnapshot CaptureChatSnapshot(const ChatSession& chat);
	/// Resolve one attachment from saved history without hydrating or adopting into the live chat.
	std::optional<AttachmentSnapshot> LoadPersistedAttachmentSnapshot(
	    const std::filesystem::path& data_root, const ChatSnapshot& chat, std::size_t message_index,
	    std::string_view attachment_id);
	/// Revalidate async completion against the current live chat, including loaded message changes.
	bool MatchesCurrentAttachmentSnapshot(const ChatSession& current, const ChatSnapshot& original,
	    std::size_t message_index, std::string_view attachment_id, const AttachmentSnapshot& snapshot);

	/// Bounded OS decoder. Only PNG/JPEG first frames are admitted; EXIF orientation is applied.
	Image Decode(std::string_view source);
	struct DeliveryImage
	{
		std::string bytes;
		unsigned width = 0;
		unsigned height = 0;
		std::string mime_type;
		bool animated = false;
	};

	/// Validate with the bounded OS decoder. Preserve fitting originals; resize only to fit delivery limits.
	DeliveryImage PrepareDeliveryImage(std::string_view source, std::size_t max_bytes, std::stop_token cancel = {});
	std::string ImageMime(std::string_view source);
	/// Read a bounded regular local image without following its final symlink.
	bool ReadDeliverySource(const std::filesystem::path& path, std::string& bytes, std::size_t max_bytes = kMaxSourceBytes);
	/// Fast metadata/header preflight. Pixel decoding belongs to the delivery worker.
	bool ValidateImageAttachmentFiles(const std::filesystem::path& workspace,
	    const std::vector<MessageAttachment>& attachments, std::string& error);

	std::string Digest(std::string_view bytes);
	bool IsKey(std::string_view key);

	/// Cache an immutable thumbnail from bytes already owned by attachment staging. Failure is optional.
	std::string Store(const std::filesystem::path& data_root, std::string_view source);
	/// Resolve a native saved attachment, never a renderer-supplied filesystem path or execution host.
	Image Read(const std::filesystem::path& data_root, const MessageAttachment& attachment, bool local_host);
}
