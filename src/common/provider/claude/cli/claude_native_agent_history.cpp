#include "common/provider/claude/cli/claude_native_agent_history.h"
#include "common/provider/claude/cli/claude_tool_projection.h"
#include "common/paths/path_utils.h"
#include "common/runtime/acp/acp_content.h"
#include "common/utils/io_utils.h"
#include "common/utils/uuid.h"
#include "core/chat_import_utils.h"
#include "core/native_import_content.h"
#include "remote/native_history_snapshot.h"
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace uam::claude
{
namespace
{
void AppendTranscriptText(std::string& target, const std::string& text)
{
	if (text.empty()) return;
	if (!target.empty() && target.back() != '\n') target += "\n";
	target += text;
}

std::string NativeContentText(const nlohmann::json& content)
{
	if (content.is_string()) return content.get<std::string>();
	if (content.is_object())
	{
		if (content.contains("text") && content["text"].is_string()) return content["text"].get<std::string>();
		if (content.contains("content")) return NativeContentText(content["content"]);
	}
	std::string text;
	if (content.is_array())
		for (const auto& item : content) AppendTranscriptText(text, NativeContentText(item));
	return text;
}
}

std::optional<ChatSession> ParseNativeTranscript(std::string_view text, const std::string& session_id, const std::string& workspace,
	std::string* error, std::stop_token stop_token, const std::string& agent_id)
{
	if (error) error->clear();
	const auto fail = [&](const char* reason) -> std::optional<ChatSession> { if (error) *error = reason; return std::nullopt; };
	if (text.empty() || text.size() > remote::native_history::kMaxSnapshotBytes || stop_token.stop_requested()) return fail("The Claude native transcript is empty, too large or cancelled.");
	try
	{
	std::unordered_map<std::string, nlohmann::json> records;
	std::string leaf;
	bool malformed = false;
	const auto consume = [&](const std::string& line)
	{
		if (stop_token.stop_requested()) return false;
		if (uam::strings::IsBlank(line)) return true;
		const nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
		if (!record.is_object()) { malformed = true; return false; }
		if (agent_id.empty() && record.value("isSidechain", false)) return true;
		if (!agent_id.empty() && ((record.contains("sessionId") && record.value("sessionId", "") != session_id) ||
		    (record.contains("agentId") && record.value("agentId", "") != agent_id))) { malformed = true; return false; }
		const std::string type = record.value("type", "");
		const bool conversational = type == "user" || type == "assistant";
		if (!record.contains("uuid") || !record["uuid"].is_string()) { if (conversational) malformed = true; return !malformed; }
		if (conversational && (!record.contains("message") || !record["message"].is_object())) { malformed = true; return false; }
		if (record.contains("cwd") && !record["cwd"].is_string()) return true;
		const std::string uuid = record["uuid"].get<std::string>();
		if (conversational || record.value("subtype", "") == "compact_boundary") leaf = uuid;
		records[uuid] = record;
		return true;
	};
	std::istringstream input{std::string(text)};
	std::string line;
	while (std::getline(input, line)) if (!consume(line)) { malformed = true; break; }
	if (stop_token.stop_requested() || malformed)
	{
		return fail("Could not parse the exact Claude native transcript.");
	}
	std::vector<nlohmann::json> chain;
	std::unordered_set<std::string> seen;
	while (!leaf.empty() && records.contains(leaf) && seen.insert(leaf).second)
	{
		chain.push_back(records.at(leaf));
		const nlohmann::json& record = chain.back();
		leaf = record.contains("parentUuid") && record["parentUuid"].is_string() ? record["parentUuid"].get<std::string>() : "";
	}
	if (chain.empty()) return std::nullopt;
	ChatSession chat;
	chat.id = session_id;
	chat.native_session_id = chat.id;
	chat.provider_id = uam::provider_ids::kClaudeCli;
	for (const nlohmann::json& record : chain)
		if (record.contains("cwd") && record["cwd"].is_string()) { chat.workspace_directory = record["cwd"].get<std::string>(); break; }
	if (chat.workspace_directory.empty()) chat.workspace_directory = workspace;
	if (chat.workspace_directory.empty() || (!workspace.empty() && !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(chat.workspace_directory), uam::paths::PathFromUtf8(workspace)))) return std::nullopt;
	std::unordered_set<std::string> summarized_boundaries;
	for (const nlohmann::json& record : chain)
		if (record.value("isCompactSummary", false) && record.contains("parentUuid") && record["parentUuid"].is_string()) summarized_boundaries.insert(record["parentUuid"].get<std::string>());
	std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> tools;
	const auto apply_notification = [&](const nlohmann::json& notification)
	{
		const std::string tool_id = notification.value("tool_use_id", "");
		const auto found = tools.find(tool_id);
		const std::string status = notification.value("status", "");
		const std::string summary = notification.value("summary", "");
		if (found != tools.end() && found->second.first < chat.messages.size())
		{
			ToolCall& tool = chat.messages[found->second.first].tool_calls[found->second.second];
			tool.task_id = notification.value("task_id", "");
			tool.status = status == "completed" ? "completed" : status == "failed" ? "failed" : "cancelled";
			if (!summary.empty()) AppendTranscriptText(tool.result_text, summary);
		}
		else if (!summary.empty())
		{
			Message notice;
			notice.role = MessageRole::System;
			notice.content = summary;
			chat.messages.push_back(std::move(notice));
		}
	};
	for (auto record = chain.rbegin(); record != chain.rend(); ++record)
	{
		if (!record->contains("message"))
		{
			if (record->value("subtype", "") == "compact_boundary" && !summarized_boundaries.contains(record->value("uuid", "")))
			{
				Message marker;
				marker.role = MessageRole::Assistant;
				marker.created_at = record->value("timestamp", "");
				marker.blocks.emplace_back("context_compaction", "", "", record->value("uuid", ""));
				chat.messages.push_back(std::move(marker));
			}
			continue;
		}
		const nlohmann::json& value = (*record)["message"];
		Message message;
		message.role = (*record).value("type", "") == "user" ? MessageRole::User : MessageRole::Assistant;
		message.created_at = (*record).value("timestamp", "");
		if (!value.contains("content")) continue;
		const nlohmann::json& content = value["content"];
		const nlohmann::json notification = uam::claude::TaskNotification(NativeContentText(content));
		if (message.role == MessageRole::User && notification.is_object() && !(*record).value("isCompactSummary", false))
		{
			apply_notification(notification);
			continue;
		}
		if (content.is_string())
		{
			message.content = content.get<std::string>();
			message.blocks.push_back({"assistant_text", message.content});
		}
		else if (content.is_array()) for (const nlohmann::json& block : content)
		{
			if (!block.is_object()) continue;
			const std::string type = block.value("type", "");
			if (type == "text")
			{
				const std::string text = block.value("text", "");
				AppendTranscriptText(message.content, text);
				if (!text.empty()) message.blocks.push_back({"assistant_text", text});
			}
			else if (type == "thinking")
			{
				const std::string text = block.value("thinking", "");
				AppendTranscriptText(message.thoughts, text);
				if (!text.empty()) message.blocks.push_back({"thought", text});
			}
			else if (type == "tool_use")
			{
				ToolCall tool;
				tool.id = block.value("id", "");
				const std::string native_name = block.value("name", "");
				const nlohmann::json input = block.value("input", nlohmann::json::object());
				tool.name = uam::claude::ToolTitle(native_name, input);
				tool.kind = uam::claude::ToolKind(native_name);
				tool.is_sub_agent = tool.kind == "sub-agent";
				tool.args_json = block.contains("input") ? block["input"].dump() : "{}";
				tool.status = "interrupted";
				tools[tool.id] = {chat.messages.size(), message.tool_calls.size()};
				message.blocks.push_back({"tool_call", "", tool.id, ""});
				message.tool_calls.push_back(std::move(tool));
			}
			else if (type == "tool_result")
			{
				const auto found = tools.find(block.value("tool_use_id", ""));
				if (found == tools.end() || found->second.first >= chat.messages.size()) continue;
				ToolCall& tool = chat.messages[found->second.first].tool_calls[found->second.second];
				tool.result_text = block.contains("content") ? (block["content"].is_string() ? block["content"].get<std::string>() : block["content"].dump()) : "";
				tool.status = block.value("is_error", false) ? "error" : "completed";
				const auto native_result = record->find("toolUseResult");
				if (tool.is_sub_agent && native_result != record->end() && native_result->is_object())
				{
					const std::string agent = remote::native_history::String(*native_result, "agentId");
					if (remote::native_history::SafeId(agent)) tool.sub_agent_id = native_agents::BuildHistoryId(provider_ids::kClaudeCli, session_id, agent, "local");
				}
			}
		}
		if ((*record).value("isCompactSummary", false))
		{
			message.blocks.clear();
			message.blocks.emplace_back("context_compaction", message.content, "", (*record).value("uuid", ""));
			message.content.clear();
			message.thoughts.clear();
			message.tool_calls.clear();
		}
		if (!message.content.empty() || !message.thoughts.empty() || !message.tool_calls.empty() || !message.blocks.empty())
		{
			if (chat.created_at.empty()) chat.created_at = message.created_at;
			chat.updated_at = message.created_at;
			chat.messages.push_back(std::move(message));
		}
	}
	if (chat.messages.empty()) return std::nullopt;
	chat.title = uam::BuildImportedChatTitle(chat.messages, chat.created_at);
	return chat;
	}
	catch (const nlohmann::json::exception&) { return fail("Claude native history contains malformed fields."); }
}

std::optional<ChatSession> ParseNativeAgentHistory(const native_agents::HistoryReference& reference, std::string_view text,
	const std::string& workspace, std::string* error, std::stop_token stop_token)
{
	if (error) error->clear();
	const std::string id = native_agents::BuildHistoryId(reference.provider_id, reference.parent_session_id, reference.agent_id, reference.execution_host_id);
	if (stop_token.stop_requested() || reference.provider_id != provider_ids::kClaudeCli || id.empty() || !remote::native_history::SafeId(reference.agent_id) ||
	    !uuid::IsCanonicalUuid(reference.parent_session_id) || text.size() > remote::native_history::kMaxSnapshotBytes ||
	    !remote::native_history::ClaudeChildIdentity(std::string(text), reference.parent_session_id, reference.agent_id, workspace))
	{ if (error) *error = "The Claude child transcript does not attest its exact parent, agent and workspace."; return std::nullopt; }
	auto chat = ParseNativeTranscript(text, reference.parent_session_id, workspace, error, stop_token, reference.agent_id);
	if (chat && !uam::HasImportedConversationContent(chat->messages)) { if (error) *error = "The exact Claude child contains no conversation content."; return std::nullopt; }
	if (chat)
	{
		chat->id = id; chat->native_session_id.clear(); chat->parent_chat_id = reference.parent_session_id; chat->branch_root_chat_id = id;
		chat->execution_host_id = reference.execution_host_id; chat->workspace_directory = workspace; chat->imported_read_only = true;
		for (auto& message : chat->messages)
			for (auto& tool : message.tool_calls)
				if (const auto nested = native_agents::ParseHistoryId(tool.sub_agent_id); nested && nested->provider_id == provider_ids::kClaudeCli)
					tool.sub_agent_id = native_agents::BuildHistoryId(nested->provider_id, nested->parent_session_id, nested->agent_id, reference.execution_host_id);
	}
	return chat;
}

std::optional<ChatSession> LoadNativeAgentHistory(const native_agents::HistoryReference& reference, const std::string& workspace,
	std::string* error, std::stop_token stop_token)
{
	if (error) error->clear();
	if (reference.execution_host_id != "local" || stop_token.stop_requested()) { if (error) *error = "The exact local Claude child reader is unavailable or cancelled."; return std::nullopt; }
	remote::native_history::Snapshot snapshot;
	std::string load_error;
	if (!remote::native_history::Load(reference.provider_id, reference.parent_session_id, workspace, snapshot, load_error, reference.agent_id))
	{ if (error) *error = load_error; return std::nullopt; }
	return ParseNativeAgentHistory(reference, snapshot.text, workspace, error, stop_token);
}
}
