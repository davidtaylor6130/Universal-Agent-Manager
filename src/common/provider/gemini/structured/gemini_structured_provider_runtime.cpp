#include "common/provider/gemini/structured/gemini_structured_provider_runtime.h"

#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/runtime/provider_runtime_internal.h"

using namespace provider_runtime_internal;

const char* GeminiStructuredProviderRuntime::RuntimeId() const
{
	return "gemini-structured";
}

bool GeminiStructuredProviderRuntime::IsEnabled() const
{
	return UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED != 0;
}

const char* GeminiStructuredProviderRuntime::DisabledReason() const
{
	return "Runtime 'gemini-structured' is disabled in this build (UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=OFF).";
}

std::string GeminiStructuredProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string GeminiStructuredProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	const AppSettings provider_settings = MergeProviderSettings(profile, settings);
	const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
	return BuildCommandFromTemplate(provider_settings, prompt, files, effective_resume_session_id, "gemini -r {resume} {flags} {prompt}");
}

std::vector<std::string> GeminiStructuredProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	return provider_runtime_internal::BuildInteractiveArgv(profile, chat, MergeProviderSettings(profile, settings));
}

MessageRole GeminiStructuredProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> GeminiStructuredProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path&, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const
{
	return LoadGeminiJsonHistoryForRuntime(native_history_chats_dir, profile, options);
}

bool GeminiStructuredProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool GeminiStructuredProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiStructuredProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiStructuredProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return false;
}

bool GeminiStructuredProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool GeminiStructuredProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return false;
}

bool GeminiStructuredProviderRuntime::UsesStructuredOutput(const ProviderProfile&) const
{
	return true;
}

bool GeminiStructuredProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return true;
}

const IProviderRuntime& GetGeminiStructuredProviderRuntime()
{
	static const GeminiStructuredProviderRuntime runtime;
	return runtime;
}