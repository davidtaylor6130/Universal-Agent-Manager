#include "cef/uam_query_handler.h"

#include "app/shell_action_service.h"
#include "common/platform/platform_services.h"
#include "common/utils/string_utils.h"
#include "cef/cef_push.h"

#include <nlohmann/json.hpp>

namespace
{
	bool ParseActions(const nlohmann::json& payload, std::vector<ShellAction>& actions, std::string& error)
	{
		if (!payload.contains("actions") || !payload["actions"].is_array())
		{
			error = "Shell actions must be an array.";
			return false;
		}
		for (const auto& value : payload["actions"])
		{
			if (!value.is_object())
			{
				error = "Every shell action must be an object.";
				return false;
			}
			ShellAction action;
			action.id = uam::strings::Trim(value.value("id", ""));
			action.label = uam::strings::Trim(value.value("label", ""));
			action.skill_path = uam::strings::Trim(value.value("skillPath", ""));
			action.provider_id = uam::strings::Trim(value.value("providerId", ""));
			action.model_id = uam::strings::Trim(value.value("modelId", ""));
			if (value.contains("groupPath") && value["groupPath"].is_array())
			{
				for (const auto& segment : value["groupPath"])
				{
					if (segment.is_string()) action.group_path.push_back(segment.get<std::string>());
				}
			}
			action.accepts_files = value.value("acceptsFiles", true);
			action.accepts_folders = value.value("acceptsFolders", true);
			action.enabled = value.value("enabled", true);
			action.open_workspace = value.value("openWorkspace", false);
			actions.push_back(std::move(action));
		}
		return true;
	}
} // namespace

void UamQueryHandler::HandleSetShellActions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::vector<ShellAction> actions;
	std::string error;
	// Excluded providers' actions and kept invalid entries are saved as loaded, never as the interface returned them.
	if (ParseActions(payload, actions, error)) actions = ShellActionService::MergeInterfaceList(m_app.shell_actions, std::move(actions));
	if (!error.empty() || !ShellActionService::Save(m_app.data_root, actions, &error, m_app.shell_actions_unreadable))
	{
		cb->Failure(400, error);
		return;
	}
	bool reload_unreadable = false;
	std::vector<ShellAction> reloaded = ShellActionService::Load(m_app.data_root, {}, nullptr, &reload_unreadable);
	// The file now holds exactly what was just written, so a reload blocked by a transient lock keeps that list instead of an empty one.
	m_app.shell_actions = reload_unreadable ? std::move(actions) : std::move(reloaded);
	m_app.shell_actions_unreadable = false;
	m_app.shell_action_notification = "Shell action settings saved; choose Apply to update Finder or Explorer.";
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleApplyShellActions(CefRefPtr<CefBrowser> browser, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	std::string error;
	const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
	// While the saved list could not be read the in-memory list is empty, and applying it would remove every installed menu item.
	if (m_app.shell_actions_unreadable) error = std::string(ShellActionService::kUnreadableWarning);
	if (!error.empty() || !ShellActionService::Apply(m_app, executable, &error))
	{
		m_app.shell_action_notification = error;
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(500, error);
		return;
	}
	m_app.shell_action_notification = "Shell actions applied successfully.";
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDismissShellActionNotification(CefRefPtr<CefBrowser> browser, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	m_app.shell_action_notification.clear();
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
