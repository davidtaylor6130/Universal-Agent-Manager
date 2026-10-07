#include "chat_file_change_receipt_service.h"
#include "common/provider/provider_ids.h"
#include "common/paths/path_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaxReceiptCount = 256;
		constexpr std::size_t kMaxPatchBytes = 1024 * 1024;
		constexpr std::size_t kMaxTotalPatchBytes = 4 * 1024 * 1024;
		constexpr std::size_t kMaxToolResultBytes = 8 * 1024 * 1024;

		std::string NormalizePath(std::string value)
		{
			std::ranges::replace(value, '\\', '/');
			std::string normalized = uam::paths::Utf8PathString(uam::paths::PathFromUtf8(value).lexically_normal());
			std::ranges::replace(normalized, '\\', '/');
			return normalized;
		}

		bool IsWindowsDrivePath(std::string_view value)
		{
			return value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':' && value[2] == '/';
		}

		std::string PathComparisonKey(std::string value, bool windows)
		{
			if (windows) std::ranges::transform(value, value.begin(), [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
			return value;
		}

		std::optional<std::string> WorkspaceRelativePath(const std::string& reported, std::string_view workspace)
		{
			if (reported.empty() || reported.size() > 4096 || reported.find('\0') != std::string::npos) return std::nullopt;
			const std::string path = NormalizePath(reported);
			if (path.empty() || path == ".") return std::nullopt;
			const bool absolute = path.front() == '/' || IsWindowsDrivePath(path);
			if (!absolute)
			{
				if (path == ".." || path.starts_with("../") || path.find(':') != std::string::npos) return std::nullopt;
				return path;
			}
			if (workspace.empty()) return std::nullopt;
			std::string root = NormalizePath(std::string(workspace));
			while (root.size() > 1 && root.back() == '/') root.pop_back();
			const bool windows = IsWindowsDrivePath(path) && IsWindowsDrivePath(root);
			const std::string key = PathComparisonKey(path, windows);
			const std::string root_key = PathComparisonKey(root, windows) + (root == "/" ? "" : "/");
			if (!key.starts_with(root_key) || path.size() <= root_key.size()) return std::nullopt;
			return path.substr(root_key.size());
		}
	}

	bool ChatFileChangeReceiptService::MatchesPersistedSnapshot(const ChatSession& loaded, std::string_view chat_id, std::size_t message_count, std::string_view digest)
	{
		return loaded.id == chat_id && loaded.messages_loaded && loaded.messages.size() == message_count &&
		    (digest.empty() || loaded.persisted_messages_digest == digest);
	}

	ChatFileChangeReceipts ChatFileChangeReceiptService::Extract(const ChatSession& chat, std::string_view workspace_root)
	{
		ChatFileChangeReceipts result;
		std::size_t bytes = 0;
		for (std::size_t index = 0; index < chat.messages.size(); ++index)
		{
			const Message& message = chat.messages[index];
			if (!message.provider.empty() && message.provider != provider_ids::kCodexCli) continue;
			for (const ToolCall& tool : message.tool_calls)
			{
				if (tool.kind != "fileChange" || tool.status != "completed") continue;
				if (tool.result_text.size() > kMaxToolResultBytes) { ++result.omitted_count; continue; }
				const nlohmann::json item = nlohmann::json::parse(tool.result_text, nullptr, false);
				if (!item.is_object() || nlohmann_json::StringViewOrEmpty(item, "type") != "fileChange" ||
				    nlohmann_json::StringViewOrEmpty(item, "status") != "completed") continue;
				const auto changes = item.find("changes");
				if (changes == item.end() || !changes->is_array()) continue;
				for (const nlohmann::json& change : *changes)
				{
					if (!change.is_object()) continue;
					const std::string reported(nlohmann_json::StringViewOrEmpty(change, "path"));
					const std::optional<std::string> path = WorkspaceRelativePath(reported, workspace_root);
					const std::string_view patch = nlohmann_json::StringViewOrEmpty(change, "diff");
					const auto kind = change.find("kind");
					if (!path || patch.empty() || patch.size() > kMaxPatchBytes ||
					    bytes + patch.size() > kMaxTotalPatchBytes || result.receipts.size() >= kMaxReceiptCount ||
					    kind == change.end() || !(kind->is_object() || kind->is_string()) || kind->dump().size() > 4096)
					{ ++result.omitted_count; continue; }
					std::string destination;
					if (kind->is_object())
					{
						const auto move = kind->find("movePath");
						if (move != kind->end() && !move->is_null())
						{
							const std::optional<std::string> move_path = move->is_string()
							    ? WorkspaceRelativePath(move->get<std::string>(), workspace_root) : std::nullopt;
							if (!move_path) { ++result.omitted_count; continue; }
							destination = *move_path;
						}
					}
					result.receipts.push_back({chat.id, index, message.created_at, std::string(provider_ids::kCodexCli), tool.id,
					    reported, *path, destination, kind->dump(), std::string(patch)});
					bytes += patch.size();
				}
			}
		}
		return result;
	}
}
