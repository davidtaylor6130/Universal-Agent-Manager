#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "app/repository_route_match_service.h"
#include "app/workspace_vcs_context_service.h"
#include "common/runtime/app_time.h"
#include <memory>

using namespace uam::query_handler_async;

void UamQueryHandler::HandleListRepositoryMatches(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	(void)browser;
	if (!payload.is_object() || !payload.empty()) { cb->Failure(400, "Repository matching takes no arguments."); return; }
	if (m_workspaceDeletionPending || m_app.folders.size() > 512) { cb->Failure(409, "Workspace inventory is unavailable."); return; }
	std::shared_ptr<void> slot = uam::AcquireWorkspaceVcsContextSlot(m_pendingWorkspaceVcsContexts);
	if (!slot) { cb->Failure(429, "Repository context is busy. Try again shortly."); return; }
	const std::vector<ExecutionHost> hosts = m_app.settings.execution_hosts;
	const std::filesystem::path data_root = m_app.data_root;
	const std::vector<uam::RepositoryRouteWorkspace> captured = uam::CaptureRepositoryRouteWorkspaces(m_app.folders, hosts);
	const std::shared_ptr<std::vector<uam::RepositoryRouteWorkspace>> inventory = std::make_shared<std::vector<uam::RepositoryRouteWorkspace>>(captured);
	RunAsyncCefQuery(m_asyncLifetime, cb,
	    [hosts, inventory, slot = std::move(slot)]() mutable
	    {
		    const std::shared_ptr<void> active_slot = std::move(slot);
		    *inventory = uam::ReadRepositoryRouteInventory(*inventory, hosts);
		    return AsyncSuccess(nlohmann::json::object());
	    },
	    [this, captured, inventory, data_root](AsyncCefResult& result)
	    {
		    const std::vector<uam::RepositoryRouteWorkspace> current = uam::CaptureRepositoryRouteWorkspaces(m_app.folders, m_app.settings.execution_hosts);
		    if (m_workspaceDeletionPending || m_app.data_root != data_root || !uam::RepositoryRouteSnapshotMatches(captured, current))
		    { result = AsyncFailure(409, "The workspace inventory changed while repository matches were loading."); return; }
		    if (!result.ok) return;
		    m_app.repository_route_workspaces = *inventory;
		    m_app.repository_route_data_root = data_root;
		    m_app.repository_route_observed_at_s = uam::GetAppTimeSeconds();
		    nlohmann::json workspaces = nlohmann::json::array();
		    for (const uam::RepositoryRouteWorkspace& workspace : *inventory)
		    {
			    if (workspace.host_id != "local") continue;
			    nlohmann::json repository = nullptr;
			    if (workspace.repository) repository = {{"kind", workspace.repository->kind}, {"identity", workspace.repository->identity}};
			    nlohmann::json matches = nlohmann::json::array();
			    for (const uam::RepositoryRouteMatch& match : uam::MatchRepositoryRoutes(workspace, *inventory, m_app.settings.execution_routing))
				    matches.push_back({{"executionHostId", match.host_id}, {"remoteDirectory", match.directory}, {"source", match.manual ? "manual" : "automatic"}});
			    workspaces.push_back({{"folderId", workspace.folder_id}, {"repository", repository}, {"matches", matches}});
		    }
		    result = AsyncSuccess({{"workspaces", workspaces}});
	    });
}
