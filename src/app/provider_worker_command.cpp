#include "app/provider_worker_command.h"

#include "common/provider/provider_runtime.h"

namespace uam
{
	std::string BuildProviderWorkerCommand(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id, ProviderWorkerPathMode path_mode)
	{
		const auto& runtime = ProviderRuntimeRegistry::Resolve(profile);
		auto argv = runtime.BuildWorkerArgv(profile, settings, prompt, model_id);

		if (argv.empty())
		{
			return "";
		}

		return BuildProviderWorkerShellCommand(argv, path_mode);
	}
} // namespace uam
