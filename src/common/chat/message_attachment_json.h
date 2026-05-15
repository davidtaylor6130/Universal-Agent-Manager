#pragma once

#include <string_view>

namespace uam::message_attachment_json
{
	inline constexpr std::string_view kIdField = "id";
	inline constexpr std::string_view kNameField = "name";
	inline constexpr std::string_view kKindField = "kind";
	inline constexpr std::string_view kPathField = "path";
	inline constexpr std::string_view kCopiedField = "copied";

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
