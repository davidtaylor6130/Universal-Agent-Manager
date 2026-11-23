#pragma once

#include "common/models/app_models.h"
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace uam::antigravity::history
{
	/// Version of the reader's protobuf field tables. Raise it whenever a field
	/// table changes, so earlier unknown-format refusals are reported again.
	inline constexpr int kReaderVersion = 2;

	/// A conversation refused because it uses a field this reader version does
	/// not know. It names only the message kind, field number and wire type,
	/// never history content.
	struct FormatRefusal
	{
		std::string native_session_id;
		/// Same text as the matching entry in Discovery::errors.
		std::string message;
		/// One detailed diagnostic line for the log.
		std::string diagnostic;
		/// Starts with "antigravity-cli|<session>|" and changes when the reader
		/// version or the native file changes.
		std::string report_key;
	};

	struct Discovery
	{
		std::vector<ChatSession> chats;
		std::vector<std::string> errors;
		// Omitted details remain structured so downstream caps cannot hide the total.
		std::size_t omitted_error_count = 0;
		// Unknown-format refusals are always listed in errors as well, outside the
		// detail cap, so a caller can report each one once.
		std::vector<FormatRefusal> format_refusals;
	};

	/// Reads only the CLI conversations directory, never launches agy or the SDK.
	/// Empty root resolves the documented user-local CLI store. Original absolute
	/// workspaces are retained; shared application code decides folder ownership.
	/// Optional observation seam for deterministic source-revision races in private
	/// fixtures; cannot supply or substitute snapshot bytes or native identity.
	using SnapshotCopied = std::function<void()>;
	using WorkspaceFilter = std::function<bool(const std::string&)>;
	Discovery Discover(const std::filesystem::path& root = {}, const std::string& native_session = {}, std::stop_token cancel = {}, const SnapshotCopied& snapshot_copied = {}, const WorkspaceFilter& workspace_filter = {}, const SnapshotCopied& summary_snapshot_copied = {});
} // namespace uam::antigravity::history
