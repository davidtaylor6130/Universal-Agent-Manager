#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"
#include "remote/remote_workspace_terminal.h"
#include "remote/runner_client.h"
#include "common/utils/diagnostic_log.h"

#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/config/execution_host_config.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <iostream>
#include "app/chat_domain_service.h"
#include <string>

// ---------------------------------------------------------------------------
// Workspace handlers (open directory, editor, terminal)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	std::optional<std::filesystem::path> ResolvePayloadWorkspaceRootOrFail(uam::AppState& app,
	                                                                    const nlohmann::json& payload,
	                                                                    CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
	{
		const ChatSession* chat = FindPayloadChatOrFail(app, payload, cb);
		if (chat == nullptr)
		{
			return std::nullopt;
		}
		const ExecutionHost* execution_host = uam::execution_hosts::Find(
		    app.settings.execution_hosts, chat->execution_host_id);
		if (execution_host == nullptr)
		{
			cb->Failure(409, "The chat's execution host no longer exists.");
			return std::nullopt;
		}
		if (execution_host->id != uam::execution_hosts::kLocalHostId)
		{
			cb->Failure(409, "Local Finder, editor, and terminal actions are unavailable for remote workspaces.");
			return std::nullopt;
		}

		const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, *chat);
		if (workspace_root.empty())
		{
			cb->Failure(400, "Chat has no workspace directory.");
			return std::nullopt;
		}
		if (!uam::paths::PathExistsNoThrow(workspace_root))
		{
			cb->Failure(404, "Workspace directory does not exist.");
			return std::nullopt;
		}
		if (!uam::paths::IsDirectoryNoThrow(workspace_root))
		{
			cb->Failure(400, "Workspace path is not a directory.");
			return std::nullopt;
		}

		return workspace_root;
	}
} // namespace

void UamQueryHandler::HandleOpenWorkspaceDirectory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto workspace_root = ResolvePayloadWorkspaceRootOrFail(m_app, payload, cb);
	if (!workspace_root)
	{
		return;
	}

	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInFileManager(*workspace_root, &error, std::filesystem::u8path(m_app.settings.file_explorer_application)))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open workspace directory."));
		return;
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleOpenWorkspaceEditor(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto workspace_root = ResolvePayloadWorkspaceRootOrFail(m_app, payload, cb);
	if (!workspace_root)
	{
		return;
	}

	const std::string editor_preset_id = SelectEditorPresetForWorkspace(m_app.settings, *workspace_root);
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInEditorPreset(*workspace_root, editor_preset_id, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open workspace editor."));
		return;
	}

	cb->Success(nlohmann::json{{"editorPresetId", editor_preset_id}}.dump());
}

void UamQueryHandler::HandleOpenWorkspaceTerminal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr) return;
	const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id);
	if (host != nullptr && host->id != uam::execution_hosts::kLocalHostId)
	{
		const std::string directory = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(m_app, *chat));
		const std::vector<std::string> argv = uam::remote::BuildRemoteWorkspaceTerminalArgv(*host, directory);
		if (host->runner_status != "ready" || argv.empty())
		{
			cb->Failure(409, "The remote workspace or runner is unavailable. Check the host in Settings.");
			return;
		}
		const ExecutionHost observed = *host;
		const std::string chat_id = chat->id;
		uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
		    [observed]()
		    {
			    uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			        uam::remote::SshBridgeArgv(observed.ssh_alias, observed.platform, observed.runner_version,
			            observed.runner_directory, observed.runner_protocol_version), observed.runner_version,
			        observed.runner_protocol_version);
			    std::string error;
			    if (client.Connect(&error)) return uam::query_handler_async::AsyncSuccess({});
			    std::cerr << "Remote workspace terminal connection failed: " << error << '\n';
			    return uam::query_handler_async::AsyncFailure(502, "SSH connection failed: " + error);
		    },
		    [this, browser, observed, chat_id, directory, argv](uam::query_handler_async::AsyncCefResult& response)
		    {
			    if (!response.ok) return;
			    const ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
			    const ExecutionHost* current_host = uam::execution_hosts::Find(m_app.settings.execution_hosts, observed.id);
			    if (current == nullptr || current->execution_host_id != observed.id || current_host == nullptr ||
			        !uam::execution_hosts::SameConnection(*current_host, observed) ||
			        uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(m_app, *current)) != directory)
			    {
				    response = uam::query_handler_async::AsyncFailure(409, "The remote workspace changed. Open the terminal again.");
				    return;
			    }
			    std::string error;
			    if (!PlatformServicesFactory::Instance().process_service.LaunchTerminalCommand(argv, &error))
			    {
				    std::cerr << "Remote workspace terminal launch failed: " << error << '\n';
				    response = uam::query_handler_async::AsyncFailure(500, FailureDetailOrFallback(error, "Failed to open SSH terminal."));
			    }
		    });
		return;
	}
	const auto workspace_root = ResolvePayloadWorkspaceRootOrFail(m_app, payload, cb);
	if (!workspace_root) return;

	std::string error;
	if (!PlatformServicesFactory::Instance().process_service.LaunchShellAt(*workspace_root, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open terminal."));
		return;
	}

	cb->Success("{}");
}
