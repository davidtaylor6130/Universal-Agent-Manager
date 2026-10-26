#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"
#include "remote/runner_client.h"

#include "app/chat_domain_service.h"
#include "app/native_agent_history_service.h"
#include "app/runtime_orchestration_internal.h"
#include "cef/state_serializer.h"
#include "common/chat/native_media_chat_save.h"
#include "common/runtime/native_refresh_lifetime.h"
#include "common/paths/app_paths.h"
#include "common/utils/io_utils.h"
#include <type_traits>
#include "app/agent_definition_service.h"
#include "app/agent_run_scheduler.h"
#include "app/managed_subagent_control_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/computer_use_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/provider_configuration_service.h"
#include "app/provider_model_catalog_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/uam_control_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/chat/native_chat_identity.h"
#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "common/config/provider_chat_defaults.h"
#include "computer_use/computer_use_mcp_config.h"
#include "computer_use/computer_use_platform.h"
#include "common/memory/memory_levels.h"
#include "common/paths/workspace_root.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/security/command_safety.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Chat configuration handlers (model, provider, approval, memory)
// ---------------------------------------------------------------------------

namespace
{
	std::string NativeChildOpenMetadata(const std::filesystem::path& root, const ChatSession& chat,
	    std::optional<bool> loaded = std::nullopt)
	{
		auto metadata = nlohmann::json::parse(ChatRepository::ChatMetadataFingerprint(root, chat), nullptr, false);
		if (!metadata.is_object()) return {};
		if (loaded) metadata["validationMessagesLoaded"] = *loaded;
		if (loaded.value_or(chat.messages_loaded))
		{
			// A loaded conversation owns its actual message graph. Metadata saves
			// can leave the live derived cache behind the serialized summary;
			// compare its canonical derived facts alongside the separate full
			// message digest and exact primary/summary revisions.
			metadata["validationPersistedCount"] = chat.messages.size();
			metadata["validationPersistedDigest"] = chat.updated_at + ":" + std::to_string(chat.messages.size());
		}
		return metadata.dump();
	}

	bool NativeChildOpenWorkspacesMatch(const ChatSession& parent, const ChatSession& child)
	{
		return uam::strings::NonEmptyOrFallback(parent.execution_host_id, "local") ==
		    uam::strings::NonEmptyOrFallback(child.execution_host_id, "local") &&
		    uam::chat_identity::NativeWorkspaceForHistoryImport(parent) == uam::chat_identity::NativeWorkspaceForHistoryImport(child);
	}

	bool NativeChildOpenReferenceAvailable(const std::filesystem::path& root, const ChatSession& source, const std::string& reference)
	{
		const auto decoded = uam::native_agents::ParseHistoryId(reference);
		if (!decoded || decoded->provider_id != uam::provider_ids::NormalizeCliProviderAliasOrSelf(source.provider_id) ||
		    decoded->parent_session_id != source.native_session_id || decoded->execution_host_id != "local" ||
		    decoded->execution_host_id != uam::strings::NonEmptyOrFallback(source.execution_host_id, "local")) return false;
		ChatSession key;
		key.id = reference; key.provider_id = decoded->provider_id; key.execution_host_id = decoded->execution_host_id;
		key.workspace_directory = source.workspace_directory;
		std::vector<ChatSession> checked;
		checked.push_back(std::move(key));
		return ChatHistorySyncService().FilterDeletedNativeHistory(root, checked) && !checked.empty();
	}

	bool NativeChildOpenProfileMatches(const ProviderProfile& left, const ProviderProfile& right)
	{
		return std::tie(left.id, left.execution_mode, left.output_mode, left.interactive_command, left.history_adapter,
		           left.supports_cli, left.supports_structured, left.structured_protocol, left.runtime_flags, left.resume_argument) ==
		    std::tie(right.id, right.execution_mode, right.output_mode, right.interactive_command, right.history_adapter,
		           right.supports_cli, right.supports_structured, right.structured_protocol, right.runtime_flags, right.resume_argument);
	}

	bool NativeChildDisplayIdentityMatches(const ChatSession& saved, const ChatSession& refreshed)
	{
		return saved.id == refreshed.id && saved.imported_read_only == refreshed.imported_read_only && saved.native_session_id == refreshed.native_session_id && uam::provider_ids::NormalizeCliProviderAliasOrSelf(saved.provider_id) == uam::provider_ids::NormalizeCliProviderAliasOrSelf(refreshed.provider_id) && uam::strings::NonEmptyOrFallback(saved.execution_host_id, "local") == uam::strings::NonEmptyOrFallback(refreshed.execution_host_id, "local") && uam::chat_identity::NativeWorkspaceForHistoryImport(saved) == uam::chat_identity::NativeWorkspaceForHistoryImport(refreshed) && (!saved.imported_read_only || saved.parent_chat_id == refreshed.parent_chat_id);
	}

	bool StopLaunchOptionTerminal(uam::AppState& app, const std::string& chat_id)
	{
		auto* terminal = uam::FindCliTerminalForChat(app, chat_id);
		if (terminal == nullptr)
			return true;
		if (uam::IsCliTerminalTurnBusy(*terminal))
		{
			app.status_line = "Wait for the native terminal turn to finish before changing launch-configured model options.";
			return false;
		}
		return uam::StopAndEraseCliTerminalForChat(app, chat_id);
	}

	bool HasComputerUseOwnership(const uam::AppState& app, const std::string& chat_id, bool enabled)
	{
		if (enabled || app.computer_use_revocation_pending_chat_ids.contains(chat_id)) return true;
		const std::unordered_map<std::string, uam::ComputerUseRuntimeState>::const_iterator runtime = app.computer_use_by_chat_id.find(chat_id);
		return runtime != app.computer_use_by_chat_id.end() &&
		    (runtime->second.state == "armed" || runtime->second.state == "running" || runtime->second.state == "paused");
	}

	uam::query_handler_async::AsyncCefResult RunComputerUseSettingsHelperOnce(
	    std::string action, std::string permission = {})
	{
		const bool user_prompt = action == "request";
		std::vector<std::string> argv = {
		    uam::computer_use::McpExecutablePath(), "--uam-computer-use-mcp", "--settings-action", std::move(action)};
		if (!permission.empty())
		{
			argv.push_back("--permission");
			argv.push_back(std::move(permission));
		}

		uam::platform::StdioProcessPlatformFields process;
		std::string error;
		auto& process_service = PlatformServicesFactory::Instance().process_service;
		if (!process_service.StartStdioProcess(process, {}, argv, &error))
			return uam::query_handler_async::AsyncFailure(500, error.empty() ? "Computer Use helper could not start." : error);
		process_service.CloseStdioProcessInput(process);

		std::string output;
		std::string stderr_output;
		std::array<char, 4096> buffer{};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(user_prompt ? 120 : 60);
		int exit_code = -1;
		bool exited = false;
		for (;;)
		{
			std::string read_error;
			const std::ptrdiff_t read = process_service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &read_error);
			if (read > 0)
			{
				output.append(buffer.data(), static_cast<std::size_t>(read));
				if (output.size() > 1024 * 1024)
				{
					process_service.TerminateStdioProcess(process, true);
					process_service.CloseStdioProcessHandles(process);
					return uam::query_handler_async::AsyncFailure(502, "Computer Use helper returned too much output.");
				}
			}
			const std::ptrdiff_t error_read = process_service.ReadStdioProcessStderr(process, buffer.data(), buffer.size(), nullptr);
			if (error_read > 0)
			{
				stderr_output.append(buffer.data(), static_cast<std::size_t>(error_read));
				if (stderr_output.size() > 1024 * 1024)
				{
					process_service.TerminateStdioProcess(process, true);
					process_service.CloseStdioProcessHandles(process);
					return uam::query_handler_async::AsyncFailure(502, "Computer Use helper returned too much diagnostic output.");
				}
			}
			if (!exited) exited = process_service.PollStdioProcessExited(process, &exit_code);
			if (exited && read <= 0 && error_read <= 0) break;
			if (std::chrono::steady_clock::now() >= deadline)
			{
				process_service.TerminateStdioProcess(process, true);
				process_service.CloseStdioProcessHandles(process);
				return uam::query_handler_async::AsyncFailure(504, "Computer Use helper timed out.");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		process_service.CloseStdioProcessHandles(process);
		const nlohmann::json result = nlohmann::json::parse(output, nullptr, false);
		if (exit_code != 0)
			return uam::query_handler_async::AsyncFailure(409, result.is_object() ? result.value("error", "Computer Use helper failed.") : stderr_output.empty() ? "Computer Use helper failed." : stderr_output);
		if (!result.is_object() && !result.is_array())
			return uam::query_handler_async::AsyncFailure(502, stderr_output.empty() ? "Computer Use helper returned invalid output." : stderr_output);
		return uam::query_handler_async::AsyncSuccess(result);
	}

	uam::query_handler_async::AsyncCefResult RunComputerUseSettingsHelper(
	    const std::string& action, const std::string& permission = {})
	{
		uam::query_handler_async::AsyncCefResult result = RunComputerUseSettingsHelperOnce(action, permission);
		if ((action == "check" || action == "list") && !result.ok && result.status == 502 &&
		    result.error == "Computer Use helper returned invalid output.")
			result = RunComputerUseSettingsHelperOnce(action, permission);
		return result;
	}
}

using namespace uam::query_handler_internal;

void UamQueryHandler::HandleOpenNativeSessionChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	const std::string source_id = payload.value("chatId", "");
	const std::string native_id = uam::strings::Trim(payload.value("nativeSessionId", ""));
	ChatSession* source = ChatDomainService().FindChatById(m_app, source_id);
	const ProviderProfile* provider = source == nullptr ? nullptr : &ProviderResolutionService().ProviderForChatOrDefault(m_app, *source);
	const std::string provider_id = provider == nullptr ? std::string{} : uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider->id);
	const auto agent_reference = uam::native_agents::ParseHistoryId(native_id);
	const bool native_agent = agent_reference.has_value();
	if (native_agent && (source == nullptr || agent_reference->provider_id != provider_id || agent_reference->parent_session_id != source->native_session_id || agent_reference->execution_host_id != (source->execution_host_id.empty() ? "local" : source->execution_host_id)))
	{
		cb->Failure(400, "The native agent reference belongs to a different provider, parent session or host.");
		return;
	}
	ChatSession* target = source == nullptr || provider == nullptr ? nullptr
	    : ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *source, *provider, native_id, false);
	const bool supports_history = provider != nullptr &&
	    (ProviderRuntime::UsesNativeOverlayHistory(*provider) || ProviderRuntime::UsesLocalHistory(*provider));
	const bool supported_host = source != nullptr && (uam::paths::IsControllerLocalWorkspace(*source) || provider_id == uam::provider_ids::kOpenCodeCli || provider_id == uam::provider_ids::kCodexCli || native_agent);
	const bool needs_export = source != nullptr && supports_history && supported_host && !native_id.empty() &&
	    (payload.value("refreshHistory", !payload.value("selectChat", true)) || target == nullptr);
	const bool needs_preparation = needs_export || (supports_history && supported_host && target != nullptr && !target->messages_loaded);
	const bool supports_local_passive_media = provider != nullptr &&
	    ProviderRuntimeRegistry::Resolve(*provider).SupportsLocalPassiveNativeMedia(*provider);

	// This opt-in route is only the existing local passive journal Open. Remote
	// snapshots and resumable native-session opens retain their current path.
	if (needs_export && native_agent && supports_local_passive_media &&
	    source != nullptr && uam::paths::IsControllerLocalWorkspace(*source))
	{
		struct Work
		{
			std::filesystem::path root;
			ChatSession source_identity;
			std::optional<uam::AdmittedLocalPassiveNativeHistory> loaded;
			std::unique_ptr<uam::PreparedNativeMediaChatSave> prepared;
			std::optional<ChatSession> retired;
			uam::PassiveNativeOpenMetadata metadata;
			std::string error, ack_error, target_metadata, target_digest, staged_metadata;
			bool target_present = false, target_loaded = false, staged_new = false, acknowledgement_finished = false;
			std::shared_ptr<const std::uint8_t> target_incarnation;
			AsyncCefResult outcome;
			void Acknowledge() noexcept
			{
				if (!prepared || !prepared->PrimaryAcknowledged() || acknowledgement_finished) return;
				try
				{
					if (!prepared->AcknowledgeOriginals(ack_error) && ack_error.empty()) ack_error = "Original acknowledgement was not confirmed.";
#if defined(__APPLE__)
					uam::io::AtomicWriteResult synced;
					if (!(uam::io::SyncAtomicWriteDirectory(AppPaths::UamChatsRootPath(root), synced) &&
					      uam::io::SyncAtomicWriteDirectory(AppPaths::UamChatSummariesRootPath(root), synced) &&
					      uam::io::SyncAtomicWriteDirectory(root, synced)) && ack_error.empty())
						ack_error = "Published history directory durability was not confirmed.";
#endif
				}
				catch (...) { ack_error = "Published original acknowledgement was not confirmed."; }
				acknowledgement_finished = true;
			}
			~Work() noexcept { Acknowledge(); } // Armed cleanup worker, never app/CEF ownership.
		};
		auto work = uam::native_history::TryCreateWorkerOwnedNativeRefresh<Work>();
		if (!work) { cb->Failure(503, "Native history preparation is busy. Retry shortly."); return; }
		work->root = m_app.data_root;
		work->target_present = target != nullptr;
		work->target_loaded = target != nullptr && target->messages_loaded;
		if (target)
		{
			work->target_incarnation = target->metadata_incarnation;
			work->target_metadata = NativeChildOpenMetadata(work->root, *target);
			if (target->messages_loaded) work->target_digest = uam::StateSerializer::MessageDigest(*target, false);
		}
		// No source/target/app transcript copy is made on this owner thread.
		auto& identity = work->source_identity;
		identity.id = source_id; identity.created_at = source->created_at; identity.metadata_incarnation = source->metadata_incarnation;
		identity.provider_id = source->provider_id; identity.native_session_id = source->native_session_id;
		identity.workspace_directory = source->workspace_directory; identity.execution_host_id = source->execution_host_id;
		identity.folder_id = source->folder_id; identity.workspace_isolation_kind = source->workspace_isolation_kind;
		identity.workspace_source_directory = source->workspace_source_directory; identity.workspace_base_ref = source->workspace_base_ref;
		identity.workspace_branch_name = source->workspace_branch_name; identity.workspace_worktree_directory = source->workspace_worktree_directory;
		const auto source_identity = ChatDomainService().CaptureBranchHistoryIdentity(*source);
		const auto source_metadata = NativeChildOpenMetadata(work->root, *source);
		const auto source_digest = source->messages_loaded ? uam::StateSerializer::MessageDigest(*source, false) : std::string{};
		const bool source_loaded = source->messages_loaded;
		const auto source_primary = ChatRepository::ChatStorageRevision(work->root, source_id);
		const auto source_summary = ChatRepository::ChatSummaryStorageRevision(work->root, source_id);
		const auto primary = ChatRepository::ChatStorageRevision(work->root, native_id);
		const auto summary = ChatRepository::ChatSummaryStorageRevision(work->root, native_id);
		const auto native_directory = m_app.native_history_chats_dir;
		const auto workspace = uam::paths::ResolveWorkspaceRootPath(m_app, *source);
		const ProviderProfile profile = *provider;
		const auto source_mapping_it = m_app.resolved_native_sessions_by_chat_id.find(source_id);
		const std::optional<std::string> source_mapping = source_mapping_it == m_app.resolved_native_sessions_by_chat_id.end()
		    ? std::nullopt : std::optional<std::string>(source_mapping_it->second);
		if (!work->root.is_absolute() || !source_primary || !source_summary || !primary || !summary ||
		    source_metadata.empty() || (target && work->target_metadata.empty()) ||
		    (source_mapping && *source_mapping != identity.native_session_id) || m_app.resolved_native_sessions_by_chat_id.contains(native_id))
		{ cb->Failure(409, "Passive native history ownership is not ready."); return; }
		const std::string request_key = native_id;
		if (const auto prior = m_nativeHistoryRequests.find(request_key); prior != m_nativeHistoryRequests.end())
		{
			prior->second->request_stop();
			const auto owner = m_app.branch_preparations.find(source_id);
			if (owner != m_app.branch_preparations.end())
				if (const auto previous = owner->second.lock(); previous && previous->cancellation.get_token() == prior->second->get_token())
					(void)ChatDomainService().ConsumeBranchPreparation(m_app, source_id, previous);
		}
		const auto ownership = ChatDomainService().BeginBranchPreparation(m_app, source_id, native_id);
		if (!ownership) { cb->Failure(409, "Chat preparation is still running. Retry after it settles."); return; }
		const auto request = std::make_shared<std::stop_source>(ownership->cancellation);
		m_nativeHistoryRequests[request_key] = request;
		const auto finish = [this, source_id, request_key, request, ownership]()
		{
			const auto found = m_nativeHistoryRequests.find(request_key);
			if (found != m_nativeHistoryRequests.end() && found->second == request) m_nativeHistoryRequests.erase(found);
			(void)ChatDomainService().ConsumeBranchPreparation(m_app, source_id, ownership);
		};
		const auto admitted = [this, work, source_id, native_id, source_identity, source_metadata, source_digest, source_loaded,
		    source_primary, source_summary, primary, summary, source_mapping, native_directory, workspace, profile, provider_id, request_key, request, ownership]() -> bool
		{
			const auto pending = m_nativeHistoryRequests.find(request_key);
			const auto preparation = m_app.branch_preparations.find(source_id);
			const auto* parent = ChatDomainService().FindChatById(m_app, source_id);
			if (m_app.data_root != work->root || request->stop_requested() ||
			    pending == m_nativeHistoryRequests.end() || pending->second != request ||
			    preparation == m_app.branch_preparations.end() || preparation->second.lock() != ownership ||
			    m_workspaceDeletionPending || m_app.deletion_progress || !parent ||
			    parent->metadata_incarnation != work->source_identity.metadata_incarnation || parent->created_at != work->source_identity.created_at ||
			    parent->native_session_id != work->source_identity.native_session_id || parent->folder_id != work->source_identity.folder_id ||
			    parent->imported_read_only || parent->messages_loaded != source_loaded ||
			    !ChatDomainService().MatchesBranchHistoryIdentity(*parent, source_identity) ||
			    NativeChildOpenMetadata(work->root, *parent) != source_metadata ||
			    (source_loaded && uam::StateSerializer::MessageDigest(*parent, false) != source_digest) ||
			    uam::paths::ResolveWorkspaceRootPath(m_app, *parent) != workspace || m_app.native_history_chats_dir != native_directory ||
			    !NativeChildOpenProfileMatches(ProviderResolutionService().ProviderForChatOrDefault(m_app, *parent), profile) ||
			    !ProviderRuntimeRegistry::Resolve(profile).SupportsLocalPassiveNativeMedia(profile) ||
			    m_app.pending_chat_save_at_by_chat_id.contains(source_id) || m_app.pending_chat_save_tasks.contains(source_id) ||
			    m_app.pending_chat_save_at_by_chat_id.contains(native_id) || m_app.pending_chat_save_tasks.contains(native_id) ||
			    m_app.worktree_operation_chat_ids.contains(source_id) || m_app.worktree_operation_chat_ids.contains(native_id) ||
			    ChatRepository::ChatStorageRevision(work->root, source_id) != source_primary ||
			    ChatRepository::ChatSummaryStorageRevision(work->root, source_id) != source_summary ||
			    ChatRepository::ChatStorageRevision(work->root, native_id) != primary ||
			    ChatRepository::ChatSummaryStorageRevision(work->root, native_id) != summary ||
			    m_app.resolved_native_sessions_by_chat_id.contains(native_id)) return false;
			const auto mapping = m_app.resolved_native_sessions_by_chat_id.find(source_id);
			const std::optional<std::string> now = mapping == m_app.resolved_native_sessions_by_chat_id.end()
			    ? std::nullopt : std::optional<std::string>(mapping->second);
			if (now != source_mapping) return false;
			const auto* child = ChatDomainService().FindChatById(m_app, native_id);
			if (work->staged_new)
			{
				if (!child || child->metadata_incarnation != work->metadata.incarnation ||
				    NativeChildOpenMetadata(work->root, *child) != work->staged_metadata) return false;
			}
			else if ((child != nullptr) != work->target_present) return false;
			else if (child && (child->metadata_incarnation != work->target_incarnation ||
			    child->messages_loaded != work->target_loaded || NativeChildOpenMetadata(work->root, *child) != work->target_metadata ||
			    (child->messages_loaded && uam::StateSerializer::MessageDigest(*child, false) != work->target_digest))) return false;
			if (child && (!child->imported_read_only || !child->native_session_id.empty() || child->parent_chat_id != parent->native_session_id ||
			    child->provider_id != provider_id || !NativeChildOpenWorkspacesMatch(*parent, *child) ||
			    uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(m_app, *child) || uam::ChatHasActiveCliTerminal(m_app, child->id))) return false;
			if (std::ranges::count_if(m_app.chats, [&](const ChatSession& row) { return row.id == source_id; }) != 1 ||
			    std::ranges::count_if(m_app.chats, [&](const ChatSession& row) { return row.id == native_id; }) != (child ? 1 : 0)) return false;
			return NativeChildOpenReferenceAvailable(work->root, *parent, native_id);
		};
		if (!admitted()) { finish(); cb->Failure(409, "Passive native history ownership changed. Retry after it settles."); return; }
		bool queued = false;
		try
		{
			queued = CefPostTask(TID_FILE_BACKGROUND, new CefQueryWorkerTask(m_asyncLifetime, cb,
			    [work, native_id, primary, summary, source_primary, source_summary, profile, request]()
			    {
				    if (request->stop_requested() || !NativeChildOpenReferenceAvailable(work->root, work->source_identity, native_id) ||
				        ChatRepository::ChatStorageRevision(work->root, work->source_identity.id) != source_primary ||
				        ChatRepository::ChatSummaryStorageRevision(work->root, work->source_identity.id) != source_summary)
					    return AsyncFailure(409, "Source history changed before passive loading.");
				    auto loaded = uam::native_agents::LoadLocalPassiveHistoryForMedia(work->root, work->source_identity, profile, native_id,
				        *primary, *summary, &work->error, request->get_token());
				    if (!loaded) return AsyncFailure(502, uam::strings::NonEmptyOrFallback(work->error, "Actual native child history is unavailable."));
				    if (work->target_present)
				    {
					    if (!loaded->Previous() || NativeChildOpenMetadata(work->root, *loaded->Previous(), work->target_loaded) != work->target_metadata ||
					        (work->target_loaded && uam::StateSerializer::MessageDigest(*loaded->Previous(), false) != work->target_digest))
						    return AsyncFailure(409, "The saved child differs from its current owned transcript.");
				    }
				    work->loaded.emplace(std::move(*loaded));
				    return AsyncSuccess(nlohmann::json::object());
			    },
			    [this, browser, cb, payload, work, native_id, primary, summary, source_primary, source_summary, request, admitted, finish](AsyncCefResult& result)
			    {
				    bool deferred = false;
				    struct Exit { std::function<void()> finish; bool& deferred; ~Exit() { if (!deferred) finish(); } } exit{finish, deferred};
				    if (!admitted()) { result = AsyncFailure(409, "Source or child changed while passive history loaded."); return; }
				    if (!result.ok || !work->loaded) return;
				    const auto& candidate = work->loaded->Candidate();
				    ChatSession lineage;
				    lineage.id = candidate.id; lineage.provider_id = candidate.provider_id; lineage.imported_read_only = true;
				    lineage.parent_chat_id = candidate.parent_chat_id; lineage.execution_host_id = candidate.execution_host_id;
				    lineage.workspace_directory = candidate.workspace_directory;
				    lineage.subagent_parent_chat_id = candidate.subagent_parent_chat_id;
				    lineage.subagent_parent_created_at = candidate.subagent_parent_created_at;
				    if (const auto* parent = ChatDomainService().FindChatById(m_app, work->source_identity.id))
					    (void)ChatRepository::ApplyNativeChildDisplayLineage(*parent, lineage, native_id);
				    work->metadata.incarnation = work->target_present ? work->target_incarnation : candidate.metadata_incarnation;
				    work->metadata.display_parent_chat_id = lineage.subagent_parent_chat_id;
				    work->metadata.display_parent_created_at = lineage.subagent_parent_created_at;
				    if (payload.value("selectChat", true)) work->metadata.last_opened_at = uam::time::TimestampNow();
				    bool preparation_queued = false;
				    try
				    {
					    preparation_queued = CefPostTask(TID_FILE_BACKGROUND, new CefQueryWorkerTask(m_asyncLifetime, cb,
					        [work, native_id, primary, summary, source_primary, source_summary, request]()
					        {
						        if (request->stop_requested() || !NativeChildOpenReferenceAvailable(work->root, work->source_identity, native_id) ||
						            ChatRepository::ChatStorageRevision(work->root, native_id) != primary ||
						            ChatRepository::ChatSummaryStorageRevision(work->root, native_id) != summary ||
						            ChatRepository::ChatStorageRevision(work->root, work->source_identity.id) != source_primary ||
						            ChatRepository::ChatSummaryStorageRevision(work->root, work->source_identity.id) != source_summary)
							        return AsyncFailure(409, "History changed before passive original preparation.");
						        work->prepared = uam::PreparedNativeMediaChatSave::PreparePassive(work->root, std::move(*work->loaded),
						            work->metadata, *primary, work->error, request->get_token());
						        work->loaded.reset();
						        return work->prepared ? AsyncSuccess(nlohmann::json::object()) :
						            AsyncFailure(502, uam::strings::NonEmptyOrFallback(work->error, "Passive originals could not be prepared."));
					        },
					        [this, browser, cb, payload, work, native_id, admitted, finish](AsyncCefResult& response)
					        {
						        bool deferred_ack = false;
						        struct Exit { std::function<void()> finish; bool& deferred; ~Exit() { if (!deferred) finish(); } } exit{finish, deferred_ack};
						        if (!admitted()) { response = AsyncFailure(409, "Source or child changed before passive publication."); return; }
						        if (!response.ok || !work->prepared) return;
						        const bool select = payload.value("selectChat", true);
						        const auto previous_selection = ChatDomainService().SelectedChatId(m_app);
						        const auto previous_draft = m_app.composer_text;
						        const auto restore_selection = [&]()
						        {
							        m_app.selected_chat_index = ChatDomainService().FindChatIndexById(m_app, previous_selection);
							        m_app.composer_text = previous_draft;
							        ChatDomainService().RefreshRememberedSelection(m_app);
						        };
						        bool settings_saved = false;
						        const auto rollback = [&]()
						        {
							        if (work->staged_new)
							        {
								        std::erase_if(m_app.chats, [&](const ChatSession& row) { return row.id == native_id && row.metadata_incarnation == work->metadata.incarnation; });
								        work->staged_new = false;
							        }
							        restore_selection();
							        if (settings_saved)
							        {
								        settings_saved = false;
								        (void)PersistenceCoordinator().SaveSettings(m_app);
							        }
						        };
						        struct TransactionExit
						        {
							        std::function<void()> rollback;
							        bool acknowledged = false;
							        ~TransactionExit() noexcept { if (!acknowledged) try { rollback(); } catch (...) {} }
						        } transaction{rollback};
						        // A new row is metadata-only until the acknowledged primary is adopted.
						        // This staging is synchronous; no worker gap exposes a partial target.
						        if (!work->target_present)
						        {
							        const auto& candidate = work->prepared->Candidate();
							        ChatSession placeholder;
							        placeholder.id = candidate.id; placeholder.provider_id = candidate.provider_id;
							        placeholder.created_at = candidate.created_at; placeholder.updated_at = candidate.updated_at;
							        placeholder.metadata_incarnation = work->metadata.incarnation;
							        placeholder.execution_host_id = candidate.execution_host_id; placeholder.workspace_directory = candidate.workspace_directory;
							        placeholder.parent_chat_id = candidate.parent_chat_id; placeholder.folder_id = candidate.folder_id;
							        placeholder.imported_read_only = true; placeholder.messages_loaded = false;
							        work->staged_metadata = NativeChildOpenMetadata(work->root, placeholder);
							        m_app.chats.push_back(std::move(placeholder));
							        work->staged_new = true;
						        }
						        if (select) ChatDomainService().SelectChatById(m_app, native_id);
						        if (!PersistenceCoordinator().SaveSettings(m_app))
						        { response = AsyncFailure(500, "Failed to persist selected chat."); return; }
						        settings_saved = true;
						        if (!work->prepared->Publish(work->error, admitted, false))
						        {
							        response = AsyncFailure(500, "Passive history publication was refused; prior saved content was retained."); return;
						        }
						        transaction.acknowledged = true; // No rollback may replace an acknowledged primary.
						        work->retired = work->prepared->TakeCommittedChat();
						        auto* current = ChatDomainService().FindChatById(m_app, native_id);
						        if (!current || !work->retired)
						        { response = AsyncFailure(500, "Passive history was saved but could not be adopted. Reload this chat."); return; }
						        static_assert(std::is_nothrow_swappable_v<ChatSession>);
						        std::swap(*current, *work->retired); // Old heavy graph retires on the same worker owner.
						        work->staged_new = false;
						        ChatDomainService().SortChatsByRecent(m_app.chats);
						        if (select) ChatDomainService().SelectChatById(m_app, native_id); else restore_selection();
						        work->outcome = AsyncSuccess({{"chatId", native_id}});
						        const auto complete_ack = [this, browser, work, finish](AsyncCefResult& ack)
						        {
							        const bool uncertain = !ack.ok || !work->ack_error.empty();
							        ack = work->outcome;
							        if (uncertain)
							        {
								        auto body = nlohmann::json::parse(ack.body);
								        const std::string warning = "History and attached files were kept, but UAM could not confirm the final save. Reopen the child to check it.";
								        body["warning"] = warning;
								        ack.body = body.dump();
								        if (m_app.data_root == work->root) m_app.status_line = warning;
							        }
							        finish();
							        uam::PushStateUpdateIfChanged(browser, m_app);
						        };
						        bool ack_queued = false;
						        try
						        {
							        ack_queued = CefPostTask(TID_FILE_BACKGROUND, new CefQueryWorkerTask(m_asyncLifetime, cb,
							            [work]() { work->Acknowledge(); return work->ack_error.empty() ? AsyncSuccess(nlohmann::json::object()) : AsyncFailure(500, "Original acknowledgement was not confirmed."); }, complete_ack));
						        }
						        catch (...) { /* The actual committed result is retained below. */ }
						        if (ack_queued) { response.callback_deferred = true; deferred_ack = true; }
						        else { response = AsyncFailure(503, "Original acknowledgement could not be queued."); complete_ack(response); }
					        }));
				    }
				    catch (...) { result = AsyncFailure(503, "Passive original preparation could not be queued."); return; }
				    if (!preparation_queued) { result = AsyncFailure(503, "Passive original preparation could not be queued."); return; }
				    result.callback_deferred = true;
				    deferred = true;
			    }));
		}
		catch (...) { finish(); cb->Failure(503, "Passive history loading could not be queued."); return; }
		if (!queued) { finish(); cb->Failure(503, "Passive history loading could not be queued."); }
		return;
	}

	if (!needs_preparation)
	{
		const AsyncCefResult result = FinishOpenNativeSessionChat(browser, payload);
		if (result.ok) cb->Success(result.body);
		else cb->Failure(result.status, result.error);
		return;
	}

	std::optional<ExecutionHost> remote_host;
	if (!uam::paths::IsControllerLocalWorkspace(*source))
	{
		const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, source->execution_host_id);
		if (host == nullptr || !uam::remote::CanAttemptRunnerConnection(*host))
		{
			cb->Failure(409, "The remote execution host is not ready to load child history.");
			return;
		}
		remote_host = *host;
	}

	struct CachedPassiveHistoryWork
	{
		std::optional<ChatSession> baseline;
		ChatSession snapshot;
		std::string revision;
	};
	std::shared_ptr<CachedPassiveHistoryWork> cached_passive;
	if (!needs_export && native_agent && supports_local_passive_media &&
	    source != nullptr && uam::paths::IsControllerLocalWorkspace(*source))
	{
		// Cached reopen hydrates canonical already-owned history, never issues a
		// native-read seal or needs a still-present parent/child source file.
		// Alias the existing generic branch's payloads to one prearmed lifetime.
		cached_passive = uam::native_history::TryCreateWorkerOwnedNativeRefresh<CachedPassiveHistoryWork>();
		if (!cached_passive) { cb->Failure(503, "Cached child history loading is busy. Retry shortly."); return; }
	}
	const std::string target_id = target == nullptr ? std::string{} : target->id;
	const std::filesystem::path data_root = m_app.data_root;
	const ChatDomainService domain;
	const ChatDomainService::BranchHistoryIdentity source_identity = domain.CaptureBranchHistoryIdentity(*source);
	const std::optional<ChatDomainService::BranchHistoryIdentity> target_identity = target == nullptr
	    ? std::nullopt : std::optional(domain.CaptureBranchHistoryIdentity(*target));
	const std::shared_ptr<std::optional<ChatSession>> baseline = cached_passive
	    ? std::shared_ptr<std::optional<ChatSession>>(cached_passive, &cached_passive->baseline)
	    : std::make_shared<std::optional<ChatSession>>();
	*baseline = target == nullptr ? std::nullopt : std::optional(*target);
	const std::optional<std::string> target_storage = target == nullptr
	    ? std::optional<std::string>("") : ChatRepository::ChatStorageRevision(data_root, target_id);
	if (!target_storage || m_app.worktree_operation_chat_ids.contains(source_id) || m_app.worktree_operation_chat_ids.contains(target_id) ||
	    m_app.pending_chat_save_at_by_chat_id.contains(source_id) || m_app.pending_chat_save_at_by_chat_id.contains(target_id))
	{
		cb->Failure(409, "Chat history is changing. Retry after it settles.");
		return;
	}

	const std::string request_key = target_id.empty() ? source_id + "/" + native_id : target_id;
	const std::unordered_map<std::string, std::shared_ptr<std::stop_source>>::const_iterator previous_request = m_nativeHistoryRequests.find(request_key);
	if (previous_request != m_nativeHistoryRequests.end())
	{
		previous_request->second->request_stop();
		const std::unordered_map<std::string, std::weak_ptr<uam::BranchPreparationOwnership>>::const_iterator previous_owner = m_app.branch_preparations.find(source_id);
		if (previous_owner != m_app.branch_preparations.end())
		{
			const std::shared_ptr<uam::BranchPreparationOwnership> owner = previous_owner->second.lock();
			if (owner && owner->cancellation.get_token() == previous_request->second->get_token())
				(void)domain.ConsumeBranchPreparation(m_app, source_id, owner);
		}
	}
	const std::shared_ptr<uam::BranchPreparationOwnership> ownership = domain.BeginBranchPreparation(m_app, source_id, target_id);
	if (!ownership)
	{
		cb->Failure(409, "Chat preparation is still running. Retry after it settles.");
		return;
	}
	const std::shared_ptr<std::stop_source> request = std::make_shared<std::stop_source>(ownership->cancellation);
	m_nativeHistoryRequests[request_key] = request;

	ChatSession identity;
	identity.id = source_id;
	identity.created_at = source->created_at;
	identity.metadata_incarnation = source->metadata_incarnation;
	identity.provider_id = source->provider_id;
	identity.native_session_id = source->native_session_id;
	identity.workspace_directory = source->workspace_directory;
	identity.execution_host_id = source->execution_host_id;
	identity.folder_id = source->folder_id;
	identity.workspace_isolation_kind = source->workspace_isolation_kind;
	identity.workspace_source_directory = source->workspace_source_directory;
	identity.workspace_base_ref = source->workspace_base_ref;
	identity.workspace_branch_name = source->workspace_branch_name;
	identity.workspace_worktree_directory = source->workspace_worktree_directory;

	const auto prepared_revision = cached_passive
	    ? std::shared_ptr<std::string>(cached_passive, &cached_passive->revision)
	    : std::make_shared<std::string>();
	auto snapshot = cached_passive
	    ? std::shared_ptr<ChatSession>(cached_passive, &cached_passive->snapshot)
	    : std::make_shared<ChatSession>();
	snapshot->id = native_id;
	snapshot->native_session_id = native_id;
	snapshot->provider_id = provider_id;
	snapshot->workspace_directory = source->workspace_directory;
	snapshot->folder_id = source->folder_id;
	snapshot->execution_host_id = source->execution_host_id;
	snapshot->title = uam::strings::NonEmptyOrFallback(uam::strings::Trim(payload.value("title", "")), native_id);
	snapshot->created_at = target == nullptr ? uam::time::TimestampNow() : target->created_at;
	snapshot->updated_at = target == nullptr ? snapshot->created_at : target->updated_at;
	if (!CefPostTask(TID_FILE_USER_BLOCKING, new CefQueryWorkerTask(m_asyncLifetime, cb,
	    [snapshot, prepared_revision, remote_host, request, profile_snapshot = *provider, baseline, data_root, identity, target_storage, target_identity,
	     settings = m_app.settings, folders = m_app.folders, resolved = m_app.resolved_native_sessions_by_chat_id,
	     native_history_directory = m_app.native_history_chats_dir, needs_export, payload_native_id = native_id]()
	    {
		    if (request->stop_requested()) return AsyncFailure(409, "A newer history refresh was requested.");
		    if (*baseline)
		    {
			    std::string error;
			    if (ChatRepository::ChatStorageRevision(data_root, (*baseline)->id) != target_storage ||
			        !ChatRepository::HydrateChatMessages(data_root, **baseline, &error) ||
			        !ChatDomainService().MatchesBranchHistoryIdentity(**baseline, *target_identity))
				    return AsyncFailure(409, uam::strings::NonEmptyOrFallback(error, "Child history changed while loading."));
		    }

		    if (!needs_export)
		    {
			    *snapshot = **baseline;
			    if (!snapshot->imported_read_only)
				    snapshot->native_session_id = uam::strings::Trim(payload_native_id);
			    snapshot->provider_id = profile_snapshot.id;
		    }
		    else if (uam::native_agents::ParseHistoryId(payload_native_id))
		    {
			    std::string error;
			    auto loaded = uam::native_agents::LoadHistory(identity, payload_native_id, remote_host, &error, request->get_token());
			    if (!loaded)
				    return AsyncFailure(502, uam::strings::NonEmptyOrFallback(error, "The actual native agent transcript is not available yet."));
			    loaded->folder_id = identity.folder_id;
			    *snapshot = std::move(*loaded);
		    }
		    else if (!remote_host && snapshot->provider_id == uam::provider_ids::kCodexCli)
		    {
			    std::string error;
			    std::optional<ChatSession> loaded = ChatHistorySyncService().LoadLocalCodexChildChat(*snapshot, &error);
			    if (!loaded) return AsyncFailure(502, uam::strings::NonEmptyOrFallback(error, "Codex child history is unavailable."));
			    *snapshot = std::move(*loaded);
		    }
		    else if (remote_host && snapshot->provider_id == uam::provider_ids::kCodexCli)
		    {
			    auto transcript = ChatHistorySyncService().LoadRemoteCodexTranscript(*remote_host, *snapshot, request->get_token());
			    if (!transcript.success) return AsyncFailure(502, transcript.error);
			    snapshot->messages = std::move(transcript.messages);
		    }
		    else if (snapshot->provider_id == uam::provider_ids::kOpenCodeCli)
		    {
			    auto transcript = remote_host
			        ? ChatHistorySyncService().LoadRemoteOpenCodeTranscript(*remote_host, *snapshot, profile_snapshot, request->get_token())
			        : ChatHistorySyncService().LoadLocalOpenCodeTranscript(*snapshot, profile_snapshot, request->get_token());
			    if (!transcript.success) return AsyncFailure(502, transcript.error);
			    snapshot->messages = std::move(transcript.messages);
		    }
		    snapshot->messages_loaded = true;
		    snapshot->created_at = uam::strings::NonEmptyOrFallback(snapshot->messages.empty() ? std::string{} : snapshot->messages.front().created_at, snapshot->created_at);
		    snapshot->updated_at = uam::strings::NonEmptyOrFallback(snapshot->messages.empty() ? std::string{} : snapshot->messages.back().created_at, snapshot->updated_at);
		    if (request->stop_requested()) return AsyncFailure(409, "Child history preparation was stopped.");
		    std::vector<ChatSession> local_summaries = ChatRepository::LoadLocalChatSummariesReadOnly(data_root, request->get_token());
		    std::unordered_map<std::string, std::string> revisions;
		    for (ChatSession& local : local_summaries)
		    {
			    if (request->stop_requested()) return AsyncFailure(409, "Child history preparation was stopped.");
			    const std::optional<std::string> revision = ChatRepository::ChatStorageRevision(data_root, local.id);
			    if (!revision) return AsyncFailure(409, "Saved child history is unavailable.");
			    const std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(data_root, local.id, false, nullptr, false);
			    if (!summary || ChatRepository::ChatStorageRevision(data_root, local.id) != revision)
				    return AsyncFailure(409, "Saved child history changed while loading.");
			    revisions.emplace(local.id, *revision);
			    local = *summary;
		    }
		    uam::AppState isolated;
		    isolated.data_root = data_root;
		    isolated.native_history_chats_dir = native_history_directory;
		    isolated.settings = settings;
		    isolated.provider_profiles = {profile_snapshot};
		    isolated.folders = folders;
		    isolated.resolved_native_sessions_by_chat_id = resolved;
		    isolated.chats.push_back(identity);
		    if (*baseline && (*baseline)->id != identity.id) isolated.chats.push_back(**baseline);
		    ChatSession* prepared = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(isolated, identity, profile_snapshot, payload_native_id, false, !needs_export || snapshot->provider_id == uam::provider_ids::kCodexCli || snapshot->provider_id == uam::provider_ids::kOpenCodeCli || snapshot->imported_read_only ? snapshot.get() : nullptr, false, request->get_token(), &local_summaries);
		    if (prepared == nullptr) return AsyncFailure(409, "Child history could not be prepared.");
		    *snapshot = *prepared;
		    snapshot->provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(profile_snapshot.id);
		    // Parent-owned read-only imports bypass the general local overlay.
		    // Retain an existing saved display receipt even if the child was not
		    // present in the live catalogue when this explicit open started.
		    for (const ChatSession& saved : local_summaries)
		    {
			    if (!NativeChildDisplayIdentityMatches(saved, *snapshot))
				    continue;
			    snapshot->subagent_parent_chat_id = saved.subagent_parent_chat_id;
			    snapshot->subagent_parent_created_at = saved.subagent_parent_created_at;
			    // The isolated catalogue omits unrelated saved branch ancestors.
			    // Its normalization cannot override this exact writable UAM record's
			    // branch receipt. Retain only those fields, not execution/recovery state.
			    if (!saved.imported_read_only && !saved.native_session_id.empty() &&
			        !saved.created_at.empty() && saved.created_at == snapshot->created_at &&
			        saved.managed_followup_incarnation == snapshot->managed_followup_incarnation)
			    {
				    snapshot->parent_chat_id = saved.parent_chat_id;
				    snapshot->branch_root_chat_id = saved.branch_root_chat_id;
				    snapshot->branch_from_message_index = saved.branch_from_message_index;
			    }
			    break;
		    }
		    const auto saved_revision = revisions.find(snapshot->id);
		    *prepared_revision = saved_revision == revisions.end() ? "missing" : saved_revision->second;
		    if (ChatRepository::ChatStorageRevision(data_root, snapshot->id) != *prepared_revision)
			    return AsyncFailure(409, "Saved child history changed while loading.");
		    if (*baseline) ChatHistorySyncService().OverlayPreparedLocalHistory(**baseline, *snapshot);
		    if (request->stop_requested() || (*baseline && ChatRepository::ChatStorageRevision(data_root, (*baseline)->id) != target_storage))
			    return AsyncFailure(409, "Child history changed or preparation was stopped.");

		    return AsyncSuccess({});
	    },
	    [this, browser, payload, identity, snapshot, prepared_revision, remote_host, target_id, baseline, target_identity, target_storage,
	     source_identity, data_root, request_key, request, ownership, profile_id = provider->id,
	     resolved_workspace = uam::paths::ResolveWorkspaceRootPath(m_app, *source), native_history_directory = m_app.native_history_chats_dir](AsyncCefResult& result)
	    {
		    const auto pending = m_nativeHistoryRequests.find(request_key);
		    if (pending == m_nativeHistoryRequests.end() || pending->second != request)
		    {
			    result = AsyncFailure(409, "A newer history refresh was requested.");
			    return;
		    }
		    m_nativeHistoryRequests.erase(pending);
		    if (!ChatDomainService().ConsumeBranchPreparation(m_app, identity.id, ownership) || request->stop_requested())
		    {
			    result = AsyncFailure(409, "Child history preparation was stopped or replaced.");
			    return;
		    }

		    if (!result.ok) return;
		    const ChatSession* current = ChatDomainService().FindChatById(m_app, identity.id);
		    if (m_app.data_root != data_root || m_app.worktree_operation_chat_ids.contains(identity.id) || m_app.worktree_operation_chat_ids.contains(target_id) || m_app.pending_chat_save_at_by_chat_id.contains(identity.id) || m_app.pending_chat_save_at_by_chat_id.contains(target_id) || current == nullptr || !ChatDomainService().MatchesBranchHistoryIdentity(*current, source_identity) || uam::paths::ResolveWorkspaceRootPath(m_app, *current) != resolved_workspace || m_app.native_history_chats_dir != native_history_directory || current->provider_id != identity.provider_id || current->native_session_id != identity.native_session_id || current->created_at != identity.created_at || current->metadata_incarnation != identity.metadata_incarnation || current->workspace_directory != identity.workspace_directory || current->execution_host_id != identity.execution_host_id || current->folder_id != identity.folder_id)
		    {
			    result = AsyncFailure(409, "Source chat changed while its child history was loading.");
			    return;
		    }
		    if (remote_host)
		    {
			    const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, remote_host->id);
			    if (host == nullptr || host->transport != remote_host->transport ||
			        host->ssh_alias != remote_host->ssh_alias || host->platform != remote_host->platform ||
			        host->runner_directory != remote_host->runner_directory ||
			        host->runner_version != remote_host->runner_version ||
			        host->runner_protocol_version != remote_host->runner_protocol_version)
			    {
				    result = AsyncFailure(409, "Remote host changed while its child history was loading.");
				    return;
			    }
		    }
		    const ProviderProfile& current_provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *current);
		    if (current_provider.id != profile_id)
		    {
			    result = AsyncFailure(409, "Provider changed while child history was loading.");
			    return;
		    }
		    ChatSession* current_target = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *current, current_provider, snapshot->native_session_id.empty() && snapshot->imported_read_only ? snapshot->id : snapshot->native_session_id, false);
		    if ((current_target == nullptr ? std::string{} : current_target->id) != target_id)
		    {
			    result = AsyncFailure(409, "Child chat changed while its history was loading. Try again.");
			    return;
		    }
		    if (current_target != nullptr && (!target_identity || !*baseline || current_target->created_at != (*baseline)->created_at || current_target->metadata_incarnation != (*baseline)->metadata_incarnation || !ChatDomainService().MatchesPreparedBranchHistory(*current_target, *target_identity, false, &**baseline) || ChatRepository::ChatStorageRevision(data_root, target_id) != target_storage))
		    {
			    result = AsyncFailure(409, "Child chat changed while its history was loading. Try again.");
			    return;
		    }
		    if (ChatRepository::ChatStorageRevision(data_root, snapshot->id) != *prepared_revision ||
		        m_app.pending_chat_save_at_by_chat_id.contains(snapshot->id) || m_app.worktree_operation_chat_ids.contains(snapshot->id))
		    {
			    result = AsyncFailure(409, "Saved child history changed while loading.");
			    return;
		    }
		    result = FinishOpenNativeSessionChat(browser, payload, snapshot.get(), true, *prepared_revision);

	    })))
	{
		m_nativeHistoryRequests.erase(request_key);
		(void)domain.ConsumeBranchPreparation(m_app, source_id, ownership);
		cb->Failure(503, "The background task queue is unavailable.");
	}
}

uam::query_handler_async::AsyncCefResult UamQueryHandler::FinishOpenNativeSessionChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, const ChatSession* native_snapshot, bool native_snapshot_prepared, const std::string& expected_storage_revision)
{
	using namespace uam::query_handler_async;
	const std::string source_chat_id = payload.value("chatId", "");
	const std::string native_session_id = uam::strings::Trim(payload.value("nativeSessionId", ""));
	const bool select_chat = payload.value("selectChat", true);
	if (native_session_id.empty())
	{
		return AsyncFailure(400, "A native session id is required.");
	}

	ChatSession* source_chat = ChatDomainService().FindChatById(m_app, source_chat_id);
	if (source_chat == nullptr)
	{
		return AsyncFailure(404, "Source chat not found: " + source_chat_id);
	}
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *source_chat);
	if (!ProviderRuntime::UsesNativeOverlayHistory(provider) && !ProviderRuntime::UsesLocalHistory(provider))
	{
		return AsyncFailure(409, "This provider does not expose a native or local session history path.");
	}

	const std::string source_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider.id);
	const auto agent_reference = uam::native_agents::ParseHistoryId(native_session_id);
	const bool readonly_native_agent = agent_reference && agent_reference->provider_id == source_provider_id && agent_reference->parent_session_id == source_chat->native_session_id && agent_reference->execution_host_id == (source_chat->execution_host_id.empty() ? "local" : source_chat->execution_host_id);
	if (!uam::paths::IsControllerLocalWorkspace(*source_chat) && source_provider_id != uam::provider_ids::kCodexCli && source_provider_id != uam::provider_ids::kOpenCodeCli && !readonly_native_agent)
		return AsyncFailure(409, "Remote child history is unavailable for this provider.");


	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	struct PreserveDesktopSelection
	{
		uam::AppState& app;
		bool preserve;
		std::string chat_id;
		std::string composer;
		void Restore() const
		{
			if (!preserve) return;
			app.selected_chat_index = ChatDomainService().FindChatIndexById(app, chat_id);
			app.composer_text = composer;
			ChatDomainService().RefreshRememberedSelection(app);
		}
		~PreserveDesktopSelection() { Restore(); }
	} desktop_selection{m_app, !select_chat, previous_selected_chat_id, select_chat ? std::string{} : m_app.composer_text};

	const std::string source_created_at = source_chat->created_at;
	const auto source_incarnation = source_chat->metadata_incarnation;
	const auto previous_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(source_chat_id);
	const bool had_previous_resolved_native_session = previous_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
	const std::string previous_resolved_native_session_id = had_previous_resolved_native_session ? previous_resolved_native_session->second : std::string{};
	ChatSession* target_chat = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false);
	std::optional<ChatSession> previous_target_chat;
	if (target_chat != nullptr) previous_target_chat = *target_chat;

	bool inserted_chat = false;
	std::string target_chat_id;
	bool had_previous_target_resolved_native_session = false;
	std::string previous_target_resolved_native_session_id;
	if (target_chat == nullptr)
	{
		target_chat = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false, native_snapshot, native_snapshot_prepared);
		if (target_chat == nullptr)
		{
			return AsyncFailure(404, "Sub-agent chat not found in native history.");
		}
		inserted_chat = true;
		target_chat_id = target_chat->id;
	}
	else
	{
		target_chat_id = target_chat->id;
		const auto previous_target_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(target_chat_id);
		had_previous_target_resolved_native_session = previous_target_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
		previous_target_resolved_native_session_id = had_previous_target_resolved_native_session ? previous_target_resolved_native_session->second : std::string{};
	}

	if (!inserted_chat && (payload.value("refreshHistory", !select_chat) || native_snapshot != nullptr))
	{
		target_chat = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false, native_snapshot, native_snapshot_prepared);
		if (target_chat == nullptr)
		{
			return AsyncFailure(404, "Sub-agent chat history is unavailable.");
		}
		if (target_chat->id != target_chat_id)
		{
			// A conflicting resolved link can yield a new import instead of the hinted chat.
			inserted_chat = true;
			target_chat_id = target_chat->id;
			previous_target_chat.reset();
		}
	}
	if (!(readonly_native_agent && target_chat->imported_read_only && target_chat->native_session_id.empty()))
		m_app.resolved_native_sessions_by_chat_id[target_chat->id] = native_session_id;
	if (target_chat->provider_id.empty())
	{
		target_chat->provider_id = source_provider_id;
	}
	// Refreshing passive native journals replaces their transcript snapshot. Keep
	// the existing UAM display receipt rather than adopting external metadata.
	if (previous_target_chat && !inserted_chat)
	{
		const bool same_identity = NativeChildDisplayIdentityMatches(*previous_target_chat, *target_chat);
		target_chat->subagent_parent_chat_id = same_identity ? previous_target_chat->subagent_parent_chat_id : std::string{};
		target_chat->subagent_parent_created_at = same_identity ? previous_target_chat->subagent_parent_created_at : std::string{};
	}
	// Import can reallocate app.chats; reacquire the actual source UAM record.
	// Stage only inside this save/rollback transaction, with no publication until
	// the chat save succeeds. Ordinary native-session opens gain no lineage.
	const ChatSession* lineage_source = ChatDomainService().FindChatById(m_app, source_chat_id);
	if (lineage_source != nullptr && lineage_source->created_at == source_created_at && lineage_source->metadata_incarnation == source_incarnation)
		(void)ChatRepository::ApplyNativeChildDisplayLineage(*lineage_source, *target_chat, native_session_id);
	if (select_chat)
	{
		ChatDomainService().SelectChatById(m_app, target_chat_id);
	}

	ChatSession* selected_chat = select_chat ? ChatDomainService().SelectedChat(m_app) : target_chat;
	if (selected_chat == nullptr)
	{
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id);
		}
		if (!inserted_chat)
		{
			*target_chat = std::move(*previous_target_chat);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
		}
		ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat_id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		return AsyncFailure(404, "Selected chat no longer exists.");
	}

	if (select_chat)
	{
		selected_chat->last_opened_at = uam::time::TimestampNow();
	}
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		if (select_chat)
		{
			ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		}
		if (!inserted_chat)
		{
			*selected_chat = std::move(*previous_target_chat);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat_id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		}
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id);
		}
		return AsyncFailure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
	}

	const bool chat_saved = expected_storage_revision.empty()
	    ? ChatHistorySyncService().SaveChatWithStatus(m_app, *selected_chat, "", "")
	    : ChatRepository::PublishPreparedChatSave(ChatRepository::PrepareChatSave(m_app.data_root, *selected_chat, expected_storage_revision), true, selected_chat);
	if (!chat_saved)
	{
		if (select_chat)
		{
			ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		}
		if (!inserted_chat)
		{
			*selected_chat = std::move(*previous_target_chat);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat_id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		}
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id);
		}
		(void)PersistenceCoordinator().SaveSettings(m_app);
		return AsyncFailure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
	}

	ChatDomainService().SortChatsByRecent(m_app.chats);
	if (select_chat) ChatDomainService().SelectChatById(m_app, target_chat_id);
	else desktop_selection.Restore();
	uam::PushStateUpdateIfChanged(browser, m_app);
	return AsyncSuccess(nlohmann::json{{"chatId", target_chat_id}});
}

void UamQueryHandler::HandleSetChatModel(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = uam::nlohmann_json::TrimmedStringValueOr(payload, "chatId", "");
	const std::string model_id = uam::nlohmann_json::TrimmedStringValueOr(payload, "modelId", "");
	const std::string model_role = uam::nlohmann_json::TrimmedStringValueOr(payload, "modelRole", "worker");

	if (!IsAllowedModelId(model_id) || (model_role != "worker" && model_role != "reviewer"))
	{
		cb->Failure(400, "Unsupported ACP model selection.");
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	if (model_role == "reviewer")
	{
		if (chat->reviewer_model_id == model_id)
		{
			cb->Success(nlohmann::json{{"reviewerModelId", model_id}}.dump());
			return;
		}
		const std::string previous_model_id = chat->reviewer_model_id;
		const std::string previous_updated_at = chat->updated_at;
		chat->reviewer_model_id = model_id;
		chat->updated_at = uam::time::TimestampNow();
		if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat reviewer model updated.", "Chat reviewer model changed in UI, but failed to save."))
		{
			chat->reviewer_model_id = previous_model_id;
			chat->updated_at = previous_updated_at;
			cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat reviewer model."));
			return;
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"reviewerModelId", model_id}}.dump());
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool defer_live_update = session != nullptr && AcpSessionBlocksModelChange(*session);
	const bool is_copilot = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCopilotCli);
	const bool is_claude = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kClaudeCli);
	const bool is_antigravity = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kAntigravityCli);
	const uam::AcpModelState* selected_model = nullptr;
	std::optional<uam::AcpModelState> cached_selected_model;
	if (session != nullptr)
	{
		const auto found = std::ranges::find_if(session->available_models, [&model_id](const uam::AcpModelState& model) { return model.id == model_id; });
		if (found != session->available_models.end())
		{
			selected_model = &*found;
		}
	}
	if ((is_claude || is_antigravity) && (selected_model == nullptr || is_antigravity) && m_app.provider_model_catalog != nullptr)
	{
		const std::string selected_id = model_id.empty() ? "default" : model_id;
		for (const auto& model : m_app.provider_model_catalog->GetCachedProviderModels(chat->provider_id, uam::paths::ResolveWorkspaceRootPath(m_app, *chat).generic_string(), chat->execution_host_id))
		{
			if (!(is_antigravity && model_id.empty() && model.value("isDefault", false)) && uam::nlohmann_json::TrimmedStringValue(model, {"id"}) != selected_id)
				continue;
			cached_selected_model = uam::acp_models::ParseAcpModelState(model);
			if (cached_selected_model)
				selected_model = &*cached_selected_model;
			break;
		}
	}
	if (is_antigravity && m_app.provider_model_catalog != nullptr)
	{
		const std::string scope_workspace = uam::paths::ResolveWorkspaceRootPath(m_app, *chat).generic_string();
		const std::string native_model = model_id.empty() && selected_model != nullptr ? selected_model->id : model_id;
		const auto* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id.empty() ? "local" : chat->execution_host_id);
		std::string discovery_error;
		if (host != nullptr && !native_model.empty() && !m_app.provider_model_catalog->StartAntigravityEffortDiscovery(ProviderResolutionService().ProviderForChatOrDefault(m_app, *chat), *host, scope_workspace, native_model, &discovery_error))
			m_app.provider_model_catalog->RememberRefreshFailure(chat->provider_id, discovery_error, scope_workspace, host->id);
	}
	const auto& option_runtime = ProviderRuntimeRegistry::ResolveById(chat->provider_id);
	const bool launch_only_options = option_runtime.ModelOptionsRequireRestart();
	if (launch_only_options)
	{
		const auto* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id.empty() ? "local" : chat->execution_host_id);
		cached_selected_model = selected_model != nullptr ? *selected_model : uam::AcpModelState{};
		cached_selected_model->id = model_id;
		cached_selected_model->supported_reasoning_efforts = option_runtime.ModelReasoningEfforts(model_id, host != nullptr ? uam::CheckedProviderConfigurationVersion(m_app, chat->provider_id, *host) : std::string{});
		cached_selected_model->default_reasoning_effort.clear();
		cached_selected_model->additional_speed_tiers.clear();
		selected_model = &*cached_selected_model;
		if (defer_live_update && chat->model_id != model_id && !chat->reasoning_effort.empty())
		{
			cb->Failure(409, "Wait for the active provider request to finish before changing a launch-configured model.");
			return;
		}
	}
	std::string reasoning_effort = chat->reasoning_effort;
	std::string service_tier = chat->service_tier;
	bool service_tier_explicit = chat->service_tier_explicit;
	if ((is_claude || is_antigravity || launch_only_options) && selected_model != nullptr && selected_model->supported_reasoning_efforts.empty())
		reasoning_effort.clear();
	if (selected_model != nullptr && !reasoning_effort.empty() && !selected_model->supported_reasoning_efforts.empty() &&
	    !uam::ranges::Contains(selected_model->supported_reasoning_efforts, reasoning_effort))
	{
		reasoning_effort = launch_only_options ? std::string{} : uam::ranges::Contains(selected_model->supported_reasoning_efforts, selected_model->default_reasoning_effort) ? selected_model->default_reasoning_effort : selected_model->supported_reasoning_efforts.front();
	}
	if (selected_model != nullptr && !service_tier.empty() && !uam::ranges::Contains(selected_model->additional_speed_tiers, service_tier))
	{
		service_tier.clear();
		service_tier_explicit = true;
	}
	const bool copilot_effort_changed = is_copilot && chat->reasoning_effort != reasoning_effort;
	const bool launch_model_restart = launch_only_options && (chat->reasoning_effort != reasoning_effort || (chat->model_id != model_id && (!chat->reasoning_effort.empty() || !reasoning_effort.empty())));
	if (defer_live_update && copilot_effort_changed)
	{
		cb->Failure(409, "Wait for the active Copilot request to finish before changing to a model with a different effort.");
		return;
	}

	if (chat->model_id == model_id && chat->reasoning_effort == reasoning_effort && chat->service_tier == service_tier && chat->service_tier_explicit == service_tier_explicit)
	{
		if (!defer_live_update && session != nullptr && session->running && !model_id.empty() && session->current_model_id != model_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"modelId", model_id}, {"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
		return;
	}
	if (!uam::EnsureAcpStopProgress(m_app, chat->id))
	{
		cb->Failure(409, "The runtime is stopping; retry shortly.");
		return;
	}
	if (launch_model_restart && !StopLaunchOptionTerminal(m_app, chat->id))
	{
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The native terminal is stopping; retry shortly."));
		return;
	}
	if (!defer_live_update && session != nullptr && session->running && (model_id.empty() || copilot_effort_changed || launch_model_restart) && !uam::StopAcpSession(m_app, chat->id))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The runtime is stopping; retry shortly."));
		return;
	}

	const std::string previous_model_id = chat->model_id;
	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const bool previous_service_tier_explicit = chat->service_tier_explicit;
	const std::string previous_updated_at = chat->updated_at;
	chat->model_id = model_id;
	chat->reasoning_effort = reasoning_effort;
	chat->service_tier = service_tier;
	chat->service_tier_explicit = service_tier_explicit;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model updated.", "Chat model changed in UI, but failed to save."))
	{
		chat->model_id = previous_model_id;
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->service_tier_explicit = previous_service_tier_explicit;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat model."));
		return;
	}

	if (!defer_live_update && session != nullptr && session->running)
	{
		std::string acp_error;
		const bool live_updated = model_id.empty() || copilot_effort_changed || launch_model_restart ? uam::StopAcpSession(m_app, chat->id) : uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error, previous_model_id);
		if (!live_updated)
		{
			chat->model_id = previous_model_id;
			chat->reasoning_effort = previous_reasoning_effort;
			chat->service_tier = previous_service_tier;
			chat->service_tier_explicit = previous_service_tier_explicit;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model reverted.", "Chat model changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"modelId", model_id}, {"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
}

void UamQueryHandler::HandleSetChatCodexOptions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested_reasoning_effort = payload.value("reasoningEffort", "");
	std::string service_tier = uam::codex::NormalizeServiceTier(payload.value("serviceTier", ""));

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool is_codex = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCodexCli);
	const bool is_copilot = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCopilotCli);
	const bool is_claude = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kClaudeCli);
	const bool is_antigravity = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kAntigravityCli);
	bool service_tier_explicit = (is_codex || is_claude) && (payload.contains("serviceTierExplicit") ? payload.value("serviceTierExplicit", false) : (payload.contains("serviceTier") || (is_claude && chat->service_tier_explicit)));
	if (is_claude && !payload.contains("serviceTier"))
		service_tier = chat->service_tier;
	if (is_claude && !service_tier_explicit)
		service_tier.clear();
	std::string reasoning_effort = uam::provider_chat_defaults::NormalizeReasoningEffort(chat->provider_id, is_claude && !payload.contains("reasoningEffort") ? chat->reasoning_effort : requested_reasoning_effort);
	if (session != nullptr && uam::AcpSessionHasBlockingRuntimeWork(*session))
	{
		cb->Failure(409, "Wait for the active provider request to finish before changing model options.");
		return;
	}
	const uam::AcpModelState* selected_model = nullptr;
	std::optional<uam::AcpModelState> cached_selected_model;
	if (session != nullptr)
	{
		const std::string selected_model_id = chat->model_id.empty() ? session->current_model_id : chat->model_id;
		for (const uam::AcpModelState& model : session->available_models)
		{
			if (model.id == selected_model_id)
			{
				selected_model = &model;
				break;
			}
		}
	}
	if ((is_claude || is_antigravity) && (selected_model == nullptr || is_antigravity) && m_app.provider_model_catalog != nullptr)
	{
		const std::string selected_id = chat->model_id.empty() ? "default" : chat->model_id;
		for (const auto& model : m_app.provider_model_catalog->GetCachedProviderModels(chat->provider_id, uam::paths::ResolveWorkspaceRootPath(m_app, *chat).generic_string(), chat->execution_host_id))
		{
			if (!(is_antigravity && chat->model_id.empty() && model.value("isDefault", false)) && uam::nlohmann_json::TrimmedStringValue(model, {"id"}) != selected_id)
				continue;
			cached_selected_model = uam::acp_models::ParseAcpModelState(model);
			if (cached_selected_model)
				selected_model = &*cached_selected_model;
			break;
		}
	}
	const auto& option_runtime = ProviderRuntimeRegistry::ResolveById(chat->provider_id);
	const bool launch_only_options = option_runtime.ModelOptionsRequireRestart();
	if (launch_only_options)
	{
		const auto* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id.empty() ? "local" : chat->execution_host_id);
		cached_selected_model = selected_model != nullptr ? *selected_model : uam::AcpModelState{};
		cached_selected_model->id = chat->model_id;
		cached_selected_model->supported_reasoning_efforts = option_runtime.ModelReasoningEfforts(chat->model_id, host != nullptr ? uam::CheckedProviderConfigurationVersion(m_app, chat->provider_id, *host) : std::string{});
		cached_selected_model->default_reasoning_effort.clear();
		cached_selected_model->additional_speed_tiers.clear();
		selected_model = &*cached_selected_model;
		if ((!requested_reasoning_effort.empty() && reasoning_effort.empty()) || (!reasoning_effort.empty() && !uam::ranges::Contains(selected_model->supported_reasoning_efforts, reasoning_effort)))
		{
			cb->Failure(409, "This concrete model and checked executing-host CLI version do not support that thinking level.");
			return;
		}
	}
	if (!is_codex && !is_copilot && !((is_claude || launch_only_options) && reasoning_effort.empty()) && (selected_model == nullptr || selected_model->supported_reasoning_efforts.empty()))
	{
		cb->Failure(409, "This provider model does not expose reasoning-effort options.");
		return;
	}
	if (selected_model != nullptr && !reasoning_effort.empty() && !selected_model->supported_reasoning_efforts.empty())
	{
		const auto requested = std::ranges::find(selected_model->supported_reasoning_efforts, reasoning_effort);
		if (requested == selected_model->supported_reasoning_efforts.end())
		{
			reasoning_effort = uam::ranges::Contains(selected_model->supported_reasoning_efforts, selected_model->default_reasoning_effort)
			                       ? selected_model->default_reasoning_effort
			                       : selected_model->supported_reasoning_efforts.front();
		}
	}
	if (!is_codex && !is_claude)
	{
		service_tier.clear();
		service_tier_explicit = false;
	}
	if ((is_codex || is_claude) && !service_tier.empty() && ((is_claude && (service_tier != "fast" || selected_model == nullptr)) || (selected_model != nullptr && !uam::ranges::Contains(selected_model->additional_speed_tiers, service_tier))))
	{
		cb->Failure(409, "The selected model does not support that speed tier.");
		return;
	}
	if (chat->reasoning_effort == reasoning_effort && chat->service_tier == service_tier && chat->service_tier_explicit == service_tier_explicit)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
		return;
	}
	if (!uam::EnsureAcpStopProgress(m_app, chat->id))
	{
		cb->Failure(409, "The runtime is stopping; retry shortly.");
		return;
	}
	if (launch_only_options && !StopLaunchOptionTerminal(m_app, chat->id))
	{
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The native terminal is stopping; retry shortly."));
		return;
	}
	const bool needs_effort_restart = is_copilot || is_claude || is_antigravity || launch_only_options;
	if (needs_effort_restart && session != nullptr && session->running && !uam::StopAcpSession(m_app, chat->id))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The runtime is stopping; retry shortly."));
		return;
	}

	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const bool previous_service_tier_explicit = chat->service_tier_explicit;
	const std::string previous_updated_at = chat->updated_at;
	chat->reasoning_effort = reasoning_effort;
	chat->service_tier = service_tier;
	chat->service_tier_explicit = service_tier_explicit;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model options updated.", "Chat model options changed in UI, but failed to save."))
	{
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->service_tier_explicit = previous_service_tier_explicit;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist Codex chat options."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
}

void UamQueryHandler::HandleSetChatProvider(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = ProviderSwitchChatIdFromPayload(payload);
	const std::string provider_id = ProviderSwitchProviderIdFromPayload(payload);

	switch (uam::SwitchChatProvider(m_app, chat_id, provider_id))
	{
	case uam::ChatProviderSwitchResult::UnsupportedProvider:
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	case uam::ChatProviderSwitchResult::ChatNotFound:
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	case uam::ChatProviderSwitchResult::ActiveRuntime:
		cb->Failure(409, "Cannot change provider while a runtime turn or input request is active.");
		return;
	case uam::ChatProviderSwitchResult::RuntimeStopping:
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The previous runtime is stopping; retry shortly."));
		return;
	case uam::ChatProviderSwitchResult::SaveFailed:
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat provider."));
		return;
	case uam::ChatProviderSwitchResult::Changed:
	case uam::ChatProviderSwitchResult::Unchanged:
		break;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatApprovalMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string mode_id = uam::approval_modes::NormalizeIncomingApprovalModeId(payload.value("modeId", ""));

	if (!uam::approval_modes::IsAgentMode(mode_id))
	{
		cb->Failure(400, "Unsupported ACP mode: " + mode_id);
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool defer_live_update = session != nullptr && AcpSessionBlocksModelChange(*session);
	const std::string effective_mode_id = uam::approval_modes::EffectiveProviderMode(mode_id, chat->command_safety_tier);

	if (chat->approval_mode == mode_id)
	{
		if (!defer_live_update && session != nullptr && session->running && session->current_mode_id != effective_mode_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionMode(m_app, chat->id, effective_mode_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"approvalMode", chat->approval_mode}, {"currentModeId", session == nullptr ? effective_mode_id : session->current_mode_id}}.dump());
		return;
	}

	const std::string previous_mode_id = chat->approval_mode;
	const std::string previous_updated_at = chat->updated_at;
	chat->approval_mode = mode_id;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode updated.", "Chat mode changed in UI, but failed to save."))
	{
		chat->approval_mode = previous_mode_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat mode."));
		return;
	}

	if (!defer_live_update && session != nullptr && session->running)
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat->id, effective_mode_id, &acp_error, previous_mode_id))
		{
			chat->approval_mode = previous_mode_id;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode reverted.", "Chat mode changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"approvalMode", chat->approval_mode}, {"currentModeId", session == nullptr ? effective_mode_id : session->current_mode_id}}.dump());
}

void UamQueryHandler::HandleSetChatUamAgent(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string agent_id = uam::strings::NonEmptyOrFallback(uam::strings::Trim(payload.value("agentId", "")), "build");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!chat->agent_run_id.empty())
	{
		cb->Failure(403, "Managed child chats cannot change their assigned UAM agent.");
		return;
	}
	const uam::AgentDefinitionCatalog agents = uam::AgentDefinitionService::Load(
	    m_app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat));
	const auto selected = std::ranges::find(agents.definitions, agent_id, &uam::AgentDefinition::id);
	if (selected == agents.definitions.end() || (selected->mode != "primary" && selected->mode != "both"))
	{
		cb->Failure(400, "UAM agent is unavailable for primary chat use: " + agent_id);
		return;
	}
	if (chat->uam_agent_id == agent_id)
	{
		cb->Success(nlohmann::json{{"uamAgentId", agent_id}}.dump());
		return;
	}
	const std::string previous_agent_id = chat->uam_agent_id;
	const std::string previous_updated_at = chat->updated_at;
	chat->uam_agent_id = agent_id;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "UAM agent updated.", "UAM agent changed in UI, but failed to save."))
	{
		chat->uam_agent_id = previous_agent_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist the UAM agent."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"uamAgentId", chat->uam_agent_id}}.dump());
}

void UamQueryHandler::HandleSetChatUamControlEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::optional<bool> enabled = uam::nlohmann_json::BoolFieldStrict(payload, "enabled");
	if (!enabled.has_value())
	{
		cb->Failure(400, "Agent goal control requires an explicit boolean enabled value.");
		return;
	}
	const std::optional<bool> next_launch = uam::nlohmann_json::BoolFieldStrict(payload, "applyOnNextLaunch");
	if ((payload.contains("applyOnNextLaunch") && !next_launch.has_value()) || (next_launch.value_or(false) && !*enabled))
	{
		cb->Failure(400, "Next-launch setup requires an explicit boolean and supports enabling UAM tools only.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!chat->agent_run_id.empty())
	{
		cb->Failure(403, "Managed child chats cannot change UAM Control authority.");
		return;
	}
	const ProviderProfile* provider = ProviderResolutionService().ProviderForChat(m_app, *chat);
	if (*enabled && (provider == nullptr ||
	                 !uam::UamControlService::SupportsStructuredProtocol(provider->structured_protocol)))
	{
		cb->Failure(409, "This provider's structured protocol cannot attach UAM Control. Supported providers are Codex, Claude Code, Gemini CLI, OpenCode, and GitHub Copilot CLI.");
		return;
	}
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (*enabled && uam::UamControlService::RequiresSessionStopForEnable(session, next_launch.value_or(false)))
	{
		cb->Failure(409, "Stop the current provider session before enabling agent goal control.");
		return;
	}
	if (chat->uam_control_enabled == *enabled && chat->uam_control_explicitly_disabled == !*enabled)
	{
		if (!*enabled)
		{
			uam::UamControlService::RevokeForChat(m_app, chat->id);
			uam::PushStateUpdateIfChanged(browser, m_app);
		}
		cb->Success(nlohmann::json{{"enabled", *enabled}}.dump());
		return;
	}

	const bool previous = chat->uam_control_enabled;
	const bool previous_explicitly_disabled = chat->uam_control_explicitly_disabled;
	const std::string previous_updated_at = chat->updated_at;
	chat->uam_control_enabled = *enabled;
	chat->uam_control_explicitly_disabled = !*enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Agent goal control updated.",
	                                                "Agent goal control changed in UI, but failed to save."))
	{
		chat->uam_control_enabled = previous;
		chat->uam_control_explicitly_disabled = previous_explicitly_disabled;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist agent goal control."));
		return;
	}
	if (!*enabled) uam::UamControlService::RevokeForChat(m_app, chat->id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"enabled", chat->uam_control_enabled}}.dump());
}

void UamQueryHandler::HandleListUamAgents(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;

	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(
	    m_app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat));
	nlohmann::json agents = nlohmann::json::array();
	for (const uam::AgentDefinition& agent : catalog.definitions)
	{
		if (agent.mode != "primary" && agent.mode != "both") continue;
		agents.push_back({
		    {"id", agent.id},
		    {"description", agent.description},
		    {"builtIn", agent.built_in},
		});
	}
	cb->Success(nlohmann::json{{"agents", std::move(agents)}, {"errors", catalog.errors}}.dump());
}

namespace
{
	nlohmann::json SerializeProviderAgentImportPreview(const uam::ProviderAgentImportPreview& preview)
	{
		return {
		    {"providerId", preview.provider_id},
		    {"sourcePath", uam::paths::Utf8PathString(preview.source_path)},
		    {"suggestedId", preview.suggested_id},
		    {"description", preview.description},
		    {"mode", preview.mode},
		    {"securityFields", preview.security_fields},
		    {"ignoredFields", preview.ignored_fields},
		    {"error", preview.error},
		    {"supported", preview.supported},
		};
	}
}

void UamQueryHandler::HandleBrowseProviderAgentImport(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(payload.value("currentValue", ""));
	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::File, initial_path, &selected_path, &error))
	{
		if (!error.empty()) cb->Failure(500, error);
		else cb->Success(nlohmann::json{{"selectedPath", ""}}.dump());
		return;
	}
	cb->Success(nlohmann::json{{"selectedPath", selected_path}}.dump());
}

void UamQueryHandler::HandlePreviewProviderAgentImport(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(
	    payload.value("providerId", ""), uam::paths::PathFromUtf8(payload.value("sourcePath", "")));
	cb->Success(SerializeProviderAgentImportPreview(preview).dump());
}

void UamQueryHandler::HandleListAgentDefinitions(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(m_app.data_root, {});
	nlohmann::json agents = nlohmann::json::array();
	for (const uam::AgentDefinition& agent : catalog.definitions)
		agents.push_back({{"id", agent.id}, {"description", agent.description}, {"mode", agent.mode}, {"workspaceAccess", agent.workspace_access},
		                  {"skills", agent.skills}, {"delegates", agent.delegates}, {"instructions", agent.instructions}, {"builtIn", agent.built_in}});
	cb->Success(nlohmann::json{{"agents", std::move(agents)}, {"errors", catalog.errors}}.dump());
}

void UamQueryHandler::HandleSaveAgentDefinition(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	uam::AgentDefinition agent;
	try
	{
		agent.id = payload.at("id").get<std::string>();
		agent.description = payload.at("description").get<std::string>();
		agent.mode = payload.at("mode").get<std::string>();
		agent.workspace_access = payload.at("workspaceAccess").get<std::string>();
		agent.skills = payload.value("skills", std::vector<std::string>{});
		agent.delegates = payload.value("delegates", std::vector<std::string>{});
		agent.instructions = payload.at("instructions").get<std::string>();
	}
	catch (const nlohmann::json::exception&) { cb->Failure(400, "Agent fields are missing or invalid."); return; }
	std::string error;
	if (!uam::AgentDefinitionService::SaveGlobalAgent(m_app.data_root, agent, &error)) { cb->Failure(400, error); return; }
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteAgentDefinition(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string error;
	if (!uam::AgentDefinitionService::DeleteGlobalAgent(m_app.data_root, payload.value("id", ""), &error)) { cb->Failure(400, error); return; }
	cb->Success("{}");
}

void UamQueryHandler::HandleImportProviderAgent(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	uam::ProviderAgentImportRequest request;
	std::filesystem::path workspace_root;
	try
	{
		const std::string chat_id = payload.value("chatId", "");
		request.provider_id = payload.value("providerId", "");
		request.source_path = uam::paths::PathFromUtf8(payload.value("sourcePath", ""));
		request.canonical_id = payload.value("canonicalId", "");
		request.workspace_access = payload.value("workspaceAccess", "");
		request.skills = payload.value("skills", std::vector<std::string>{});
		request.workspace_scope = payload.value("workspaceScope", false);
		request.acknowledge_ignored_fields = payload.value("acknowledgeIgnoredFields", false);
		if (request.workspace_scope)
		{
			ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Open a workspace chat before importing an agent.");
			if (chat == nullptr) return;
			workspace_root = uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat);
		}
	}
	catch (const nlohmann::json::exception&)
	{
		cb->Failure(400, "Agent import fields are missing or invalid.");
		return;
	}

	uam::AgentDefinition imported;
	std::string error;
	if (!uam::AgentDefinitionService::ImportProviderAgent(
	        m_app.data_root, workspace_root, request, &imported, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Provider agent import failed."));
		return;
	}
	cb->Success(nlohmann::json{{"id", imported.id}, {"description", imported.description}, {"mode", imported.mode}}.dump());
}

void UamQueryHandler::HandleGetManagedAgentTranscript(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	const std::string root_chat_id = payload.value("chatId", "");
	const std::string transcript_chat_id = payload.value("transcriptChatId", "");
	const ChatSession* root_chat = ChatDomainService().FindChatById(m_app, root_chat_id);
	if (root_chat == nullptr || !root_chat->agent_run_id.empty())
	{
		cb->Failure(404, "Managed agent transcript parent chat is unavailable.");
		return;
	}
	const auto inspection = uam::CaptureManagedAgentTranscriptRead(m_app, root_chat_id, transcript_chat_id);
	if (inspection && !inspection->history_error.empty())
	{
		cb->Failure(404, inspection->history_error);
		return;
	}
	if (inspection && inspection->initial_history)
	{
		const std::string key = "history:" + root_chat_id + ":" + transcript_chat_id;
		if (m_managedSubagentQueries.contains(key) || m_managedSubagentQueries.size() >= 8)
		{
			cb->Failure(409, "A bounded saved managed history query is already pending.");
			return;
		}
		const auto operation = std::make_shared<const char>(0);
		m_managedSubagentQueries.emplace(key, operation);
		auto loaded = std::make_shared<uam::ManagedAgentTranscriptReadResult>();
		auto response = std::make_shared<nlohmann::json>();
		const auto data_root = m_app.data_root;
		if (!DispatchManagedSubagentQuery(
		        cb,
		        [identity = *inspection, data_root, loaded, response]()
		        {
			        *loaded = uam::LoadManagedAgentTranscriptDetached(data_root, identity);
			        if (loaded->status != uam::ManagedAgentTranscriptReadStatus::Ready)
				        return AsyncFailure(404, loaded->error);
			        const auto serialized = uam::StateSerializer::SerializeSession(loaded->transcript);
			        *response = {{"runId", identity.run_id}, {"agentId", identity.run_agent_id}, {"status", identity.run_status}, {"providerId", identity.run_provider_id}, {"executionCapability", identity.run_execution_capability}, {"title", loaded->transcript.title}, {"messages", serialized.value("messages", nlohmann::json::array())}, {"lastReported", true}};
			        return AsyncSuccess({});
		        },
		        [this, identity = *inspection, key, operation, data_root, loaded, response](AsyncCefResult& result)
		        {
			        const auto pending = m_managedSubagentQueries.find(key);
			        if (pending == m_managedSubagentQueries.end() || pending->second != operation)
			        {
				        result = AsyncFailure(409, "Saved managed history query was replaced.");
				        return;
			        }
			        m_managedSubagentQueries.erase(pending);
			        if (!result.ok)
				        return;
			        if (m_app.data_root != data_root || !uam::MatchesManagedAgentTranscriptRead(m_app, identity, loaded->transcript))
			        {
				        result = AsyncFailure(409, "Saved managed conversation ownership changed while reading.");
				        return;
			        }
			        result = AsyncSuccess(std::move(*response));
		        }))
			m_managedSubagentQueries.erase(key);
		return;
	}
	const auto run = std::ranges::find_if(m_app.agent_runs, [&](const AgentRun& candidate) {
		return candidate.root_chat_id == root_chat_id && candidate.transcript_chat_id == transcript_chat_id;
	});
	if (run == m_app.agent_runs.end())
	{
		cb->Failure(404, "Managed agent transcript is outside this chat or no longer exists.");
		return;
	}
	ChatSession* transcript = ChatDomainService().FindChatById(m_app, transcript_chat_id);
	if (m_workspaceDeletionPending && m_app.worktree_operation_chat_ids.contains(transcript_chat_id))
	{
		cb->Failure(409, "Managed agent transcript is being deleted.");
		return;
	}
	if (transcript != nullptr && transcript->agent_run_id != run->id)
	{
		cb->Failure(404, "Managed agent transcript is unavailable.");
		return;
	}
	if (transcript != nullptr && !transcript->messages_loaded)
	{
		const std::optional<uam::ManagedAgentTranscriptReadIdentity> identity =
		    uam::CaptureManagedAgentTranscriptRead(m_app, root_chat_id, transcript_chat_id);
		if (identity)
		{
			if (m_nativeHistoryRequests.contains(transcript_chat_id))
			{
				cb->Failure(409, "Managed agent transcript history is already loading.");
				return;
			}
			const std::shared_ptr<std::stop_source> request = std::make_shared<std::stop_source>();
			m_nativeHistoryRequests[transcript_chat_id] = request;
			const std::filesystem::path data_root = m_app.data_root;
			auto read_result = std::make_shared<uam::ManagedAgentTranscriptReadResult>();
			auto detached_response = std::make_shared<nlohmann::json>();
			if (!RunAsyncCefQuery(m_asyncLifetime, cb,
			    [data_root, identity = *identity, request, read_result, detached_response]()
			    {
				    if (request->stop_requested())
					    return AsyncFailure(409, "A newer transcript history request was made.");
				    *read_result = uam::LoadManagedAgentTranscriptDetached(data_root, identity);
				    if (read_result->status == uam::ManagedAgentTranscriptReadStatus::Ready)
				    {
					    const nlohmann::json serialized = uam::StateSerializer::SerializeSession(read_result->transcript);
					    *detached_response = {
					        {"runId", identity.run_id}, {"agentId", identity.run_agent_id}, {"status", identity.run_status},
					        {"providerId", identity.run_provider_id}, {"executionCapability", identity.run_execution_capability},
					        {"resumedFromRunId", identity.run_resumed_from_run_id},
					        {"title", read_result->transcript.title},
					        {"messages", serialized.value("messages", nlohmann::json::array())}};
					    return AsyncSuccess({});
				    }
				    return AsyncFailure(read_result->status == uam::ManagedAgentTranscriptReadStatus::Unavailable ? 404 : 409,
				        uam::strings::NonEmptyOrFallback(read_result->error, "Managed agent transcript is unavailable."));
			    },
		    [this, identity = *identity, request, read_result, detached_response, transcript_chat_id](AsyncCefResult& response)
			    {
				    const auto pending = m_nativeHistoryRequests.find(transcript_chat_id);
				    if (pending == m_nativeHistoryRequests.end() || pending->second != request)
				    {
					    response = AsyncFailure(409, "Managed agent transcript changed while it was loading.");
					    return;
				    }
				    m_nativeHistoryRequests.erase(pending);
				    if (!response.ok) return;
				    if (!uam::MatchesManagedAgentTranscriptRead(m_app, identity, read_result->transcript))
				    {
					    response = AsyncFailure(409, "Managed agent transcript changed while it was loading.");
					    return;
				    }
				    if (const auto hint = ManagedFollowupHint(identity.root_chat_id, identity.run_id, &read_result->transcript)) (*detached_response)["followup"] = *hint;
				    response = AsyncSuccess(std::move(*detached_response));
			    }))
			{
				const auto pending = m_nativeHistoryRequests.find(transcript_chat_id);
				if (pending != m_nativeHistoryRequests.end() && pending->second == request)
					m_nativeHistoryRequests.erase(pending);
				return;
			}
			return;
		}
		cb->Failure(409, "Managed agent transcript is active or changed. Retry after it settles.");
		return;
	}
	std::string hydrate_warning;
	std::optional<ChatSession> loaded_transcript;
	if (transcript == nullptr)
	{
		loaded_transcript = ChatRepository::LoadLocalChat(m_app.data_root, transcript_chat_id, true, &hydrate_warning);
		if (!loaded_transcript)
		{
			cb->Failure(404, FailureDetailOrFallback(hydrate_warning, "Managed agent transcript is unavailable."));
			return;
		}
		transcript = &*loaded_transcript;
		if (transcript->agent_run_id != run->id)
		{
			cb->Failure(404, "Managed agent transcript is unavailable.");
			return;
		}
	}
	if (!transcript->messages_loaded && !ChatRepository::HydrateChatMessages(m_app.data_root, *transcript, &hydrate_warning))
	{
		cb->Failure(500, FailureDetailOrFallback(hydrate_warning, "Failed to load the managed agent transcript."));
		return;
	}
	const nlohmann::json serialized = uam::StateSerializer::SerializeSession(*transcript);
	nlohmann::json response{
	    {"runId", run->id}, {"agentId", run->agent_id}, {"status", run->status},
	    {"providerId", run->provider_id}, {"executionCapability", run->execution_capability},
	    {"resumedFromRunId", run->resumed_from_run_id},
	    {"title", transcript->title}, {"messages", serialized.value("messages", nlohmann::json::array())}};
	if (const auto hint = ManagedFollowupHint(root_chat_id, run->id, transcript)) response["followup"] = *hint;
	cb->Success(response.dump());
}

void UamQueryHandler::HandleResumeAgentRun(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string new_run_id;
	std::string error;
	if (!uam::AgentRunScheduler::ResumeInterrupted(
	        m_app, payload.value("runId", ""), &new_run_id, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "Managed run could not be resumed."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"runId", new_run_id}, {"status", "queued"}}.dump());
}

void UamQueryHandler::HandleSetChatCommandSafetyTier(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string raw_requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("commandSafetyTier", "")));
	if (raw_requested != "off" && raw_requested != "acceptedits" && raw_requested != "aireview" && raw_requested != "yolo")
	{
		cb->Failure(400, "Permission mode must be default, accept edits, AI Review, or YOLO.");
		return;
	}
	const std::string requested = raw_requested == "acceptedits" ? uam::approval_modes::kAcceptEditsApprovalMode : raw_requested == "aireview" ? "aiReview" : raw_requested;
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	const auto auto_approve_pending = [&]()
	{
		uam::AcpSessionState* pending_session = uam::FindAcpSessionForChat(m_app, chat->id);
		if (pending_session == nullptr || pending_session->pending_permission.request_id_json.empty()) return;
		std::string acp_error;
		if (uam::TryAutoApprovePendingAcpPermission(m_app, chat->id, &acp_error))
		{
			pending_session->last_error.clear();
		}
		else if (!acp_error.empty())
		{
			pending_session->last_error = acp_error;
		}
	};
	if (chat->command_safety_tier == requested)
	{
		auto_approve_pending();
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"commandSafetyTier", chat->command_safety_tier}}.dump());
		return;
	}

	const std::string previous = chat->command_safety_tier;
	const ChatSession previous_chat = *chat;
	const std::string previous_updated_at = chat->updated_at;
	chat->command_safety_tier = requested;
	const std::string requested_effective_mode = uam::approval_modes::EffectiveProviderMode(chat->approval_mode, requested);
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Command safety tier updated.", "Command safety tier changed in UI, but failed to save."))
	{
		chat->command_safety_tier = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist command safety tier."));
		return;
	}
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (session != nullptr && session->running && !AcpSessionBlocksModelChange(*session) &&
	    CommandSafetyTierNeedsLiveUpdate(previous_chat, *chat))
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat->id, requested_effective_mode, &acp_error, std::nullopt, previous))
		{
			chat->command_safety_tier = previous;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat permissions reverted.", "Chat permissions changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP permissions."));
			return;
		}
	}
	auto_approve_pending();

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"commandSafetyTier", chat->command_safety_tier}}.dump());
}

void UamQueryHandler::HandleSetChatComputerUseEnabled(CefRefPtr<CefBrowser> browser,
    const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", false);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (enabled && !m_app.settings.computer_use_globally_enabled)
	{
		cb->Failure(409, "Computer Use is disabled globally.");
		return;
	}
	if (enabled && !uam::computer_use::AvailableForChat(*chat))
	{
		cb->Failure(409, "Computer Use is disabled for remote execution hosts.");
		return;
	}
	const bool uses_uam_backend = uam::computer_use::UsesUamBackend(*chat);

	if (!enabled)
	{
		if (!HasComputerUseOwnership(m_app, chat_id, chat != nullptr && chat->computer_use_enabled))
		{
			cb->Success("{}");
			return;
		}
		m_app.computer_use_revocation_pending_chat_ids.insert(chat_id);
		chat->computer_use_enabled = false;
		const bool acp_stopped = uam::StopAcpSession(m_app, chat_id);
		const bool terminal_stopped = uam::StopAndEraseCliTerminalForChat(m_app, chat_id);
		chat = ChatDomainService().FindChatById(m_app, chat_id);
		if (chat != nullptr) chat->computer_use_enabled = false;
		std::string error;
		const bool control_stopped = !uses_uam_backend || uam::ComputerUseService::SetControlState(m_app, chat_id, "stopped", &error);
		if (acp_stopped && terminal_stopped && control_stopped)
			m_app.computer_use_revocation_pending_chat_ids.erase(chat_id);
		uam::PushStateUpdateIfChanged(browser, m_app);
		if (m_app.computer_use_revocation_pending_chat_ids.contains(chat_id))
			cb->Failure(500, FailureDetailOrFallback(error, "Computer Use is disabled; its previous controls are still stopping. Retry disabling."));
		else cb->Success("{}");
		return;
	}
	if (m_app.computer_use_revocation_pending_chat_ids.contains(chat_id))
	{
		cb->Failure(409, "Retry disabling Computer Use to clear its previous controls before enabling it.");
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
	if (!uam::EnsureAcpStopProgress(m_app, chat_id))
	{
		cb->Failure(409, "The runtime is stopping; retry shortly.");
		return;
	}
	if (enabled && !uses_uam_backend && session != nullptr && uam::AcpSessionHasActiveTurn(*session))
	{
		cb->Failure(409, "Activate provider computer use after the current structured turn finishes.");
		return;
	}
	if (session != nullptr && session->running && !uses_uam_backend && !uam::StopAcpSession(m_app, chat_id))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The runtime is stopping; retry shortly."));
		return;
	}

	chat->computer_use_enabled = true;
	if (uses_uam_backend)
	{
		std::string error;
		if (!uam::ComputerUseService::SetControlState(m_app, chat_id, "armed", &error))
		{
			chat->computer_use_enabled = false;
			cb->Failure(500, FailureDetailOrFallback(error, "Failed to arm Computer Use."));
			return;
		}
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatComputerUseBackend(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("backend", "")));
	if (requested != uam::computer_use::kBackendAuto && requested != uam::computer_use::kBackendProvider && requested != uam::computer_use::kBackendUam)
	{
		cb->Failure(400, "Computer-use backend must be auto, provider, or uam.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (requested == uam::computer_use::kBackendProvider && !uam::computer_use::ProviderBackendAvailable(chat->provider_id))
	{
		cb->Failure(409, "Provider computer use is unavailable in this structured session.");
		return;
	}
	if (chat->computer_use_enabled)
	{
		cb->Failure(409, "Turn off computer use before changing its control method.");
		return;
	}
	if (uam::computer_use::BackendPreference(chat->computer_use_backend) == requested)
	{
		cb->Success("{}");
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
	if (!uam::EnsureAcpStopProgress(m_app, chat_id))
	{
		cb->Failure(409, "The runtime is stopping; retry shortly.");
		return;
	}
	if (session != nullptr && uam::AcpSessionHasActiveTurn(*session))
	{
		cb->Failure(409, "Change the computer-use backend after the current turn finishes.");
		return;
	}
	if (session != nullptr && session->running && !uam::StopAcpSession(m_app, chat_id))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The runtime is stopping; retry shortly."));
		return;
	}

	const ChatSession previous = *chat;
	const std::string previous_updated_at = chat->updated_at;
	chat->computer_use_backend = requested;
	chat->computer_use_target_kind = "window";
	chat->computer_use_target_id.clear();
	chat->computer_use_target_process_id.clear();
	chat->computer_use_target_title.clear();
	chat->computer_use_target_input_mode.clear();
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Computer-use control method updated.", "Computer-use control method changed in UI, but failed to save."))
	{
		*chat = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist the computer-use control method."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetComputerUseControl(CefRefPtr<CefBrowser> browser,
    const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("state", "")));
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!uam::computer_use::UsesUamBackend(*chat))
	{
		cb->Failure(409, "Use the provider's controls for provider computer use.");
		return;
	}
	if (requested == "running" && !chat->computer_use_enabled)
	{
		cb->Failure(409, "Enable computer use before resuming it.");
		return;
	}

	std::string error;
	if (!uam::ComputerUseService::SetControlState(m_app, chat_id, requested, &error))
	{
		cb->Failure(400, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleGetComputerUseSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	nlohmann::json allowed = nlohmann::json::array();
	for (const auto& rule : m_app.settings.computer_use_allowed_applications)
		allowed.push_back({{"identityKind", rule.identity_kind}, {"identity", rule.identity}});
	cb->Success(nlohmann::json{{"computerUseGloballyEnabled", m_app.settings.computer_use_globally_enabled}, {"computerUseRevocationPending", !m_app.computer_use_revocation_pending_chat_ids.empty()}, {"allowlistEnabled", m_app.settings.computer_use_allowlist_enabled}, {"allowedApplications", allowed}}.dump());
}

void UamQueryHandler::HandleSetComputerUseSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (payload.contains("computerUseGloballyEnabled") && !payload["computerUseGloballyEnabled"].is_boolean()) { cb->Failure(400, "computerUseGloballyEnabled must be a boolean."); return; }
	if (payload.contains("allowlistEnabled") && !payload["allowlistEnabled"].is_boolean()) { cb->Failure(400, "allowlistEnabled must be a boolean."); return; }
	AppSettings previous = m_app.settings;
	const bool enabled = payload.value("allowlistEnabled", m_app.settings.computer_use_allowlist_enabled);
	std::vector<ComputerUseApplicationRule> allowed = m_app.settings.computer_use_allowed_applications;
	if (payload.contains("allowedApplications"))
	{
		if (!payload["allowedApplications"].is_array()) { cb->Failure(400, "allowedApplications must be an array."); return; }
		allowed.clear();
		for (const auto& item : payload["allowedApplications"])
		{
			if (!item.is_object() || !item.contains("identityKind") || !item["identityKind"].is_string() || !item.contains("identity") || !item["identity"].is_string()) { m_app.settings = previous; cb->Failure(400, "Each allowed application needs identityKind and identity."); return; }
			allowed.push_back({item.value("identityKind", ""), item.value("identity", "")});
		}
	}
	const bool globally_enabled = payload.value("computerUseGloballyEnabled", previous.computer_use_globally_enabled);
	if (globally_enabled && (!m_app.computer_use_revocation_pending_chat_ids.empty() ||
	    (!previous.computer_use_globally_enabled && !uam::ComputerUseService::ResetControlsForStartup(m_app))))
	{
		cb->Failure(500, "Computer Use cannot be re-enabled until its previous control grants are cleared.");
		return;
	}
	m_app.settings.computer_use_globally_enabled = globally_enabled;
	m_app.settings.computer_use_allowlist_enabled = enabled;
	m_app.settings.computer_use_allowed_applications = std::move(allowed);
	if (!PersistenceCoordinator().SaveSettings(m_app)) { m_app.settings = previous; cb->Failure(500, "Failed to persist Computer Use Settings."); return; }
	bool revoked = true;
	if (!m_app.settings.computer_use_globally_enabled)
	{
		// Terminal history synchronization can replace the chat vector.
		std::unordered_set<std::string> owned_ids = m_app.computer_use_revocation_pending_chat_ids;
		for (const std::pair<const std::string, uam::ComputerUseRuntimeState>& entry : m_app.computer_use_by_chat_id)
		{
			const std::string& chat_id = entry.first;
			const ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
			if ((chat == nullptr || uam::computer_use::AvailableForChat(*chat)) && HasComputerUseOwnership(m_app, chat_id, chat != nullptr && chat->computer_use_enabled))
				owned_ids.insert(chat_id);
		}
		for (ChatSession& chat : m_app.chats)
		{
			if (uam::computer_use::AvailableForChat(chat) && HasComputerUseOwnership(m_app, chat.id, chat.computer_use_enabled))
				owned_ids.insert(chat.id);
			chat.computer_use_enabled = false;
		}
		m_app.computer_use_revocation_pending_chat_ids.insert(owned_ids.begin(), owned_ids.end());
		for (const std::string& chat_id : owned_ids)
		{
			const bool acp_stopped = uam::StopAcpSession(m_app, chat_id);
			const bool terminal_stopped = uam::StopAndEraseCliTerminalForChat(m_app, chat_id);
			ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
			if (chat != nullptr) chat->computer_use_enabled = false;
			const bool control_stopped = uam::ComputerUseService::SetControlState(m_app, chat_id, "stopped");
			if (acp_stopped && terminal_stopped && control_stopped)
				m_app.computer_use_revocation_pending_chat_ids.erase(chat_id);
			else revoked = false;
		}
		revoked = uam::ComputerUseService::ResetControlsForStartup(m_app) && revoked;
	}
	if (!revoked)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(500, "Computer Use is disabled globally; some runtime controls are still stopping. Retry disabling to verify they stopped.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"computerUseGloballyEnabled", m_app.settings.computer_use_globally_enabled}, {"computerUseRevocationPending", !m_app.computer_use_revocation_pending_chat_ids.empty()}, {"allowlistEnabled", m_app.settings.computer_use_allowlist_enabled}}.dump());
}

void UamQueryHandler::HandleListComputerUseApplications(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [] {
		const auto result = RunComputerUseSettingsHelper("list");
		if (!result.ok) return result;
		const nlohmann::json applications = nlohmann::json::parse(result.body, nullptr, false);
		return uam::query_handler_async::AsyncSuccess(nlohmann::json{{"applications", applications}, {"error", ""}});
	});
}

void UamQueryHandler::HandleOpenComputerUseSystemSettings(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string permission = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("permission", "")));
	if (permission != "screenrecording" && permission != "accessibility") { cb->Failure(400, "permission must be screenRecording or accessibility."); return; }
	const std::string helper_permission = permission == "screenrecording" ? "screenRecording" : "accessibility";
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [helper_permission] {
		return RunComputerUseSettingsHelper("open", helper_permission);
	});
}

void UamQueryHandler::HandleCheckComputerUsePermissions(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [] {
		return RunComputerUseSettingsHelper("check");
	});
}

void UamQueryHandler::HandleRequestComputerUsePermission(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!m_app.settings.computer_use_globally_enabled)
	{
		cb->Failure(409, "Computer Use is disabled globally.");
		return;
	}
	const std::string permission = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("permission", "")));
	if (permission != "screenrecording" && permission != "accessibility") { cb->Failure(400, "permission must be screenRecording or accessibility."); return; }
	const std::string helper_permission = permission == "screenrecording" ? "screenRecording" : "accessibility";
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [helper_permission] {
		return RunComputerUseSettingsHelper("request", helper_permission);
	});
}

void UamQueryHandler::HandleSetChatMemoryEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", true);
	const std::string requested_level = uam::memory_levels::NormalizeSelection(uam::nlohmann_json::TrimmedStringValueOr(payload, "memoryLevel", ""), enabled);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->memory_level == requested_level && chat->memory_enabled == uam::memory_levels::IsEnabled(requested_level))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->memory_enabled;
	const std::string previous_level = chat->memory_level;
	const std::string previous_updated_at = chat->updated_at;
	chat->memory_level = requested_level;
	chat->memory_enabled = uam::memory_levels::IsEnabled(requested_level);
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat memory setting updated.", "Chat memory setting changed in UI, but failed to save."))
	{
		chat->memory_enabled = previous;
		chat->memory_level = previous_level;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat memory setting."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatRemoteRecovery(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindChatOrFail(m_app, payload.value("chatId", ""), cb, "Chat not found.");
	if (chat == nullptr) return;
	const bool enabled = payload.value("enabled", false);
	const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id);
	if (host == nullptr || host->id == "local" || (enabled && (!host->startup_enabled || host->startup_status != "enabled")))
	{
		cb->Failure(409, "Enable runner startup on this SSH host before enabling chat recovery.");
		return;
	}
	const bool previous = chat->remote_recovery_enabled;
	chat->remote_recovery_enabled = enabled;
	if (!ChatRepository::SaveChat(m_app.data_root, *chat))
	{
		chat->remote_recovery_enabled = previous;
		cb->Failure(500, "Chat recovery could not be saved.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatSmallModelMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::optional<bool> enabled = uam::nlohmann_json::BoolFieldStrict(payload, "enabled");
	if (!enabled)
	{
		cb->Failure(400, "Small-model mode must be a boolean.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}
	if (chat->small_model_mode == *enabled)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->small_model_mode;
	const std::string previous_updated_at = chat->updated_at;
	chat->small_model_mode = *enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Small-model workflow updated.", "Small-model workflow changed in UI, but failed to save."))
	{
		chat->small_model_mode = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist small-model workflow."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
