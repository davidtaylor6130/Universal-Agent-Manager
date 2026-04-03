#include "common/provider/gemini/base/gemini_base_provider_runtime.h"

#include "common/provider/gemini/base/gemini_command_builder.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/runtime/provider_runtime_internal.h"

using namespace provider_runtime_internal;

GeminiBaseProviderRuntime::GeminiBaseProviderRuntime(const char* runtime_id, const bool enabled, const char* disabled_reason, const bool uses_cli_output) :
    runtime_id_(runtime_id), enabled_(enabled), disabled_reason_(disabled_reason == nullptr ? "" : disabled_reason), uses_cli_output_(uses_cli_output)
{
}

const char* GeminiBaseProviderRuntime::RuntimeId() const
{
	return runtime_id_;
}

bool GeminiBaseProviderRuntime::IsEnabled() const
{
	return enabled_;
}

const char* GeminiBaseProviderRuntime::DisabledReason() const
{
	return disabled_reason_;
}

std::string GeminiBaseProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return GeminiCommandBuilder::BuildPrompt(user_prompt, files);
}

std::string GeminiBaseProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	const AppSettings provider_settings = MergeProviderSettings(profile, settings);
	const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
	return GeminiCommandBuilder::BuildCommand(provider_settings, prompt, files, effective_resume_session_id);
}

std::vector<std::string> GeminiBaseProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = MergeProviderSettings(profile, settings);

	if ((profile.interactive_command.empty() || EqualsIgnoreCase(profile.interactive_command, "gemini")) && (profile.resume_argument.empty() || profile.resume_argument == "-r") && profile.supports_resume)
	{
		return GeminiCommandBuilder::BuildInteractiveArgv(chat, provider_settings);
	}

	return provider_runtime_internal::BuildInteractiveArgv(profile, chat, provider_settings);
}

MessageRole GeminiBaseProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> GeminiBaseProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path&, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const
{
	return LoadGeminiJsonHistoryForRuntime(native_history_chats_dir, profile, options);
}

bool GeminiBaseProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool GeminiBaseProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiBaseProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiBaseProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return false;
}

bool GeminiBaseProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool GeminiBaseProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return uses_cli_output_;
}

bool GeminiBaseProviderRuntime::UsesStructuredOutput(const ProviderProfile&) const
{
	return !uses_cli_output_;
}

bool GeminiBaseProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return true;
}
