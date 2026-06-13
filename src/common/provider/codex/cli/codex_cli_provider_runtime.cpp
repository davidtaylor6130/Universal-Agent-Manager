#include "common/provider/codex/cli/codex_cli_provider_runtime.h"

#include "common/provider/codex/cli/codex_thread_id.h"
#include "app/goal_service.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"

namespace
{
	constexpr const char* kCodexFullAutoFlag = "--full-auto";

	std::vector<std::string> CodexFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, kCodexFullAutoFlag);
	}
} // namespace

const char* CodexCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCodexCli;
}

bool CodexCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* CodexCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::vector<std::string> CodexCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv;
	const std::string resume_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
	if (profile.supports_resume && !resume_id.empty())
	{
		uam::provider_runtime_internal::AppendLiteralArgs(argv, {"codex", "resume", "--no-alt-screen"});
		argv.push_back(resume_id);
	}
	else
	{
		argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "codex --no-alt-screen");
	}

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "-m", chat.model_id);

	uam::provider_runtime_internal::AppendArgs(argv, CodexFlagsFromSettings(provider_settings));
	return argv;
}

MessageRole CodexCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> CodexCliProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	(void)profile;
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool CodexCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

bool CodexCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool CodexCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

std::vector<std::string> CodexCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"codex", "exec"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);
	
	// Add read-only args for worker mode
	constexpr const char* kCodexReadOnlyWorkerArgs[] = {
	    "--ignore-user-config", "--ignore-rules", "--json", "--color", "never",
	    "--ephemeral", "--skip-git-repo-check", "--sandbox", "read-only",
	    "-c", "model_reasoning_effort=\"low\"",
	};
	for (const char* arg : kCodexReadOnlyWorkerArgs)
	{
		argv.push_back(arg);
	}
	
	if (!model_id.empty())
	{
		argv.push_back("-m");
		argv.push_back(std::string(model_id));
	}
	
	argv.push_back(std::string(prompt));
	return argv;
}

const IProviderRuntime& GetCodexCliProviderRuntime()
{
	static const CodexCliProviderRuntime runtime;
	return runtime;
}
