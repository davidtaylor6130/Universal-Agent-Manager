#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/chat_domain_service.h"
#include "app/git_worktree_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/vcs_commit_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/paths/path_utils.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/utils/nlohmann_json_utils.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// VCS and worktree handlers
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;
using namespace uam::query_handler_async;

namespace
{
	uam::AppState BuildReadOnlyAppSnapshot(std::filesystem::path data_root, AppSettings settings, std::vector<ChatFolder> folders, std::vector<ProviderProfile> provider_profiles, std::unordered_map<std::string, uam::CliProviderVersionState> provider_versions)
	{
		uam::AppState snapshot;
		snapshot.data_root = std::move(data_root);
		snapshot.settings = std::move(settings);
		snapshot.folders = std::move(folders);
		snapshot.provider_profiles = std::move(provider_profiles);
		snapshot.runtime_cli_versions_by_provider_id = std::move(provider_versions);
		return snapshot;
	}

	struct ReadOnlyAppSnapshotInputs
	{
		std::filesystem::path data_root;
		AppSettings settings;
		std::vector<ChatFolder> folders;
		std::vector<ProviderProfile> provider_profiles;
		std::unordered_map<std::string, uam::CliProviderVersionState> provider_versions;
	};

	ReadOnlyAppSnapshotInputs CaptureReadOnlyAppSnapshotInputs(const uam::AppState& app)
	{
		return {app.data_root, app.settings, app.folders, app.provider_profiles, app.runtime_cli_versions_by_provider_id};
	}

	uam::AppState BuildReadOnlyAppSnapshot(ReadOnlyAppSnapshotInputs inputs)
	{
		return BuildReadOnlyAppSnapshot(std::move(inputs.data_root), std::move(inputs.settings), std::move(inputs.folders), std::move(inputs.provider_profiles), std::move(inputs.provider_versions));
	}

	nlohmann::json SerializeGitWorktreeStatus(const uam::GitWorktreeStatus& status)
	{
		nlohmann::json json;
		json["isGitRepository"] = status.is_git_repository;
		json["isSvnWorkspace"] = status.is_svn_workspace;
		json["managedRepository"] = status.managed_repository;
		json["isolated"] = status.isolated;
		json["sourceDirty"] = status.source_dirty;
		json["worktreeDirty"] = status.worktree_dirty;
		json["worktreeMissing"] = status.worktree_missing;
		json["sourceDirectory"] = status.source_directory;
		json["worktreeDirectory"] = status.worktree_directory;
		json["branchName"] = status.branch_name;
		json["baseRef"] = status.base_ref;
		json["warning"] = status.warning;
		json["error"] = status.error;
		return json;
	}

	nlohmann::json SerializeGitWorktreeResult(const uam::GitWorktreeOperationResult& result)
	{
		nlohmann::json json;
		json["status"] = SerializeGitWorktreeStatus(result.status);
		json["message"] = result.message;
		json["patchPath"] = result.patch_path.empty() ? "" : uam::paths::Utf8PathString(result.patch_path);
		return json;
	}

	using WorktreeOperation = uam::GitWorktreeOperationResult (uam::GitWorktreeService::*)(uam::AppState&, ChatSession&) const;

	struct WorktreeAsyncState
	{
		uam::GitWorktreeOperationResult result;
		std::string isolation_kind;
		std::string source_directory;
		std::string base_ref;
		std::string branch_name;
		std::string worktree_directory;
		std::string updated_at;
	};

	void CaptureWorktreeFields(WorktreeAsyncState& target, const ChatSession& source)
	{
		target.isolation_kind = source.workspace_isolation_kind;
		target.source_directory = source.workspace_source_directory;
		target.base_ref = source.workspace_base_ref;
		target.branch_name = source.workspace_branch_name;
		target.worktree_directory = source.workspace_worktree_directory;
		target.updated_at = source.updated_at;
	}

	void ApplyWorktreeFields(ChatSession& target, const WorktreeAsyncState& source)
	{
		target.workspace_isolation_kind = source.isolation_kind;
		target.workspace_source_directory = source.source_directory;
		target.workspace_base_ref = source.base_ref;
		target.workspace_branch_name = source.branch_name;
		target.workspace_worktree_directory = source.worktree_directory;
		target.updated_at = source.updated_at;
	}

	void RunWorktreeOperationAsync(uam::AppState& app,
	                               CefRefPtr<CefBrowser> browser,
	                               CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
	                               const std::string& chat_id,
	                               WorktreeOperation operation,
	                               std::string failure_fallback)
	{
		auto state = std::make_shared<WorktreeAsyncState>();
		ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(app);
		uam::AppState* live_app = &app;
		app.worktree_operation_chat_ids.insert(chat_id);

		RunAsyncCefQuery(
		    callback,
		    [snapshot_inputs = std::move(snapshot_inputs), state, chat_id, operation, failure_fallback = std::move(failure_fallback)]() mutable
		    {
			    uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
			    std::string load_warning;
			    std::optional<ChatSession> chat = ChatRepository::LoadLocalChat(
			        snapshot.data_root, chat_id, true, &load_warning);
			    if (!chat)
			    {
				    return AsyncFailure(
				        500,
				        FailureDetailOrFallback(
				            load_warning, "Failed to load chat before its worktree operation."));
			    }
			    snapshot.chats.push_back(std::move(*chat));
			    const uam::GitWorktreeService service;
			    state->result = (service.*operation)(snapshot, snapshot.chats.front());
			    CaptureWorktreeFields(*state, snapshot.chats.front());
			    if (!state->result.ok)
			    {
				    std::string message = FailureDetailOrFallback(state->result.message, failure_fallback);
				    if (!state->result.patch_path.empty())
				    {
					    message += "\nPatch saved at: " + uam::paths::Utf8PathString(state->result.patch_path);
				    }
				    return AsyncFailure(400, std::move(message));
			    }
			    return AsyncSuccess(SerializeGitWorktreeResult(state->result));
		    },
		    [live_app, browser, state, chat_id](AsyncCefResult& result)
		    {
			    live_app->worktree_operation_chat_ids.erase(chat_id);
			    if (!result.ok)
			    {
				    return;
			    }

			    ChatSession* live_chat = ChatDomainService().FindChatById(*live_app, chat_id);
			    if (live_chat == nullptr)
			    {
				    result = AsyncFailure(404, "Chat was removed while its worktree operation was running.");
				    return;
			    }

			    ApplyWorktreeFields(*live_chat, *state);
			    uam::PushStateUpdateIfChanged(browser, *live_app);
		    });
	}

	nlohmann::json SerializeVcsCommitStatus(const uam::VcsCommitStatus& status)
	{
		nlohmann::json types = nlohmann::json::array();
		for (const uam::VcsType type : status.vcs_types)
		{
			types.push_back(uam::VcsTypeToString(type));
		}

		nlohmann::json files = nlohmann::json::array();
		for (const uam::VcsChangedFile& file : status.changed_files)
		{
			files.push_back({
			    {"path", file.path},
			    {"status", file.status},
			    {"staged", file.staged},
			    {"additions", file.additions},
			    {"deletions", file.deletions},
			    {"binary", file.binary},
			});
		}

		nlohmann::json json;
		json["available"] = status.available;
		json["vcsTypes"] = std::move(types);
		json["activeVcsType"] = uam::VcsTypeToString(status.active_vcs_type);
		json["workspaceDirectory"] = status.workspace_directory;
		json["branchOrRevision"] = status.branch_or_revision;
		json["changedFiles"] = std::move(files);
		json["lineStatsReady"] = status.line_stats_ready;
		json["warning"] = status.warning;
		json["error"] = status.error;
		return json;
	}

	nlohmann::json SerializeVcsCommitResult(const uam::VcsCommitResult& result)
	{
		return {
		    {"ok", result.ok},
		    {"status", SerializeVcsCommitStatus(result.status)},
		    {"message", result.message},
		    {"error", result.error},
		};
	}

	bool ChatRuntimeBusy(const uam::AppState& app, const std::string& chat_id)
	{
		if (uam::HasPendingCallForChat(app, chat_id))
		{
			return true;
		}
		for (const auto& session : app.acp_sessions)
		{
			if (session == nullptr || session->chat_id != chat_id || !session->running)
			{
				continue;
			}

			if (uam::AcpSessionHasBlockingRuntimeWork(*session))
			{
				return true;
			}
		}
		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal != nullptr && terminal->running && uam::CliTerminalMatchesChatId(*terminal, chat_id))
			{
				return true;
			}
		}
		return false;
	}
} // namespace

void UamQueryHandler::HandleGetChatWorktreeStatus(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::string chat_id = chat->id;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	RunAsyncCefQuery(
	    cb,
	    [snapshot_inputs = std::move(snapshot_inputs), chat_id]() mutable
	    {
		    uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		    std::string load_warning;
		    std::optional<ChatSession> chat = ChatRepository::LoadLocalChat(
		        snapshot.data_root, chat_id, false, &load_warning);
		    if (!chat)
		    {
			    return AsyncFailure(
			        500,
			        FailureDetailOrFallback(
			            load_warning, "Failed to load chat worktree status."));
		    }
		    return AsyncSuccess(SerializeGitWorktreeStatus(
		        uam::GitWorktreeService().Status(snapshot, *chat)));
	    });
}

void UamQueryHandler::HandleCreateChatWorktree(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (m_app.pending_chat_save_at_by_chat_id.contains(chat->id))
	{
		cb->Failure(409, "Wait for chat history to finish saving before changing workspace isolation.");
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before changing workspace isolation.");
		return;
	}

	RunWorktreeOperationAsync(m_app,
	                          browser,
	                          cb,
	                          chat->id,
	                          &uam::GitWorktreeService::CreateForChat,
	                          "Failed to create isolated Git worktree.");
}

void UamQueryHandler::HandleDiscardChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (m_app.pending_chat_save_at_by_chat_id.contains(chat->id))
	{
		cb->Failure(409, "Wait for chat history to finish saving before changing workspace isolation.");
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before discarding worktree changes.");
		return;
	}

	RunWorktreeOperationAsync(m_app,
	                          browser,
	                          cb,
	                          chat->id,
	                          &uam::GitWorktreeService::DiscardChatChanges,
	                          "Failed to discard worktree changes.");
}

void UamQueryHandler::HandlePortChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (m_app.pending_chat_save_at_by_chat_id.contains(chat->id))
	{
		cb->Failure(409, "Wait for chat history to finish saving before changing workspace isolation.");
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before porting worktree changes.");
		return;
	}

	RunWorktreeOperationAsync(m_app,
	                          browser,
	                          cb,
	                          chat->id,
	                          &uam::GitWorktreeService::PortChatChanges,
	                          "Failed to port worktree changes.");
}

void UamQueryHandler::HandleGetVcsCommitStatus(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const uam::VcsType requested_type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const bool include_line_stats = payload.value("includeLineStats", true);
	const std::string request_id = payload.value("requestId", "");
	const ChatSession chat_snapshot = *chat;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);

	RunAsyncCefQuery(cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), requested_type, include_line_stats, request_id]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 const uam::VcsCommitStatus status = uam::VcsCommitService().Status(snapshot, chat, requested_type, include_line_stats);
		                 return AsyncSuccess(WithOptionalRequestId(SerializeVcsCommitStatus(status), request_id));
	                 });
}

void UamQueryHandler::HandleGetVcsFileDiff(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::string path = payload.value("path", "");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const std::string request_id = payload.value("requestId", "");
	const ChatSession chat_snapshot = *chat;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);

	RunAsyncCefQuery(cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), path, type, request_id]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 std::string error;
		                 const std::string diff = uam::VcsCommitService().Diff(snapshot, chat, path, type, &error);
		                 if (!error.empty())
		                 {
			                 return AsyncFailure(400, error);
		                 }
		                 return AsyncSuccess(WithOptionalRequestId(nlohmann::json{{"diff", diff}}, request_id));
	                 });
}

void UamQueryHandler::HandleCommitVcsChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::vector<std::string> files = uam::nlohmann_json::TrimmedStringArrayField(payload, "files");
	const std::string message = payload.value("message", "");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(m_app, *chat, type, message, files);
	if (!result.ok)
	{
		cb->Failure(400, FailureDetailOrFallback(result.error, "Failed to commit changes."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeVcsCommitResult(result).dump());
}

void UamQueryHandler::HandleGenerateVcsCommitMessage(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	std::vector<std::string> files = uam::nlohmann_json::TrimmedStringArrayField(payload, "files");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const std::string request_id = payload.value("requestId", "");
	const ChatSession chat_snapshot = *chat;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);

	RunAsyncCefQuery(cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), type, files = std::move(files), request_id]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 const uam::VcsCommitMessageSuggestion suggestion = uam::VcsCommitService().GenerateMessage(snapshot, chat, type, files);
		                 if (!suggestion.ok)
		                 {
			                 return AsyncFailure(400, FailureDetailOrFallback(suggestion.error, "Failed to generate commit message."));
		                 }

		                 return AsyncSuccess(WithOptionalRequestId(
		                     nlohmann::json{
		                         {"title", suggestion.title},
		                         {"description", suggestion.description},
		                     },
		                     request_id));
	                 });
}
