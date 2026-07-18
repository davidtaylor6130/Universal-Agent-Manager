#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/git_worktree_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/vcs_commit_service.h"
#include "cef/cef_push.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/utils/nlohmann_json_utils.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// VCS and worktree handlers
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;
using namespace uam::query_handler_async;

namespace
{
		uam::AppState BuildReadOnlyAppSnapshot(std::filesystem::path data_root, AppSettings settings, std::vector<ChatFolder> folders, std::vector<ProviderProfile> provider_profiles)
		{
			uam::AppState snapshot;
		snapshot.data_root = std::move(data_root);
		snapshot.settings = std::move(settings);
		snapshot.folders = std::move(folders);
		snapshot.provider_profiles = std::move(provider_profiles);
		return snapshot;
	}

	struct ReadOnlyAppSnapshotInputs
	{
		std::filesystem::path data_root;
		AppSettings settings;
		std::vector<ChatFolder> folders;
		std::vector<ProviderProfile> provider_profiles;
	};

	ReadOnlyAppSnapshotInputs CaptureReadOnlyAppSnapshotInputs(const uam::AppState& app)
	{
		return {app.data_root, app.settings, app.folders, app.provider_profiles};
	}

	uam::AppState BuildReadOnlyAppSnapshot(ReadOnlyAppSnapshotInputs inputs)
	{
		return BuildReadOnlyAppSnapshot(std::move(inputs.data_root), std::move(inputs.settings), std::move(inputs.folders), std::move(inputs.provider_profiles));
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
		json["patchPath"] = result.patch_path.empty() ? "" : result.patch_path.string();
		return json;
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

	const uam::GitWorktreeStatus status = uam::GitWorktreeService().Status(m_app, *chat);
	cb->Success(SerializeGitWorktreeStatus(status).dump());
}

void UamQueryHandler::HandleCreateChatWorktree(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before changing workspace isolation.");
		return;
	}

	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().CreateForChat(m_app, *chat);
	if (!result.ok)
	{
		cb->Failure(400, FailureDetailOrFallback(result.message, "Failed to create isolated Git worktree."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeGitWorktreeResult(result).dump());
}

void UamQueryHandler::HandleDiscardChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before discarding worktree changes.");
		return;
	}

	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().DiscardChatChanges(m_app, *chat);
	if (!result.ok)
	{
		cb->Failure(400, FailureDetailOrFallback(result.message, "Failed to discard worktree changes."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeGitWorktreeResult(result).dump());
}

void UamQueryHandler::HandlePortChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before porting worktree changes.");
		return;
	}

	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PortChatChanges(m_app, *chat);
	if (!result.ok)
	{
		std::string message = FailureDetailOrFallback(result.message, "Failed to port worktree changes.");
		if (!result.patch_path.empty())
		{
			message += "\nPatch saved at: " + result.patch_path.string();
		}
		cb->Failure(400, message);
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeGitWorktreeResult(result).dump());
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
