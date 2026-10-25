#include "app/native_agent_history_service.h"
#include "app/copilot_agent_history_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/native_chat_identity.h"
#include "common/provider/native_provider_agent_history.h"
#include "common/provider/provider_runtime.h"
#include "common/platform/platform_services.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/utils/history_timestamp_order.h"
#include "remote/native_history_snapshot.h"
#include "remote/runner_client.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace uam::native_agents
{
namespace
{
std::optional<HistoryReference> Validate(const ChatSession& source, std::string_view reference, std::string* error)
{
	if (error) error->clear();
	const auto decoded = ParseHistoryId(reference);
	if (!decoded || decoded->provider_id != provider_ids::NormalizeCliProviderAliasOrSelf(source.provider_id) ||
		decoded->parent_session_id != source.native_session_id || decoded->execution_host_id != (source.execution_host_id.empty() ? "local" : source.execution_host_id))
	{
		if (error) *error = "The native agent transcript belongs to a different provider, parent session or host.";
		return std::nullopt;
	}
	return decoded;
}

}


class LocalPassiveHistoryLoader final
{
public:
	static std::optional<AdmittedLocalPassiveNativeHistory> Load(
	    const std::filesystem::path& root, const ChatSession& source, const ProviderProfile& profile,
	    std::string_view reference, std::string_view primary, std::string_view summary, std::string* error, std::stop_token stop)
	{
		const auto fail = [&](const char* message) -> std::optional<AdmittedLocalPassiveNativeHistory>
		{ if (error) *error = message; return std::nullopt; };
		const auto decoded = Validate(source, reference, error);
		if (!decoded || decoded->provider_id != provider_ids::NormalizeCliProviderAliasOrSelf(profile.id) ||
		    !ProviderRuntimeRegistry::Resolve(profile).SupportsLocalPassiveNativeMedia(profile) || decoded->execution_host_id != "local" ||
		    source.imported_read_only || source.id.empty() || source.created_at.empty() || !source.metadata_incarnation ||
		    !root.is_absolute() || !paths::IsControllerLocalWorkspace(source) ||
		    !paths::PathFromUtf8(source.workspace_directory).is_absolute() || primary.empty() || summary.empty() || (primary == "missing" && summary != "missing"))
			return fail("This local child history no longer matches the selected parent or provider. Reopen it and try again.");
		if (ChatRepository::ChatStorageRevision(root, reference) != primary ||
		    ChatRepository::ChatSummaryStorageRevision(root, reference) != summary)
			return fail("Saved passive history changed before loading.");
		auto loaded = LoadHistory(source, reference, std::nullopt, error, stop);
		if (!loaded || stop.stop_requested()) return std::nullopt;
		// Validate the actual reader before saved metadata can replace any field.
		if (loaded->id != reference || loaded->provider_id != decoded->provider_id || !loaded->imported_read_only ||
		    !loaded->native_session_id.empty() || loaded->parent_chat_id != decoded->parent_session_id ||
		    loaded->execution_host_id != "local" || loaded->workspace_directory != source.workspace_directory || !loaded->messages_loaded)
			return fail("The actual passive reader returned a different native identity.");
		AdmittedLocalPassiveNativeHistory result;
		if (primary != "missing")
		{
			result.m_previous = ChatRepository::LoadLocalChat(root, std::string(reference), true, nullptr, false);
			const auto& old = result.m_previous;
			if (!old || !old->messages_loaded || old->id != reference || !old->imported_read_only || !old->native_session_id.empty() ||
			    old->provider_id != decoded->provider_id || old->parent_chat_id != decoded->parent_session_id ||
			    old->execution_host_id != "local" || chat_identity::NativeWorkspaceForHistoryImport(*old) != chat_identity::NativeWorkspaceForHistoryImport(source))
				return fail("Saved passive history belongs to a different owner.");
			// A readable partial journal does not authorize discarding saved history.
			// Compare the actual full graphs, independently of metadata/cache timestamps.
			if (loaded->messages.size() < old->messages.size())
				return fail("Native child history is shorter than the saved conversation. Saved history was retained for reading. Refresh can succeed when this child's complete native history is available; the same shorter source cannot replace it.");
			// Explicit passive refresh has an actual scoped native conversation.
			// The saved record owns UAM metadata, not a newer conversation merely
			// because Rename/Pin/attention changed its display timestamp.
			const auto native_updated_at = loaded->updated_at;
			auto native_messages = std::move(loaded->messages);
			ChatSession refreshed;
			{
				// Copy metadata without cloning either heavy message graph. Previous
				// is worker-owned and restored even if a metadata allocation throws;
				// the CEF owner must still compare its exact saved message witness.
				auto previous_messages = std::move(result.m_previous->messages);
				struct RestoreMessages
				{
					ChatSession& previous;
					std::vector<Message>& messages;
					~RestoreMessages() noexcept { previous.messages.swap(messages); }
				} restore{*result.m_previous, previous_messages};
				refreshed = *result.m_previous;
			}
			refreshed.messages = std::move(native_messages);
			refreshed.messages_loaded = true;
			// Equal, earlier and unknown instants retain the exact saved metadata.
			// Bootstrap an empty saved stamp only from an independently valid native stamp.
			const auto recency = uam::time::CompareHistoryTimestamps(native_updated_at, refreshed.updated_at.empty() ? native_updated_at : refreshed.updated_at);
			if ((refreshed.updated_at.empty() && recency.order == uam::time::HistoryTimestampOrder::Equal) ||
			    (!refreshed.updated_at.empty() && recency.order == uam::time::HistoryTimestampOrder::After))
				refreshed.updated_at = native_updated_at;
			refreshed.persisted_message_count = refreshed.messages.size();
			// Repository-compatible derived cache, never a content/ownership proof.
			refreshed.persisted_messages_digest = refreshed.updated_at + ":" + std::to_string(refreshed.persisted_message_count);
			*loaded = std::move(refreshed);
		}
		else
		{
			loaded->folder_id = source.folder_id;
			// A fresh native reader has no UAM queue archive or derived saved
			// caches. Adopt the same known-empty passive archive and count/digest
			// that the repository will persist, before capturing owner witnesses.
			loaded->persisted_message_count = loaded->messages.size();
			loaded->persisted_messages_digest = loaded->updated_at + ":" + std::to_string(loaded->persisted_message_count);
			if (!loaded->acp_queued_prompts.empty() || loaded->acp_queue_held || loaded->acp_dispatched_queued_prompt_count != 0)
				return fail("A fresh passive native transcript cannot own queued input.");
			loaded->passive_queued_prompt_archive = ChatRepository::ParsePassiveQueuedPromptArchive(
			    nlohmann::json{{"id", loaded->id}}.dump(), loaded->id);
			if (!loaded->passive_queued_prompt_archive) return fail("The passive history archive could not be prepared.");
		}
		if (ChatRepository::ChatStorageRevision(root, reference) != primary ||
		    ChatRepository::ChatSummaryStorageRevision(root, reference) != summary || stop.stop_requested())
			return fail("Saved passive history changed while loading.");
		if (loaded->id != reference || loaded->provider_id != decoded->provider_id || !loaded->imported_read_only ||
		    !loaded->native_session_id.empty() || loaded->parent_chat_id != decoded->parent_session_id ||
		    loaded->execution_host_id != "local" || loaded->workspace_directory != source.workspace_directory)
			return fail("The actual passive reader returned a different native identity.");
		std::vector<ChatSession> checked;
		ChatSession key;
		key.id = loaded->id; key.provider_id = loaded->provider_id; key.execution_host_id = "local";
		key.workspace_directory = loaded->workspace_directory;
		checked.push_back(std::move(key));
		if (!ChatHistorySyncService().FilterDeletedNativeHistory(root, checked, error) || checked.empty())
			return fail("The passive native transcript was deleted or deletion records are unavailable.");
		result.m_root = root;
		result.m_primary = std::string(primary);
		result.m_summary = std::string(summary);
		result.m_reference = std::string(reference);
		result.m_parent_native_id = decoded->parent_session_id;
		result.m_workspace = source.workspace_directory;
		result.m_source_id = source.id;
		result.m_source_birth = source.created_at;
		result.m_chat = std::move(*loaded);
		return std::optional<AdmittedLocalPassiveNativeHistory>(std::move(result));
	}
};

std::optional<AdmittedLocalPassiveNativeHistory> LoadLocalPassiveHistoryForMedia(
	const std::filesystem::path& root, const ChatSession& source, const ProviderProfile& profile,
	std::string_view reference, std::string_view primary, std::string_view summary, std::string* error, std::stop_token stop)
{
	return LocalPassiveHistoryLoader::Load(root, source, profile, reference, primary, summary, error, stop);
}

std::optional<ChatSession> ParseHistorySnapshot(const ExecutionHost& host, const ChatSession& source,
	std::string_view reference, const nlohmann::json& snapshot, std::string* error, std::stop_token stop_token)
{
	const auto decoded = Validate(source, reference, error);
	if (!decoded) return std::nullopt;
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	if (decoded->provider_id == provider_ids::kCopilotCli) return copilot::ParseAgentHistorySnapshot(host, source, reference, snapshot, error, stop_token);
#endif
	const auto fail = [&](const char* message) -> std::optional<ChatSession> { if (error) *error = message; return std::nullopt; };
	if (stop_token.stop_requested()) return fail("Native agent history loading was cancelled.");
	if (host.id == "local" || host.id != decoded->execution_host_id || host.transport != "ssh" || !snapshot.is_object() ||
		remote::native_history::String(snapshot, "providerId") != decoded->provider_id ||
		remote::native_history::String(snapshot, "sessionId") != decoded->parent_session_id ||
		remote::native_history::String(snapshot, "agentId") != decoded->agent_id ||
		!execution_hosts::IsAbsoluteRemotePath(host.platform, source.workspace_directory) ||
		!execution_hosts::RemotePathsMatch(host.platform, remote::native_history::String(snapshot, "workspace"), source.workspace_directory))
		return fail("The native agent snapshot does not attest its provider, parent, agent, host and workspace.");
	const std::string text = remote::native_history::String(snapshot, "text");
	if (text.empty() || text.size() > remote::native_history::kMaxSnapshotBytes) return fail("The native agent snapshot is empty or exceeds its size limit.");
	std::optional<ChatSession> loaded;
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	if (decoded->provider_id == provider_ids::kClaudeCli) loaded = claude::ParseNativeAgentHistory(*decoded, text, source.workspace_directory, error, stop_token);
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	if (decoded->provider_id == provider_ids::kGeminiCli) loaded = gemini::ParseNativeAgentHistory(*decoded, text, source.workspace_directory, error, stop_token);
#endif
	if (!loaded && error && error->empty()) *error = "This provider's exact native agent transcript is unavailable.";
	return loaded;
}

std::optional<ChatSession> LoadHistory(const ChatSession& source, std::string_view reference,
	const std::optional<ExecutionHost>& host, std::string* error, std::stop_token stop_token)
{
	const auto decoded = Validate(source, reference, error);
	if (!decoded || stop_token.stop_requested()) return std::nullopt;
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	if (decoded->provider_id == provider_ids::kCopilotCli) return copilot::LoadAgentHistory(source, reference, host, error, stop_token);
#endif
	if (!host)
	{
		if (!paths::IsControllerLocalWorkspace(source) || !paths::PathFromUtf8(source.workspace_directory).is_absolute())
		{ if (error) *error = "The exact local native parent workspace is unavailable."; return std::nullopt; }
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
		if (decoded->provider_id == provider_ids::kClaudeCli) return claude::LoadNativeAgentHistory(*decoded, source.workspace_directory, error, stop_token);
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		if (decoded->provider_id == provider_ids::kGeminiCli) return gemini::LoadNativeAgentHistory(*decoded, source.workspace_directory, error, stop_token);
#endif
		if (error) *error = "This native agent reader is unavailable in the current build.";
		return std::nullopt;
	}
	if (host->id != source.execution_host_id || host->transport != "ssh" || host->runner_status != "ready")
	{ if (error) *error = "The exact remote native parent helper is unavailable."; return std::nullopt; }
	remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
		remote::SshBridgeArgv(host->ssh_alias, host->platform, host->runner_version, host->runner_directory, host->runner_protocol_version),
		host->runner_version, host->runner_protocol_version);
	nlohmann::json snapshot;
	if (!client.ReadNativeHistory(decoded->provider_id, decoded->parent_session_id, source.workspace_directory, snapshot, error, stop_token, decoded->agent_id)) return std::nullopt;
	return ParseHistorySnapshot(*host, source, reference, snapshot, error, stop_token);
}
}
