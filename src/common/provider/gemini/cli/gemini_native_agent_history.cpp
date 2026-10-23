#include "common/provider/gemini/cli/gemini_native_agent_history.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/provider_profile.h"
#include "common/utils/uuid.h"
#include "core/chat_import_utils.h"
#include "core/native_import_content.h"
#include "remote/native_history_snapshot.h"

namespace uam::gemini
{
namespace
{
	bool ValidReference(const native_agents::HistoryReference& reference)
	{
		return reference.provider_id == provider_ids::kGeminiCli &&
		    !native_agents::BuildHistoryId(reference.provider_id, reference.parent_session_id, reference.agent_id, reference.execution_host_id).empty() &&
		    remote::native_history::SafeId(reference.agent_id) && uuid::IsCanonicalUuid(reference.parent_session_id);
	}

	std::optional<ChatSession> MapNativeAgentDocument(const native_agents::HistoryReference& reference, nlohmann::json document,
	    const std::string& workspace, std::string* error, std::stop_token stop_token)
	{
		if (stop_token.stop_requested() || !ValidReference(reference) ||
		    !remote::native_history::GeminiChildDocumentIdentity(document, reference.parent_session_id, reference.agent_id, workspace))
		{ if (error) *error = "The Gemini child transcript does not attest its exact child and workspace."; return std::nullopt; }
		auto chat = GeminiJsonHistoryStore::ParseDocument(std::move(document), ProviderProfileStore::DefaultGeminiProfile(), {0, 0, reference.execution_host_id});
		if (!chat || !uam::HasImportedConversationContent(chat->messages) || stop_token.stop_requested())
		{ if (error) *error = "The exact Gemini child has no readable conversation or was cancelled."; return std::nullopt; }
		chat->id = native_agents::BuildHistoryId(reference.provider_id, reference.parent_session_id, reference.agent_id, reference.execution_host_id);
		chat->native_session_id.clear(); chat->parent_chat_id = reference.parent_session_id; chat->branch_root_chat_id = chat->id;
		chat->execution_host_id = reference.execution_host_id; chat->workspace_directory = workspace; chat->imported_read_only = true;
		return chat;
	}
}

std::optional<ChatSession> ParseNativeAgentHistory(const native_agents::HistoryReference& reference, std::string_view text,
	const std::string& workspace, std::string* error, std::stop_token stop_token)
{
	if (error) error->clear();
	if (!ValidReference(reference) || stop_token.stop_requested() || text.empty() ||
	    (reference.execution_host_id != "local" && text.size() > remote::native_history::kMaxSnapshotBytes))
	{ if (error) *error = "The exact Gemini child snapshot is unavailable, cancelled or exceeds its SSH transport limit."; return std::nullopt; }
	auto document = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
	if (document.is_object()) return MapNativeAgentDocument(reference, std::move(document), workspace, error, stop_token);
	auto folded = FoldHistoryJournal(text);
	if (!folded) { if (error) *error = "The Gemini child journal is malformed."; return std::nullopt; }
	return MapNativeAgentDocument(reference, std::move(*folded), workspace, error, stop_token);
}

std::optional<ChatSession> LoadNativeAgentHistory(const native_agents::HistoryReference& reference, const std::string& workspace,
	std::string* error, std::stop_token stop_token)
{
	if (error) error->clear();
	if (reference.execution_host_id != "local" || !ValidReference(reference) || stop_token.stop_requested())
	{ if (error) *error = "The exact local Gemini child reader is unavailable or cancelled."; return std::nullopt; }
	std::optional<nlohmann::json> child_document;
	const bool visited = remote::native_history::VisitGeminiParentFiles(reference.parent_session_id, workspace, [&](const auto& root, const auto& parent_file)
	{
		if (stop_token.stop_requested()) return false;
		const auto parent_path = remote::native_history::GeminiFileInsideRoot(root, parent_file);
		if (!parent_path) return false;
		const auto metadata = ReadHistoryDocumentFile(*parent_path, 0, false, stop_token);
		if (!metadata || remote::native_history::String(*metadata, "sessionId") != reference.parent_session_id) return true;
		auto relative_child = parent_file.parent_path() / reference.parent_session_id / (reference.agent_id + ".jsonl");
		std::error_code path_error;
		if (!std::filesystem::exists(root / relative_child, path_error)) relative_child = parent_file.parent_path() / reference.parent_session_id / (reference.agent_id + ".json");
		if (path_error) return false;
		const auto child_path = remote::native_history::GeminiFileInsideRoot(root, relative_child);
		if (!child_path || child_document) return false;
		auto document = ReadHistoryDocumentFile(*child_path, 0, true, stop_token);
		if (!document || !remote::native_history::GeminiChildDocumentIdentity(*document, reference.parent_session_id, reference.agent_id, workspace)) return false;
		child_document = std::move(*document);
		return true;
	}, stop_token);
	if (!visited || !child_document)
	{ if (error) *error = "The exact local Gemini parent and child journals are unavailable or ambiguous."; return std::nullopt; }
	return MapNativeAgentDocument(reference, std::move(*child_document), workspace, error, stop_token);
}
}
