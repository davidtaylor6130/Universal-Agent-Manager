#pragma once

#include "common/chat/native_attachment_assets.h"
#include "common/models/app_models.h"

#include <algorithm>
#include <optional>
#include <string_view>

namespace uam::attachment_sources
{
	inline std::string_view Name(AttachmentSource source)
	{
		switch (source)
		{
		case AttachmentSource::LegacyPath: return "legacy_path";
		case AttachmentSource::NativeInlinePending: return "native_inline_pending";
		case AttachmentSource::OwnedNativeAsset: return "owned_native_asset";
		case AttachmentSource::ProviderUri: return "provider_uri";
		}
		return {};
	}

	inline std::optional<AttachmentSource> Parse(std::string_view source)
	{
		for (const auto value : {AttachmentSource::LegacyPath, AttachmentSource::NativeInlinePending,
		         AttachmentSource::OwnedNativeAsset, AttachmentSource::ProviderUri})
			if (Name(value) == source) return value;
		return std::nullopt;
	}

	inline bool BoundedText(std::string_view value, std::size_t maximum)
	{
		return value.size() <= maximum && std::none_of(value.begin(), value.end(),
		    [](unsigned char c) { return c < 32 || c == 127; });
	}

	inline bool MetadataEqual(const MessageAttachment& lhs, const MessageAttachment& rhs)
	{
		return lhs.source == rhs.source && lhs.source_uri == rhs.source_uri &&
		    lhs.origin_execution_host_id == rhs.origin_execution_host_id &&
		    lhs.native_part_key == rhs.native_part_key && lhs.native_tool_call_id == rhs.native_tool_call_id &&
		    lhs.native_source_field == rhs.native_source_field && lhs.native_asset_key == rhs.native_asset_key &&
		    lhs.content_digest == rhs.content_digest;
	}

	/// No I/O or preparation. Generic saves must never acknowledge a pending
	/// original, a mixed source/path representation, or a foreign owned reference.
	inline bool IsPersistable(const MessageAttachment& value, std::string_view owner = {})
	{
		if (value.source == AttachmentSource::LegacyPath)
			return value.source_uri.empty() && value.origin_execution_host_id.empty() &&
			    value.native_part_key.empty() && value.native_tool_call_id.empty() &&
			    value.native_source_field.empty() && value.native_asset_key.empty() &&
			    value.content_digest.empty() && !value.native_inline_payload;
		if (!value.path.empty() || value.copied || !value.preview_origin_path.empty() ||
		    value.native_inline_payload || value.native_part_key.empty() || value.id.empty() ||
		    !BoundedText(value.id, 16384) || !BoundedText(value.name, 4096) ||
		    !BoundedText(value.mime_type, 255) || value.mime_type.find('/') == std::string::npos ||
		    !BoundedText(value.native_part_key, 16384) || !BoundedText(value.native_tool_call_id, 1024) ||
		    !BoundedText(value.native_source_field, 16384) || !BoundedText(value.origin_execution_host_id, 1024)) return false;
		if (value.source == AttachmentSource::ProviderUri)
			return !value.source_uri.empty() && BoundedText(value.source_uri, 16384) &&
			    value.native_asset_key.empty() && value.content_digest.empty() && value.preview_key.empty() &&
			    value.size_bytes == 0;
		if (value.source == AttachmentSource::OwnedNativeAsset)
			return value.source_uri.empty() && value.size_bytes <= native_assets::kMaxOriginalBytes &&
			    native_assets::IsReferenceValid({value.native_asset_key, value.mime_type,
			        value.content_digest, static_cast<std::size_t>(value.size_bytes)}, owner);
		return false;
	}
}
