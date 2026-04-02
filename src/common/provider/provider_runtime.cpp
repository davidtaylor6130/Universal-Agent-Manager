#include "provider_runtime.h"

#include "command_line_words.h"
#include "gemini_command_builder.h"
#include "gemini_native_history_store.h"
#include "local_chat_store.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#ifndef UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
#define UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED 1
#endif

#ifndef UAM_ENABLE_RUNTIME_GEMINI_CLI
#define UAM_ENABLE_RUNTIME_GEMINI_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_CODEX_CLI
#define UAM_ENABLE_RUNTIME_CODEX_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_CLAUDE_CLI
#define UAM_ENABLE_RUNTIME_CLAUDE_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_OPENCODE_CLI
#define UAM_ENABLE_RUNTIME_OPENCODE_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
#define UAM_ENABLE_RUNTIME_OPENCODE_LOCAL 1
#endif

#ifndef UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
#define UAM_ENABLE_RUNTIME_OLLAMA_ENGINE 1
#endif

namespace
{

	bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < lhs.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
			{
				return false;
			}
		}

		return true;
	}

	std::string LowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	bool AnyTypeMatches(const std::vector<std::string>& types, const std::string& value)
	{
		for (const std::string& candidate : types)
		{
			if (EqualsIgnoreCase(candidate, value))
			{
				return true;
			}
		}

		return false;
	}

	std::string JoinFlags(const std::vector<std::string>& flags)
	{
		std::ostringstream out;
		bool first = true;

		for (const std::string& flag : flags)
		{
			if (flag.empty())
			{
				continue;
			}

			if (!first)
			{
				out << ' ';
			}

			out << flag;
			first = false;
		}

		return out.str();
	}

	AppSettings MergeProviderSettings(const ProviderProfile& profile, const AppSettings& settings)
	{
		AppSettings merged = settings;

		if (!profile.command_template.empty())
		{
			merged.provider_command_template = profile.command_template;
		}

		const std::string provider_flags = JoinFlags(profile.runtime_flags);

		if (!provider_flags.empty() && merged.provider_extra_flags.empty())
		{
			merged.provider_extra_flags = provider_flags;
		}
		else if (!provider_flags.empty())
		{
			merged.provider_extra_flags = provider_flags + " " + merged.provider_extra_flags;
		}

		return merged;
	}

	class StandardProviderRuntime : public IProviderRuntime
	{
	  public:
		StandardProviderRuntime(const char* runtime_id, const bool enabled, const char* disabled_reason) : runtime_id_(runtime_id), enabled_(enabled), disabled_reason_(disabled_reason == nullptr ? "" : disabled_reason)
		{
		}

		const char* RuntimeId() const override
		{
			return runtime_id_;
		}

		bool IsEnabled() const override
		{
			return enabled_;
		}

		const char* DisabledReason() const override
		{
			return disabled_reason_.c_str();
		}

		std::string BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const override
		{
			return GeminiCommandBuilder::BuildPrompt(user_prompt, files);
		}

		std::string BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const override
		{
			if (!enabled_ || UsesInternalEngine(profile))
			{
				return "";
			}

			const AppSettings provider_settings = MergeProviderSettings(profile, settings);
			const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
			return GeminiCommandBuilder::BuildCommand(provider_settings, prompt, files, effective_resume_session_id);
		}

		std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override
		{
			const AppSettings provider_settings = MergeProviderSettings(profile, settings);

			if (!enabled_ || UsesInternalEngine(profile) || !profile.supports_interactive)
			{
				return {};
			}

			if ((profile.interactive_command.empty() || EqualsIgnoreCase(profile.interactive_command, "gemini")) && (profile.resume_argument.empty() || profile.resume_argument == "--resume") && profile.supports_resume)
			{
				return GeminiCommandBuilder::BuildInteractiveArgv(chat, provider_settings);
			}

			std::vector<std::string> argv = SplitCommandLineWords(profile.interactive_command);

			if (argv.empty())
			{
				argv.push_back(profile.id.empty() ? "provider-cli" : profile.id);
			}

			if (provider_settings.provider_yolo_mode)
			{
				argv.push_back("--yolo");
			}

			const std::vector<std::string> extra_flags = SplitCommandLineWords(provider_settings.provider_extra_flags);
			argv.insert(argv.end(), extra_flags.begin(), extra_flags.end());

			if (profile.supports_resume && chat.uses_native_session && !chat.native_session_id.empty() && !profile.resume_argument.empty())
			{
				argv.push_back(profile.resume_argument);
				argv.push_back(chat.native_session_id);
			}

			return argv;
		}

		MessageRole RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const override
		{
			if (AnyTypeMatches(profile.user_message_types, native_type))
			{
				return MessageRole::User;
			}

			if (AnyTypeMatches(profile.assistant_message_types, native_type))
			{
				return MessageRole::Assistant;
			}

			return MessageRole::System;
		}

		std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& gemini_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override
		{
			if (UsesNativeOverlayHistory(profile))
			{
				GeminiNativeHistoryStoreOptions native_options;
				native_options.max_file_bytes = options.native_max_file_bytes;
				native_options.max_messages = options.native_max_messages;
				return GeminiNativeHistoryStore::Load(gemini_chats_dir, profile, native_options);
			}

			return LocalChatStore::Load(data_root);
		}

		bool SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const override
		{
			return LocalChatStore::Save(data_root, chat);
		}

		bool UsesNativeOverlayHistory(const ProviderProfile& profile) const override
		{
			return SupportsGeminiJsonHistory(profile) && !UsesLocalHistory(profile);
		}

		bool SupportsGeminiJsonHistory(const ProviderProfile& profile) const override
		{
			return EqualsIgnoreCase(profile.history_adapter, "gemini-cli-json");
		}

		bool UsesLocalHistory(const ProviderProfile& profile) const override
		{
			return EqualsIgnoreCase(profile.history_adapter, "local-only") || profile.history_adapter.empty();
		}

		bool UsesInternalEngine(const ProviderProfile& profile) const override
		{
			return EqualsIgnoreCase(profile.execution_mode, "internal-engine");
		}

		bool UsesCliOutput(const ProviderProfile& profile) const override
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

		bool UsesStructuredOutput(const ProviderProfile& profile) const override
		{
			return !UsesCliOutput(profile);
		}

		bool UsesGeminiPathBootstrap(const ProviderProfile& profile) const override
		{
			return EqualsIgnoreCase(profile.prompt_bootstrap, "gemini-at-path");
		}

	  private:
		const char* runtime_id_ = "";
		bool enabled_ = true;
		std::string disabled_reason_;
	};

	class FixedPolicyProviderRuntime : public StandardProviderRuntime
	{
	  public:
		FixedPolicyProviderRuntime(const char* runtime_id,
		                           const bool enabled,
		                           const char* disabled_reason,
		                           const bool supports_gemini_json_history,
		                           const bool uses_local_history,
		                           const bool uses_internal_engine,
		                           const bool uses_cli_output,
		                           const bool uses_gemini_path_bootstrap) :
		    StandardProviderRuntime(runtime_id, enabled, disabled_reason),
		    supports_gemini_json_history_(supports_gemini_json_history),
		    uses_local_history_(uses_local_history),
		    uses_native_overlay_history_(supports_gemini_json_history && !uses_local_history),
		    uses_internal_engine_(uses_internal_engine),
		    uses_cli_output_(uses_cli_output),
		    uses_gemini_path_bootstrap_(uses_gemini_path_bootstrap)
		{
		}

		bool SupportsGeminiJsonHistory(const ProviderProfile&) const override
		{
			return supports_gemini_json_history_;
		}

		bool UsesLocalHistory(const ProviderProfile&) const override
		{
			return uses_local_history_;
		}

		std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& gemini_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override
		{
			if (uses_native_overlay_history_)
			{
				GeminiNativeHistoryStoreOptions native_options;
				native_options.max_file_bytes = options.native_max_file_bytes;
				native_options.max_messages = options.native_max_messages;
				return GeminiNativeHistoryStore::Load(gemini_chats_dir, profile, native_options);
			}

			return LocalChatStore::Load(data_root);
		}

		bool SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const override
		{
			return LocalChatStore::Save(data_root, chat);
		}

		bool UsesNativeOverlayHistory(const ProviderProfile&) const override
		{
			return uses_native_overlay_history_;
		}

		bool UsesInternalEngine(const ProviderProfile&) const override
		{
			return uses_internal_engine_;
		}

		bool UsesCliOutput(const ProviderProfile&) const override
		{
			return uses_cli_output_;
		}

		bool UsesStructuredOutput(const ProviderProfile&) const override
		{
			return !uses_cli_output_;
		}

		bool UsesGeminiPathBootstrap(const ProviderProfile&) const override
		{
			return uses_gemini_path_bootstrap_;
		}

	  private:
		bool supports_gemini_json_history_ = false;
		bool uses_local_history_ = true;
		bool uses_native_overlay_history_ = false;
		bool uses_internal_engine_ = false;
		bool uses_cli_output_ = true;
		bool uses_gemini_path_bootstrap_ = false;
	};

	class GeminiStructuredProviderRuntime final : public FixedPolicyProviderRuntime
	{
	  public:
		GeminiStructuredProviderRuntime() : FixedPolicyProviderRuntime("gemini-structured",
		                                                               UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED != 0,
		                                                               "Runtime 'gemini-structured' is disabled in this build (UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=OFF).",
		                                                               true,
		                                                               false,
		                                                               false,
		                                                               false,
		                                                               true)
		{
		}
	};

	class GeminiCliProviderRuntime final : public FixedPolicyProviderRuntime
	{
	  public:
		GeminiCliProviderRuntime() : FixedPolicyProviderRuntime("gemini-cli",
		                                                        UAM_ENABLE_RUNTIME_GEMINI_CLI != 0,
		                                                        "Runtime 'gemini-cli' is disabled in this build (UAM_ENABLE_RUNTIME_GEMINI_CLI=OFF).",
		                                                        true,
		                                                        false,
		                                                        false,
		                                                        true,
		                                                        true)
		{
		}
	};

	class CodexCliProviderRuntime final : public FixedPolicyProviderRuntime
	{
	  public:
		CodexCliProviderRuntime() : FixedPolicyProviderRuntime("codex-cli",
		                                                       UAM_ENABLE_RUNTIME_CODEX_CLI != 0,
		                                                       "Runtime 'codex-cli' is disabled in this build (UAM_ENABLE_RUNTIME_CODEX_CLI=OFF).",
		                                                       false,
		                                                       true,
		                                                       false,
		                                                       true,
		                                                       false)
		{
		}
	};

	class ClaudeCliProviderRuntime final : public FixedPolicyProviderRuntime
	{
	  public:
		ClaudeCliProviderRuntime() : FixedPolicyProviderRuntime("claude-cli",
		                                                        UAM_ENABLE_RUNTIME_CLAUDE_CLI != 0,
		                                                        "Runtime 'claude-cli' is disabled in this build (UAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF).",
		                                                        false,
		                                                        true,
		                                                        false,
		                                                        true,
		                                                        false)
		{
		}
	};

	class OpenCodeCliProviderRuntime final : public FixedPolicyProviderRuntime
	{
	  public:
		OpenCodeCliProviderRuntime() : FixedPolicyProviderRuntime("opencode-cli",
		                                                          UAM_ENABLE_RUNTIME_OPENCODE_CLI != 0,
		                                                          "Runtime 'opencode-cli' is disabled in this build (UAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF).",
		                                                          false,
		                                                          true,
		                                                          false,
		                                                          true,
		                                                          false)
		{
		}
	};

	class OpenCodeLocalProviderRuntime final : public FixedPolicyProviderRuntime
	{
	  public:
		OpenCodeLocalProviderRuntime() : FixedPolicyProviderRuntime("opencode-local",
		                                                            UAM_ENABLE_RUNTIME_OPENCODE_LOCAL != 0,
		                                                            "Runtime 'opencode-local' is disabled in this build (UAM_ENABLE_RUNTIME_OPENCODE_LOCAL=OFF).",
		                                                            false,
		                                                            true,
		                                                            false,
		                                                            true,
		                                                            false)
		{
		}
	};

	class OllamaEngineProviderRuntime final : public FixedPolicyProviderRuntime
	{
	  public:
		OllamaEngineProviderRuntime() : FixedPolicyProviderRuntime("ollama-engine",
		                                                           UAM_ENABLE_RUNTIME_OLLAMA_ENGINE != 0,
		                                                           "Runtime 'ollama-engine' is disabled in this build (UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=OFF).",
		                                                           false,
		                                                           true,
		                                                           true,
		                                                           false,
		                                                           false)
		{
		}
	};

	class UnknownProviderRuntime final : public StandardProviderRuntime
	{
	  public:
		UnknownProviderRuntime() : StandardProviderRuntime("custom-provider", true, "")
		{
		}
	};

	const IProviderRuntime& ResolveKnownRuntimeByLowercaseId(const std::string& lower_id)
	{
		static const GeminiStructuredProviderRuntime gemini_structured_runtime;
		static const GeminiCliProviderRuntime gemini_cli_runtime;
		static const CodexCliProviderRuntime codex_cli_runtime;
		static const ClaudeCliProviderRuntime claude_cli_runtime;
		static const OpenCodeCliProviderRuntime opencode_cli_runtime;
		static const OpenCodeLocalProviderRuntime opencode_local_runtime;
		static const OllamaEngineProviderRuntime ollama_engine_runtime;
		static const UnknownProviderRuntime unknown_runtime;

		if (lower_id == "gemini-structured")
		{
			return gemini_structured_runtime;
		}

		if (lower_id == "gemini-cli")
		{
			return gemini_cli_runtime;
		}

		if (lower_id == "codex-cli")
		{
			return codex_cli_runtime;
		}

		if (lower_id == "claude-cli")
		{
			return claude_cli_runtime;
		}

		if (lower_id == "opencode-cli")
		{
			return opencode_cli_runtime;
		}

		if (lower_id == "opencode-local")
		{
			return opencode_local_runtime;
		}

		if (lower_id == "ollama-engine")
		{
			return ollama_engine_runtime;
		}

		return unknown_runtime;
	}

} // namespace

const IProviderRuntime& ProviderRuntimeRegistry::Resolve(const ProviderProfile& profile)
{
	return ResolveById(profile.id);
}

const IProviderRuntime& ProviderRuntimeRegistry::ResolveById(const std::string& provider_id)
{
	return ResolveKnownRuntimeByLowercaseId(LowerAscii(provider_id));
}

bool ProviderRuntimeRegistry::IsKnownRuntimeId(const std::string& provider_id)
{
	const std::string lower_id = LowerAscii(provider_id);
	return lower_id == "gemini-structured" || lower_id == "gemini-cli" || lower_id == "codex-cli" || lower_id == "claude-cli" || lower_id == "opencode-cli" || lower_id == "opencode-local" || lower_id == "ollama-engine";
}

std::string ProviderRuntime::BuildPrompt(const ProviderProfile& profile, const std::string& user_prompt, const std::vector<std::string>& files)
{
	return ProviderRuntimeRegistry::Resolve(profile).BuildPrompt(profile, user_prompt, files);
}

std::string ProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id)
{
	return ProviderRuntimeRegistry::Resolve(profile).BuildCommand(profile, settings, prompt, files, resume_session_id);
}

std::vector<std::string> ProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings)
{
	return ProviderRuntimeRegistry::Resolve(profile).BuildInteractiveArgv(profile, chat, settings);
}

bool ProviderRuntime::IsRuntimeEnabled(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).IsEnabled();
}

bool ProviderRuntime::IsRuntimeEnabled(const std::string& provider_id)
{
	return ProviderRuntimeRegistry::ResolveById(provider_id).IsEnabled();
}

std::string ProviderRuntime::DisabledReason(const ProviderProfile& profile)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);

	if (runtime.IsEnabled())
	{
		return "";
	}

	const char* reason = runtime.DisabledReason();
	return reason == nullptr ? std::string() : std::string(reason);
}

std::string ProviderRuntime::DisabledReason(const std::string& provider_id)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(provider_id);

	if (runtime.IsEnabled())
	{
		return "";
	}

	const char* reason = runtime.DisabledReason();

	if (reason != nullptr && *reason != '\0')
	{
		return reason;
	}

	if (ProviderRuntimeRegistry::IsKnownRuntimeId(provider_id))
	{
		return "Runtime '" + provider_id + "' is disabled in this build.";
	}

	return "";
}

MessageRole ProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type)
{
	return ProviderRuntimeRegistry::Resolve(profile).RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> ProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& gemini_chats_dir, const ProviderRuntimeHistoryLoadOptions& options)
{
	return ProviderRuntimeRegistry::Resolve(profile).LoadHistory(profile, data_root, gemini_chats_dir, options);
}

bool ProviderRuntime::SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat)
{
	return ProviderRuntimeRegistry::Resolve(profile).SaveHistory(profile, data_root, chat);
}

bool ProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesNativeOverlayHistory(profile);
}

bool ProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).SupportsGeminiJsonHistory(profile);
}

bool ProviderRuntime::UsesLocalHistory(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesLocalHistory(profile);
}

bool ProviderRuntime::UsesInternalEngine(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesInternalEngine(profile);
}

bool ProviderRuntime::UsesCliOutput(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesCliOutput(profile);
}

bool ProviderRuntime::UsesStructuredOutput(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesStructuredOutput(profile);
}

bool ProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesGeminiPathBootstrap(profile);
}
