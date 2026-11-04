#pragma once

#include <string_view>

namespace uam::message_attachment_json
{
	inline constexpr std::string_view kIdField = "id";
	inline constexpr std::string_view kNameField = "name";
	inline constexpr std::string_view kKindField = "kind";
	inline constexpr std::string_view kPathField = "path";
	inline constexpr std::string_view kCopiedField = "copied";
	inline constexpr std::string_view kPreviewKeyField = "previewKey";
	inline constexpr std::string_view kPreviewOriginField = "previewOriginPath";
	inline constexpr std::string_view kSourceField = "source";
	inline constexpr std::string_view kSourceUriField = "sourceUri";
	inline constexpr std::string_view kOriginHostField = "originExecutionHostId";
	inline constexpr std::string_view kNativePartKeyField = "nativePartKey";
	inline constexpr std::string_view kNativeToolCallIdField = "nativeToolCallId";
	inline constexpr std::string_view kNativeSourceField = "nativeSourceField";
	inline constexpr std::string_view kNativeAssetKeyField = "nativeAssetKey";
	inline constexpr std::string_view kContentDigestField = "contentDigest";

	namespace persisted
	{
		inline constexpr std::string_view kMimeTypeField = "mime_type";
		inline constexpr std::string_view kSizeBytesField = "size_bytes";
	} // namespace persisted

	namespace frontend
	{
		inline constexpr std::string_view kMimeTypeField = "type";
		inline constexpr std::string_view kSizeBytesField = "size";
		inline constexpr std::string_view kMimeTypeInputField = "mimeType";
		inline constexpr std::string_view kDataBase64Field = "dataBase64";
		inline constexpr std::string_view kAttachmentsField = "attachments";
		inline constexpr std::string_view kItemsField = "items";
		inline constexpr std::string_view kDirectoryKind = "directory";
		inline constexpr std::string_view kImageKind = "image";
		inline constexpr std::string_view kFileKind = "file";
	} // namespace frontend
} // namespace uam::message_attachment_json
