#include "runtime_local_service.h"

#include "app/application_core_helpers.h"

#include "common/runtime/local_bridge_runtime.h"

#include <algorithm>

namespace
{
	constexpr const char* kRuntimeIdCliLocalBridge = "opencode-local";
}

bool RuntimeLocalService::ProviderUsesLocalBridgeRuntime(const ProviderProfile& provider) const
{
	return ToLowerAscii(Trim(provider.id)) == kRuntimeIdCliLocalBridge;
}

bool RuntimeLocalService::EnsureSelectedLocalRuntimeModelForProvider(uam::AppState& app) const
{
	const std::filesystem::path model_folder = ResolveRagModelFolder(app);
	const std::vector<std::string> runtime_models = app.runtime_model_service.ListModels(model_folder);
	const std::string selected_model = Trim(app.settings.selected_model_id);
	const bool selected_model_valid = !selected_model.empty() && std::find(runtime_models.begin(), runtime_models.end(), selected_model) != runtime_models.end();

	if (selected_model_valid)
	{
		return true;
	}

	if (runtime_models.empty())
	{
		app.runtime_model_selection_id.clear();
		app.status_line = "No local runtime models found. Add one, then retry.";
	}
	else
	{
		if (Trim(app.runtime_model_selection_id).empty() || std::find(runtime_models.begin(), runtime_models.end(), app.runtime_model_selection_id) == runtime_models.end())
		{
			app.runtime_model_selection_id = runtime_models.front();
		}

		if (selected_model.empty())
		{
			app.status_line = "Select a local runtime model to continue.";
		}
		else
		{
			app.status_line = "Selected model is unavailable. Choose a local runtime model to continue.";
		}
	}

	app.open_runtime_model_selection_popup = true;
	return false;
}

bool RuntimeLocalService::EnsureLocalRuntimeModelLoaded(uam::AppState& app, std::string* error_out) const
{
	const std::filesystem::path model_folder = ResolveRagModelFolder(app);
	return app.runtime_model_service.LoadModelIfNeeded(model_folder, app.settings.selected_model_id, app.loaded_runtime_model_id, error_out);
}

bool RuntimeLocalService::RestartLocalBridgeIfModelChanged(uam::AppState& app, std::string* error_out) const
{
	const std::filesystem::path desired_model_folder_path = ResolveRagModelFolder(app);
	std::filesystem::path desired_model_folder_norm = NormalizeAbsolutePath(desired_model_folder_path);

	if (desired_model_folder_norm.empty())
	{
		desired_model_folder_norm = desired_model_folder_path;
	}

	const std::string desired_requested_model = Trim(app.settings.selected_model_id);
	LocalBridgeRuntime bridge_runtime;
	return bridge_runtime.EnsureRunning(app, desired_model_folder_norm, desired_requested_model, error_out);
}

bool RuntimeLocalService::EnsureLocalBridgeRunning(uam::AppState& app, std::string* error_out) const
{
	return RestartLocalBridgeIfModelChanged(app, error_out);
}

void RuntimeLocalService::StopLocalBridge(uam::AppState& app) const
{
	LocalBridgeRuntime bridge_runtime;
	bridge_runtime.Stop(app, true);
}

RuntimeLocalService& GetRuntimeLocalService()
{
	static RuntimeLocalService service;
	return service;
}
