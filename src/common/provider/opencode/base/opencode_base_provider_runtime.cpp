#include "common/provider/opencode/base/opencode_base_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

using namespace provider_runtime_internal;

OpenCodeBaseProviderRuntime::OpenCodeBaseProviderRuntime(const char* runtime_id, const bool enabled, const char* disabled_reason) :
    runtime_id_(runtime_id), enabled_(enabled), disabled_reason_(disabled_reason == nullptr ? "" : disabled_reason)
{
}

const char* OpenCodeBaseProviderRuntime::RuntimeId() const
{
	return runtime_id_;
}

bool OpenCodeBaseProviderRuntime::IsEnabled() const
{
	return enabled_;
}

const char* OpenCodeBaseProviderRuntime::DisabledReason() const
{
	return disabled_reason_;
}

std::string OpenCodeBaseProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string OpenCodeBaseProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	const AppSettings provider_settings = MergeProviderSettings(profile, settings);
	const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
	return BuildCommandFromTemplate(provider_settings, prompt, files, effective_resume_session_id, "opencode {flags} {prompt}");
}

std::vector<std::string> OpenCodeBaseProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	return provider_runtime_internal::BuildInteractiveArgv(profile, chat, MergeProviderSettings(profile, settings));
}

MessageRole OpenCodeBaseProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> OpenCodeBaseProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return LoadLocalChats(data_root);
}

bool OpenCodeBaseProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool OpenCodeBaseProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeBaseProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeBaseProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool OpenCodeBaseProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeBaseProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool OpenCodeBaseProviderRuntime::UsesStructuredOutput(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeBaseProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}
