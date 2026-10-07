#include "common/provider/antigravity/cli/antigravity_cli_provider_runtime.h"

#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"

const char* AntigravityCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kAntigravityCli;
}

std::vector<std::string> AntigravityCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive) return {};
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "agy");
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	const AppSettings merged = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	// Permission policy remains with the CLI. Keep explicit extra flags in the existing launch contract.
	uam::provider_runtime_internal::AppendArgs(argv, uam::command_line::SplitWords(merged.provider_extra_flags));
	return argv;
}

MessageRole AntigravityCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, type);
}

std::vector<ChatSession> AntigravityCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool AntigravityCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

const IProviderRuntime& GetAntigravityCliProviderRuntime()
{
	static const AntigravityCliProviderRuntime runtime;
	return runtime;
}
