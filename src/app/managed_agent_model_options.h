#pragma once

#include "app/provider_configuration_service.h"
#include "app/provider_model_catalog_service.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_model_json.h"

namespace uam
{
	// Observed model metadata stays scoped to the child's actual workspace and host.
	inline AcpModelState ManagedAgentModelOptions(const AppState& app, const ChatSession& child,
	    const std::string& provider_id, const std::string& model_id)
	{
		AcpModelState result;
		result.id = model_id;
		if (app.provider_model_catalog)
		{
			for (const auto& entry : app.provider_model_catalog->GetCachedProviderModels(provider_id,
			         paths::PortablePathString(paths::ResolveWorkspaceRootPath(app, child)), child.execution_host_id))
			{
				const auto model = acp_models::ParseAcpModelState(entry);
				if (!model || (model_id.empty() ? !entry.value("isDefault", false) : model->id != model_id)) continue;
				result = *model;
				break;
			}
		}
		const auto& runtime = ProviderRuntimeRegistry::ResolveById(provider_id);
		if (runtime.ModelOptionsRequireRestart())
		{
			const auto* host = execution_hosts::Find(app.settings.execution_hosts,
			    child.execution_host_id.empty() ? "local" : child.execution_host_id);
			// An alias/default is not a concrete launch-configured model.
			result.supported_reasoning_efforts = runtime.ModelReasoningEfforts(model_id,
			    host ? CheckedProviderConfigurationVersion(app, provider_id, *host) : std::string{});
			result.default_reasoning_effort.clear();
		}
		return result;
	}
}
