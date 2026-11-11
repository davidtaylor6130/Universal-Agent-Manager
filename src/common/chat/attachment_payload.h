#pragma once

#include "common/chat/message_attachment_json.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/base64.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uam::attachment_payload
{
	inline constexpr std::uintmax_t kMaxBytes = 25ull * 1024ull * 1024ull;

	/// Reads one file attachment's bytes from its inline base64 or controller path; shared by local and SSH staging.
	/// A payload that is empty or shorter than its source is refused here, so neither route can stage a hollow file.
	inline bool Load(const nlohmann::json& item, const std::string& source_path_text, std::string& bytes, int& status, std::string& error)
	{
		const auto fail = [&](const int code, std::string message, const std::string_view reason, const std::uintmax_t expected)
		{
			// Sizes and a fixed reason only: names, paths and bytes stay out of the log.
			uam::diagnostics::Write("[stageChatAttachments] Attachment refused before staging: reason=" + std::string(reason) + " expected_bytes=" + std::to_string(expected) + " read_bytes=" + std::to_string(bytes.size()));
			status = code;
			error = std::move(message);
			return false;
		};
		std::optional<std::uintmax_t> expected;
		if (const auto declared = item.find(std::string(message_attachment_json::frontend::kSizeBytesField)); declared != item.end() && declared->is_number_unsigned())
			expected = declared->get<std::uintmax_t>();
		if (const nlohmann::json* data = uam::nlohmann_json::FindStringField(item, message_attachment_json::frontend::kDataBase64Field); data != nullptr)
		{
			if (!uam::base64::Decode(data->get_ref<const std::string&>(), bytes))
				return fail(400, "Attachment data is not valid base64.", "invalid_base64", expected.value_or(0));
		}
		else if (!source_path_text.empty())
		{
			const std::filesystem::path source = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(source_path_text);
			const std::optional<std::uintmax_t> size = uam::paths::FileSizeNoThrow(source);
			if (!size || !uam::paths::IsRegularFileNoThrow(source))
				return fail(400, "File attachment does not exist: " + source_path_text, "source_missing", 0);
			if (*size > kMaxBytes)
				return fail(413, "Attachment is larger than the 25 MB limit.", "over_limit", *size);
			expected = *size; // The file on disk is the authority for a path attachment.
			if (!uam::io::TryReadBinaryFile(source, bytes))
				return fail(500, "Failed to read attachment: " + source_path_text, "read_failed", *size);
		}
		else
		{
			return fail(400, "File attachments require data or a filesystem path.", "no_source", 0);
		}
		if (bytes.size() > kMaxBytes)
			return fail(413, "Attachment is larger than the 25 MB limit.", "over_limit", expected.value_or(0));
		// A file that really is empty may be attached; an empty payload for anything else is a failed read.
		if (bytes.empty() && expected != std::uintmax_t{0})
			return fail(400, "The attachment arrived empty, so it was not copied. A file too large for the app to read arrives this way; attach a smaller file.", "empty_payload", expected.value_or(0));
		if (expected && *expected != bytes.size())
			return fail(400, "The attachment was read incompletely, so it was not copied. Attach it again.", "size_mismatch", *expected);
		return true;
	}
} // namespace uam::attachment_payload
