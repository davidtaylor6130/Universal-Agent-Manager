#include "common/provider/provider_runtime.h"
#include "common/chat/chat_ids.h"

#include "common/provider/runtime/provider_runtime_internal.h"
#include "app/native_session_link_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"

namespace
{
constexpr auto kFallbackExactPromptLines = std::to_array<std::string_view>({">", ">_"});
constexpr auto kFallbackPromptCueTexts = std::to_array<std::string_view>({"Type your message", "? for shortcuts"});
constexpr int kFallbackPromptRecentLineLimit = 6;

std::string NormalizeInputPromptLine(std::string_view line)
{
	std::string normalized = uam::strings::Trim(line);

	while (!normalized.empty())
	{
		const unsigned char ch = static_cast<unsigned char>(normalized.front());
		if (ch == '|' || ch == '>' || ch < 0x80)
		{
			break;
		}
		normalized.erase(normalized.begin());
		normalized = uam::strings::Trim(normalized);
	}

	while (!normalized.empty())
	{
		const unsigned char ch = static_cast<unsigned char>(normalized.back());
		if (ch < 0x80)
		{
			break;
		}
		normalized.pop_back();
		normalized = uam::strings::Trim(normalized);
	}

	const auto box_prefix = normalized.find('>');
	if (box_prefix != std::string::npos)
	{
		normalized = uam::strings::Trim(std::string_view(normalized).substr(box_prefix));
	}

	return normalized;
}

}

void IProviderRuntime::NormalizeLoadedNativeSessionId(ChatSession& chat) const
{
	if (chat.native_session_id.empty() && !chat.id.empty() && !uam::chat_ids::IsLocalDraftChatId(chat.id))
	{
		chat.native_session_id = chat.id;
	}
}

bool IProviderRuntime::RecentOutputIndicatesInputPrompt(std::string_view recent_output) const
{
	const std::string stripped = uam::RecentTerminalPromptScanText(recent_output);

	if (stripped.empty())
	{
		return false;
	}

	const std::vector<std::string> lines = uam::SplitTerminalLines(stripped);

	int inspected = 0;
	for (auto it = lines.rbegin(); it != lines.rend() && inspected < kFallbackPromptRecentLineLimit; ++it)
	{
		std::string line = NormalizeInputPromptLine(*it);
		if (line.empty())
		{
			continue;
		}

		++inspected;
		if (uam::ranges::Contains(kFallbackExactPromptLines, std::string_view(line)))
		{
			return true;
		}

		if (uam::strings::Contains(line, '>') && uam::strings::ContainsAny(line, kFallbackPromptCueTexts))
		{
			return true;
		}
	}

	return false;
}

std::string IProviderRuntime::InteractiveConfigurationError(const ProviderProfile& profile, const AppSettings& settings) const
{
	if (uam::provider_runtime_internal::HasPermissionBypassExtraFlags(uam::provider_runtime_internal::MergeProviderSettings(profile, settings)))
	{
		return "Terminal fallback permissions are controlled by the provider. Remove permission-bypass flags from provider settings and approve requests in the terminal.";
	}
	return {};
}

std::vector<std::pair<std::string, std::string>> IProviderRuntime::BuildInteractiveEnvironment(const ProviderProfile& profile) const
{
	return uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(profile);
}

std::string IProviderRuntime::CreateNativeSession(const ProviderProfile&, const std::filesystem::path&,
    std::stop_token, std::string* error_out, const ExecutionHost*) const
{
	if (error_out != nullptr) *error_out = "This provider does not support empty native session creation.";
	return {};
}

std::string IProviderRuntime::ResolveInteractiveResumeId(const uam::AppState& app, const ChatSession& chat) const
{
	const std::string resolved = uam::ResolvedNativeSessionIdForChat(app, chat);
	if (!resolved.empty()) return resolved;
	const NativeSessionLinkService linker;
	if (linker.HasRealNativeSessionId(chat)) return linker.RealNativeSessionId(chat);
	return ChatHistorySyncService().ResolveResumeSessionIdForChat(app, chat);
}

bool IProviderRuntime::PrepareInteractiveSession(uam::AppState&, ChatSession&, const ProviderProfile&,
    const std::string&, const ExecutionHost&, std::string* error_out) const
{
	if (error_out != nullptr) error_out->clear();
	return true;
}

namespace
{
	bool RuntimeEnabledForProfile(const ProviderProfile& profile, const IProviderRuntime& runtime)
	{
		return runtime.IsEnabled() && uam::provider_runtime_internal::RuntimeConfigurationError(profile, runtime).empty();
	}

	std::string DisabledReasonForRuntime(const IProviderRuntime& runtime)
	{
		const char* reason = runtime.DisabledReason();
		return reason == nullptr ? std::string() : std::string(reason);
	}

	bool RuntimeProfileBoolean(const ProviderProfile& profile, bool (IProviderRuntime::*predicate)(const ProviderProfile&) const)
	{
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
		return (runtime.*predicate)(profile);
	}
} // namespace

std::vector<std::string> ProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
	if (!RuntimeEnabledForProfile(profile, runtime))
	{
		return {};
	}

	return runtime.BuildInteractiveArgv(profile, chat, settings);
}

bool ProviderRuntime::IsRuntimeEnabled(const ProviderProfile& profile)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
	return RuntimeEnabledForProfile(profile, runtime);
}

bool ProviderRuntime::IsRuntimeEnabled(std::string_view provider_id)
{
	return ProviderRuntimeRegistry::ResolveById(provider_id).IsEnabled();
}

std::string ProviderRuntime::DisabledReason(const ProviderProfile& profile)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);

	if (runtime.IsEnabled())
	{
		const std::string config_error = uam::provider_runtime_internal::RuntimeConfigurationError(profile, runtime);

		if (!config_error.empty())
		{
			return config_error;
		}

		return "";
	}

	return DisabledReasonForRuntime(runtime);
}

std::string ProviderRuntime::DisabledReason(std::string_view provider_id)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(provider_id);

	if (runtime.IsEnabled())
	{
		return "";
	}

	const std::string reason = DisabledReasonForRuntime(runtime);

	if (!reason.empty())
	{
		return reason;
	}

	if (ProviderRuntimeRegistry::IsEnabledRuntimeId(provider_id))
	{
		return "Runtime '" + std::string(provider_id) + "' is disabled in this build.";
	}

	return "";
}

MessageRole ProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type)
{
	return ProviderRuntimeRegistry::Resolve(profile).RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> ProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options)
{
	return ProviderRuntimeRegistry::Resolve(profile).LoadHistory(profile, data_root, native_history_chats_dir, options);
}

bool ProviderRuntime::SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat)
{
	return ProviderRuntimeRegistry::Resolve(profile).SaveHistory(profile, data_root, chat);
}

bool ProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesNativeOverlayHistory);
}

bool ProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::SupportsGeminiJsonHistory);
}

bool ProviderRuntime::UsesLocalHistory(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesLocalHistory);
}

bool ProviderRuntime::UsesCliOutput(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesCliOutput);
}

bool ProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesGeminiPathBootstrap);
}

bool ProviderRuntime::RebuildNativeSessionFile(const ProviderProfile& profile, const ChatSession& chat, const std::filesystem::path& workspace_path)
{
	return ProviderRuntimeRegistry::Resolve(profile).RebuildNativeSessionFile(profile, chat, workspace_path);
}

ProviderDiscoveryResult ProviderRuntime::DiscoverChatSources(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).DiscoverChatSources(profile);
}

bool ProviderRuntime::ProviderRecognizesSubagentTool(const ProviderProfile& profile, std::string_view tool_name)
{
	return ProviderRuntimeRegistry::Resolve(profile).ProviderRecognizesSubagentTool(tool_name);
}
