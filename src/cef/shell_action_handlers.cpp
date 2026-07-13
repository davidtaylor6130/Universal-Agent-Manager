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
			action.accepts_files = value.value("acceptsFiles", true);
			action.accepts_folders = value.value("acceptsFolders", true);
			action.enabled = value.value("enabled", true);
			action.open_workspace = value.value("openWorkspace", false);
			actions.push_back(std::move(action));
		}
		return true;
	}
}

void UamQueryHandler::HandleSetShellActions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::vector<ShellAction> actions;
	std::string error;
	if (!ParseActions(payload, actions, error) || !ShellActionService::Save(m_app.data_root, actions, &error))
	{
		cb->Failure(400, error);
		return;
	}
	m_app.shell_actions = ShellActionService::Load(m_app.data_root);
	m_app.shell_action_notification = "Shell action settings saved. Choose Apply to update Finder or Explorer.";
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleApplyShellActions(CefRefPtr<CefBrowser> browser, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	std::string error;
	const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
	if (!ShellActionService::Apply(m_app, executable, &error))
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
