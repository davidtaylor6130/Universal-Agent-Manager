#include "common/provider/claude/cli/claude_cli_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

using namespace provider_runtime_internal;

const char* ClaudeCliProviderRuntime::RuntimeId() const
{
	return "claude-cli";
}

bool ClaudeCliProviderRuntime::IsEnabled() const
{
	return UAM_ENABLE_RUNTIME_CLAUDE_CLI != 0;
}

const char* ClaudeCliProviderRuntime::DisabledReason() const
{
	return "Runtime 'claude-cli' is disabled in this build (UAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF).";
}

std::string ClaudeCliProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string ClaudeCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	const AppSettings provider_settings = MergeProviderSettings(profile, settings);
	const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
	return BuildCommandFromTemplate(provider_settings, prompt, files, effective_resume_session_id, "claude {flags} {prompt}");
}

std::vector<std::string> ClaudeCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	return provider_runtime_internal::BuildInteractiveArgv(profile, chat, MergeProviderSettings(profile, settings));
}

MessageRole ClaudeCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> ClaudeCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return LoadLocalChats(data_root);
}

bool ClaudeCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool ClaudeCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool ClaudeCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool ClaudeCliProviderRuntime::UsesStructuredOutput(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetClaudeCliProviderRuntime()
{
	static const ClaudeCliProviderRuntime runtime;
	return runtime;
}
