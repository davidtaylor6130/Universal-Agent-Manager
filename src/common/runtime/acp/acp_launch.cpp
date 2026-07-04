#include "common/runtime/acp/acp_session_internal.h"

#include "app/provider_resolution_service.h"
#include "common/paths/path_utils.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace uam::acp_detail
{

std::vector<std::string> BuildAcpLaunchArgv(const ProviderProfile& provider, const ChatSession& chat)
{
	return ProviderRuntimeRegistry::Resolve(provider).BuildStructuredLaunchArgv(provider, chat);
}

std::string JoinAcpArgvForDiagnostics(const std::vector<std::string>& argv)
{
	std::ostringstream out;
	for (std::size_t i = 0; i < argv.size(); ++i)
	{
		if (i > 0)
		{
			out << ' ';
		}
		out << argv[i];
	}
	return out.str();
}

std::string AcpWorkingDirectoryString(const std::filesystem::path& workspace_root)
{
	const std::filesystem::path cwd = workspace_root.empty() ? uam::paths::CurrentPathOrDot() : workspace_root;
	return cwd.string();
}

std::string BuildAcpLaunchDetail(const ProviderProfile& provider, const AppState& app, const std::filesystem::path& workspace_root, const ChatSession& chat)
{
	const std::vector<std::string> argv = BuildAcpLaunchArgv(provider, chat);
	return "cwd=" + AcpWorkingDirectoryString(workspace_root) + ", argv=" + JoinAcpArgvForDiagnostics(argv) + ", nativeSessionId=" + ResolvedAcpResumeIdForChat(app, chat);
}

std::string BuildAcpLaunchDetail(const AppState& app, const std::filesystem::path& workspace_root, const ChatSession& chat)
{
	if (const ProviderProfile* provider = ProviderResolutionService().ProviderForChat(app, chat); provider != nullptr)
	{
		return BuildAcpLaunchDetail(*provider, app, workspace_root, chat);
	}

	ProviderProfile provider;
	provider.id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
	return BuildAcpLaunchDetail(provider, app, workspace_root, chat);
}

int NextAcpRequestId(AcpSessionState& session, const std::string& method)
{
	const int id = session.next_request_id++;
	session.pending_request_methods[id] = method;
	return id;
}

} // namespace uam::acp_detail
