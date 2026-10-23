#include "app/copilot_agent_history_service.h"
#include "common/provider/copilot_native_history.h"
#include "common/provider/copilot/cli/copilot_session_paths.h"
#include "common/provider/provider_ids.h"
#include "common/platform/platform_services.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/utils/io_utils.h"
#include "remote/native_history_snapshot.h"
#include "remote/runner_client.h"

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
namespace uam::copilot
{
namespace
{

std::optional<CopilotAgentHistoryReference> Validate(const ChatSession& source, std::string_view reference, std::string* error)
{
	if (error) error->clear();
	auto decoded = ParseCopilotAgentHistoryId(reference);
	const std::string host = source.execution_host_id.empty() ? "local" : source.execution_host_id;
	if (!decoded || source.provider_id != provider_ids::kCopilotCli || decoded->parent_session_id != source.native_session_id ||
		decoded->execution_host_id != host)
	{
		if (error) *error = "The Copilot agent history reference belongs to a different parent session or host.";
		return std::nullopt;
	}
	return decoded;
}
}

std::optional<ChatSession> ParseAgentHistorySnapshot(const ExecutionHost& host, const ChatSession& source,
	std::string_view reference, const nlohmann::json& snapshot, std::string* error, std::stop_token stop_token)
{
	const auto decoded = Validate(source, reference, error);
	const auto fail = [&](const char* message) -> std::optional<ChatSession> { if (error) *error = message; return std::nullopt; };
	if (!decoded) return std::nullopt;
	if (stop_token.stop_requested()) return fail("Copilot agent history loading was cancelled.");
	if (host.id == "local" || host.id != decoded->execution_host_id || host.transport != "ssh" ||
		!snapshot.is_object() || remote::native_history::String(snapshot, "providerId") != provider_ids::kCopilotCli ||
		remote::native_history::String(snapshot, "sessionId") != decoded->parent_session_id ||
		!execution_hosts::IsAbsoluteRemotePath(host.platform, source.workspace_directory))
		return fail("The Copilot agent snapshot belongs to a different parent session or host.");
	const std::string workspace = remote::native_history::String(snapshot, "workspace");
	const std::string text = remote::native_history::String(snapshot, "text");
	const std::string yaml = remote::native_history::String(snapshot, "workspaceYaml");
	std::string recorded;
	if (!execution_hosts::RemotePathsMatch(host.platform, workspace, source.workspace_directory) ||
		text.empty() || text.size() > remote::native_history::kMaxSnapshotBytes || yaml.size() > 16384 ||
		text.size() + yaml.size() > remote::native_history::kMaxSnapshotBytes ||
		!remote::native_history::CopilotIdentity(text, decoded->parent_session_id, workspace, recorded))
		return fail("The Copilot agent snapshot does not attest the parent session and workspace.");
	const auto temporary = paths::TempDirectoryPathNoThrow();
	const std::string uuid = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (!temporary || !remote::native_history::SafeId(uuid)) return fail("Could not allocate an isolated Copilot agent reader.");
	const auto root = *temporary / ("uam-copilot-agent-" + uuid);
	std::error_code fs_error;
	if (!std::filesystem::create_directory(root, fs_error) || fs_error) return fail("Could not create an isolated Copilot agent reader.");
	struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); } } cleanup{root};
#if !defined(_WIN32)
	std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, fs_error);
	if (fs_error) return fail("Could not protect the isolated Copilot agent reader.");
#endif
	const auto directory = root / decoded->parent_session_id;
	if (!io::WriteTextFile(directory / "events.jsonl", text) || (!yaml.empty() && !io::WriteTextFile(directory / "workspace.yaml", yaml)))
		return fail("Could not stage the exact parent-owned Copilot agent journal.");
	auto loaded = LoadCopilotAgentSessionStateChat(directory, decoded->agent_id, paths::PathFromUtf8(workspace), host.id, error, stop_token);
	if (loaded) loaded->workspace_directory = source.workspace_directory;
	return loaded;
}

std::optional<ChatSession> LoadAgentHistory(const ChatSession& source, std::string_view reference,
	const std::optional<ExecutionHost>& host, std::string* error, std::stop_token stop_token)
{
	const auto decoded = Validate(source, reference, error);
	if (!decoded || stop_token.stop_requested()) return std::nullopt;
	if (!host)
	{
		if (!paths::IsControllerLocalWorkspace(source) || !paths::PathFromUtf8(source.workspace_directory).is_absolute())
		{ if (error) *error = "The exact local Copilot parent workspace is unavailable."; return std::nullopt; }
		return LoadCopilotAgentSessionStateChat(CopilotSessionStatePath() / decoded->parent_session_id,
			decoded->agent_id, paths::PathFromUtf8(source.workspace_directory), decoded->execution_host_id, error, stop_token);
	}
	if (host->id != source.execution_host_id || host->runner_status != "ready" || host->transport != "ssh")
	{ if (error) *error = "The exact remote Copilot parent helper is unavailable."; return std::nullopt; }
	remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
		remote::SshBridgeArgv(host->ssh_alias, host->platform, host->runner_version, host->runner_directory, host->runner_protocol_version),
		host->runner_version, host->runner_protocol_version);
	nlohmann::json snapshot;
	if (!client.ReadNativeHistory(provider_ids::kCopilotCli, decoded->parent_session_id, source.workspace_directory, snapshot, error, stop_token)) return std::nullopt;
	return ParseAgentHistorySnapshot(*host, source, reference, snapshot, error, stop_token);
}
}

#endif
