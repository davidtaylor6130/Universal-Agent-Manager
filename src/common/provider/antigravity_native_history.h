#pragma once

#include "common/models/app_models.h"
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace uam::antigravity::history
{
	struct Discovery
	{
		std::vector<ChatSession> chats;
		std::vector<std::string> errors;
		// Omitted details remain structured so downstream caps cannot hide the total.
		std::size_t omitted_error_count = 0;
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
