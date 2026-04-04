#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"

#include "common/paths/app_paths.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/runtime/provider_runtime_internal.h"

namespace fs = std::filesystem;

using namespace provider_runtime_internal;

const char* GeminiCliProviderRuntime::RuntimeId() const
{
	return "gemini-cli";
}

bool GeminiCliProviderRuntime::IsEnabled() const
{
	return UAM_ENABLE_RUNTIME_GEMINI_CLI != 0;
}

const char* GeminiCliProviderRuntime::DisabledReason() const
{
	return "Runtime 'gemini-cli' is disabled in this build (UAM_ENABLE_RUNTIME_GEMINI_CLI=OFF).";
}

std::string GeminiCliProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string GeminiCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	const AppSettings provider_settings = MergeProviderSettings(profile, settings);
	const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
	return BuildCommandFromTemplate(provider_settings, prompt, files, effective_resume_session_id, "gemini -r {resume} {flags} {prompt}");
}

std::vector<std::string> GeminiCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	return provider_runtime_internal::BuildInteractiveArgv(profile, chat, MergeProviderSettings(profile, settings));
}

MessageRole GeminiCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> GeminiCliProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path&, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const
{
	return LoadGeminiJsonHistoryForRuntime(native_history_chats_dir, profile, options);
}

bool GeminiCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool GeminiCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return false;
}

bool GeminiCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool GeminiCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::UsesStructuredOutput(const ProviderProfile&) const
{
	return false;
}

bool GeminiCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::PortSessionToWorkspace(const ProviderProfile&, const std::string& sessionId, const std::filesystem::path& fromWorkspace, const std::filesystem::path& toWorkspace) const
{
	if (sessionId.empty() || fromWorkspace.empty() || toWorkspace.empty())
	{
		return false;
	}

	if (fromWorkspace == toWorkspace)
	{
		return true;
	}

	const auto fromTmpDir = AppPaths::ResolveGeminiProjectTmpDir(fromWorkspace);
	const auto toTmpDir = AppPaths::ResolveGeminiProjectTmpDir(toWorkspace);

	if (!fromTmpDir.has_value() || !toTmpDir.has_value())
	{
		return false;
	}

	const fs::path fromChatsDir = fromTmpDir.value() / "chats";
	const fs::path toChatsDir = toTmpDir.value() / "chats";

	std::error_code l_ec;
	fs::create_directories(toChatsDir, l_ec);
	if (l_ec)
	{
		return false;
	}

	fs::path sourceFile = fromChatsDir / (sessionId + ".json");
	if (!fs::exists(sourceFile))
	{
		sourceFile = fromChatsDir / (sessionId + ".txt");
	}

	if (!fs::exists(sourceFile))
	{
		return false;
	}

	const fs::path destFile = toChatsDir / sourceFile.filename();
	fs::copy_file(sourceFile, destFile, fs::copy_options::overwrite_existing, l_ec);

	return !l_ec;
}

const IProviderRuntime& GetGeminiCliProviderRuntime()
{
	static const GeminiCliProviderRuntime runtime;
	return runtime;
}

ProviderDiscoveryResult GeminiCliProviderRuntime::DiscoverChatSources(const ProviderProfile&) const
{
	ProviderDiscoveryResult result;
	result.sources = DiscoverGeminiTmpChatSources();
	return result;
}