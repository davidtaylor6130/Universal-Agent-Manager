#pragma once

#include "common/chat/message_attachment_json.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/base64.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <string_view>

namespace uam::attachment_payload
{
	/// Limit for payloads held in memory: inline base64 data, and path files small enough to load and preview.
	inline constexpr std::uintmax_t kMaxBytes = 25ull * 1024ull * 1024ull;
	/// Limit for files copied from a controller path in bounded chunks; equals the SSH helper's upload limit.
	inline constexpr std::uintmax_t kMaxStreamedBytes = 4ull * 1024ull * 1024ull * 1024ull;
	using Progress = std::function<void(std::uintmax_t copied, std::uintmax_t total)>;

	/// Attachment ids whose chip the user removed while its copy was still running. A copy checks this between
	/// chunks, stops, and removes what it wrote. Ids are unique per attachment, so entries are small and never reused.
	/// Set once when the app is shutting down: every running copy then stops between chunks and removes its
	/// partial file, so quitting does not wait for a multi-gigabyte copy to finish.
	inline std::atomic<bool>& ShuttingDown()
	{
		static std::atomic<bool> shutting_down{false};
		return shutting_down;
	}

	inline bool ExchangeCanceled(const std::string& attachment_id, const bool cancel)
	{
		static std::mutex mutex;
		static std::set<std::string> canceled;
		if (!cancel && ShuttingDown().load()) return true;
		const std::scoped_lock lock(mutex);
		if (cancel) return canceled.insert(attachment_id).second;
		return canceled.contains(attachment_id);
	}

	/// True when the item is a controller file too large for the in-memory route, which must be copied from disk instead.
	/// A file over the streamed limit is refused here with its reason.
	inline bool IsStreamedSource(const nlohmann::json& item, const std::string& source_path_text, std::filesystem::path& source, std::uintmax_t& size, int& status, std::string& error)
	{
		if (source_path_text.empty() || uam::nlohmann_json::FindStringField(item, message_attachment_json::frontend::kDataBase64Field) != nullptr)
			return false;
		source = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(source_path_text);
		const std::optional<std::uintmax_t> found = uam::paths::FileSizeNoThrow(source);
		if (!found || !uam::paths::IsRegularFileNoThrow(source) || *found <= kMaxBytes)
			return false;
		size = *found;
		if (size > kMaxStreamedBytes)
		{
			uam::diagnostics::Write("[stageChatAttachments] Attachment refused before staging: reason=over_streamed_limit expected_bytes=" + std::to_string(size) + " read_bytes=0");
			status = 413;
			error = "Attachment is larger than the 4 GB limit.";
		}
		return true;
	}

	/// Copies a controller file into the workspace in bounded chunks. Checks free space first, confirms the
	/// written size, and removes a partial copy on any failure.
	inline bool CopyStreamed(const std::filesystem::path& source, const std::filesystem::path& target, const std::uintmax_t size, const Progress& progress, std::string& error, const std::function<bool()>& canceled = {})
	{
		std::ifstream input;
		std::ofstream output;
		const auto fail = [&](std::string message, const std::string_view reason, const std::uintmax_t copied)
		{
			// Closed first: Windows cannot remove a file that is still open.
			input.close();
			output.close();
			std::error_code remove_error;
			std::filesystem::remove(target, remove_error);
			uam::diagnostics::Write("[stageChatAttachments] Attachment copy failed: reason=" + std::string(reason) + " expected_bytes=" + std::to_string(size) + " copied_bytes=" + std::to_string(copied));
			error = std::move(message);
			return false;
		};
		std::error_code space_error;
		const std::filesystem::space_info space = std::filesystem::space(target.parent_path(), space_error);
		if (!space_error && space.available != static_cast<std::uintmax_t>(-1) && space.available < size)
			return fail("There is not enough free space in the workspace for this attachment.", "insufficient_space", 0);
		input.open(source, std::ios::binary);
		output.open(target, std::ios::binary | std::ios::trunc);
		if (!input || !output)
			return fail("The attachment could not be opened for copying.", "open_failed", 0);
		std::string buffer(1024 * 1024, '\0'); // On the heap: worker threads have small stacks.
		std::uintmax_t copied = 0;
		while (input)
		{
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize count = input.gcount();
			if (count <= 0) break;
			output.write(buffer.data(), count);
			if (!output)
				return fail("The attachment could not be written. Check the free space in the workspace and try again.", "write_failed", copied);
			copied += static_cast<std::uintmax_t>(count);
			// A source that is still growing must not run past the size the free-space check allowed for.
			if (copied > size)
				return fail("The attachment changed while it was being copied, so the copy was removed. Attach it again.", "source_grew", copied);
			if (canceled && canceled())
				return fail("The attachment copy was canceled.", "canceled", copied);
			if (progress) progress(copied, size);
		}
		output.close();
		std::error_code size_error;
		if (input.bad() || !output || copied != size || std::filesystem::file_size(target, size_error) != size || size_error)
			return fail("The attachment was copied incompletely, so the copy was removed. Attach it again.", "size_mismatch", copied);
		return true;
	}

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
