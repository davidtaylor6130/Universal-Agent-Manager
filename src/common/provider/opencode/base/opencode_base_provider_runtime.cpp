#include "common/provider/opencode/base/opencode_base_provider_runtime.h"

#include "common/provider/opencode/opencode_history_service.h"
#include "common/provider/runtime/provider_runtime_internal.h"

#include <chrono>
#include <iomanip>
#include <random>

using namespace provider_runtime_internal;

OpenCodeBaseProviderRuntime::OpenCodeBaseProviderRuntime(const char* runtime_id, const bool enabled, const char* disabled_reason) : runtime_id_(runtime_id), enabled_(enabled), disabled_reason_(disabled_reason == nullptr ? "" : disabled_reason), rng_(std::random_device{}())
{
}

std::string OpenCodeBaseProviderRuntime::GenerateOpenCodeSessionId() const
{
	std::uniform_int_distribution<int> dist(0, 61);
	static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	std::string result = "ses_";

	for (int i = 0; i < 20; ++i)
	{
		result += charset[dist(rng_)];
	}

	return result;
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

	std::vector<std::string> argv = provider_runtime_internal::BuildInteractiveArgv(profile, chat, MergeProviderSettings(profile, settings));

	const std::string model_id = settings.selected_model_id;
	if (!model_id.empty())
	{
		bool has_model = false;
		for (const auto& arg : argv)
		{
			if (arg == "--model" || arg == "-m" || arg.rfind("--model=", 0) == 0 || arg.rfind("-m=", 0) == 0)
			{
				has_model = true;
				break;
			}
		}

		if (!has_model)
		{
			argv.push_back("--model");
			argv.push_back(model_id);
		}
	}

	return argv;
}

MessageRole OpenCodeBaseProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> OpenCodeBaseProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions&) const
{
	std::vector<ChatSession> local_chats = LoadLocalChats(data_root);

	if (!native_history_chats_dir.empty())
	{
		std::vector<ChatSession> opencode_chats = OpenCodeHistoryService::LoadOpenCodeHistory(data_root, native_history_chats_dir);

		for (auto& chat : opencode_chats)
		{
			bool found = false;
			for (const auto& local : local_chats)
			{
				if (local.native_session_id == chat.native_session_id)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				local_chats.push_back(chat);
			}
		}
	}

	return local_chats;
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

std::string OpenCodeBaseProviderRuntime::GenerateSessionUUID() const
{
	return GenerateOpenCodeSessionId();
}
