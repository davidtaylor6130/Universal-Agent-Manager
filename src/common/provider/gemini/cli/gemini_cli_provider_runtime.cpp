#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"

#include "app/application_core_helpers.h"
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

	fs::path normalizedFrom = fs::weakly_canonical(fromWorkspace);
	fs::path normalizedTo = fs::weakly_canonical(toWorkspace);
	if (normalizedFrom == normalizedTo)
	{
		return true;
	}

	const auto fromTmpDir = AppPaths::ResolveGeminiProjectTmpDir(fromWorkspace);
	const auto toTmpDir = AppPaths::ResolveGeminiProjectTmpDir(toWorkspace);

	if (!fromTmpDir.has_value())
	{
		return false;
	}

	const fs::path fromChatsDir = fromTmpDir.value() / "chats";
	if (!fs::exists(fromChatsDir))
	{
		return false;
	}

	fs::path targetChatsDir;
	fs::path targetTmpDir;

	if (toTmpDir.has_value())
	{
		targetTmpDir = toTmpDir.value();
		targetChatsDir = targetTmpDir / "chats";
	}
	else
	{
		targetTmpDir = AppPaths::GeminiHomePath() / "tmp" / toWorkspace.filename();
		targetChatsDir = targetTmpDir / "chats";
	}

	std::error_code l_ec;
	fs::create_directories(targetChatsDir, l_ec);
	if (l_ec)
	{
		return false;
	}

	std::string searchPart = sessionId;
	if (sessionId.length() > 8)
	{
		searchPart = sessionId.substr(0, 8);
	}

	fs::path sourceFile;
	for (const auto& l_entry : fs::directory_iterator(fromChatsDir, l_ec))
	{
		if (l_ec || !l_entry.is_regular_file())
		{
			continue;
		}

		const std::string l_filename = l_entry.path().string();
		if (l_filename.find(searchPart) != std::string::npos)
		{
			sourceFile = l_entry.path();
			break;
		}
	}

	if (sourceFile.empty())
	{
		return false;
	}

	const fs::path destFile = targetChatsDir / sourceFile.filename();
	fs::copy_file(sourceFile, destFile, fs::copy_options::overwrite_existing, l_ec);
	if (l_ec)
	{
		return false;
	}

	if (!toTmpDir.has_value())
	{
		const fs::path projectRootFile = targetTmpDir / ".project_root";
		WriteTextFile(projectRootFile, toWorkspace.string());
	}

	return true;
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