#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"

#include <nlohmann/json.hpp>
#include <optional>
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
	if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInFileManager(*workspace_root, &error))
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

void UamQueryHandler::HandleOpenWorkspaceTerminal(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto workspace_root = ResolvePayloadWorkspaceRootOrFail(m_app, payload, cb);
	if (!workspace_root)
	{
		return;
	}

	std::string error;
	if (!PlatformServicesFactory::Instance().process_service.LaunchShellAt(*workspace_root, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open terminal."));
		return;
	}

	cb->Success("{}");
}
