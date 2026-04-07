#include "common/provider/custom/unknown_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

using namespace provider_runtime_internal;

const char* UnknownProviderRuntime::RuntimeId() const
{
	return "custom-provider";
}

bool UnknownProviderRuntime::IsEnabled() const
{
	return true;
}

const char* UnknownProviderRuntime::DisabledReason() const
{
	return "";
}

std::string UnknownProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string UnknownProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	if (UsesInternalEngine(profile))
	{
		return "";
	}

	const AppSettings provider_settings = MergeProviderSettings(profile, settings);
	const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
	return BuildCommandFromTemplate(provider_settings, prompt, files, effective_resume_session_id, "provider-cli {flags} {prompt}");
}

std::vector<std::string> UnknownProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (UsesInternalEngine(profile) || !profile.supports_interactive)
	{
		return {};
	}

	return provider_runtime_internal::BuildInteractiveArgv(profile, chat, MergeProviderSettings(profile, settings));
}

MessageRole UnknownProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> UnknownProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return LoadLocalChats(data_root);
}

bool UnknownProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool UnknownProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool UnknownProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool UnknownProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool UnknownProviderRuntime::UsesInternalEngine(const ProviderProfile& profile) const
{
	return EqualsIgnoreCase(profile.execution_mode, "internal-engine");
}

bool UnknownProviderRuntime::UsesCliOutput(const ProviderProfile& profile) const
{
	if (EqualsIgnoreCase(profile.output_mode, "cli"))
	{
		return true;
	}

	if (EqualsIgnoreCase(profile.output_mode, "structured"))
	{
		return false;
	}

	return !UsesInternalEngine(profile) && profile.supports_interactive;
}

bool UnknownProviderRuntime::UsesStructuredOutput(const ProviderProfile& profile) const
{
	return !UsesCliOutput(profile);
}

bool UnknownProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile& profile) const
{
	return EqualsIgnoreCase(profile.prompt_bootstrap, "gemini-at-path");
}

const IProviderRuntime& GetUnknownProviderRuntime()
{
	static const UnknownProviderRuntime runtime;
	return runtime;
}
