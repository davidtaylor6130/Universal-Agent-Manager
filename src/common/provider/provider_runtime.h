#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/// <summary>
/// Runtime history load policy options.
/// </summary>
struct ProviderRuntimeHistoryLoadOptions
{
	std::uintmax_t native_max_file_bytes = 0;
	std::size_t native_max_messages = 0;
};

/// <summary>
/// Runtime-polymorphic provider backend contract.
/// </summary>
class IProviderRuntime
{
  public:
	virtual ~IProviderRuntime() = default;

	/// <summary>Canonical provider runtime id (for example `gemini-cli`).</summary>
	virtual const char* RuntimeId() const = 0;
	/// <summary>Returns whether this runtime backend is enabled in the current build.</summary>
	virtual bool IsEnabled() const = 0;
	/// <summary>Returns a concise reason when runtime is disabled in this build.</summary>
	virtual const char* DisabledReason() const = 0;

	/// <summary>Builds a provider prompt from user text and file references.</summary>
	virtual std::string BuildPrompt(const ProviderProfile& profile, const std::string& user_prompt, const std::vector<std::string>& files) const = 0;
	/// <summary>Builds the provider command for one request.</summary>
	virtual std::string BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const = 0;
	/// <summary>Builds interactive terminal argv for the active provider.</summary>
	virtual std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const = 0;
	/// <summary>Maps provider-native message types to app message roles.</summary>
	virtual MessageRole RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const = 0;
	/// <summary>Loads history according to runtime policy.</summary>
	virtual std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const = 0;
	/// <summary>Saves chat according to runtime history policy.</summary>
	virtual bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const = 0;
	/// <summary>Returns true when runtime uses Gemini-native history plus local overlay.</summary>
	virtual bool UsesNativeOverlayHistory(const ProviderProfile& profile) const = 0;

	/// <summary>Returns true when provider uses Gemini JSON history files.</summary>
	virtual bool SupportsGeminiJsonHistory(const ProviderProfile& profile) const = 0;
	/// <summary>Returns true when provider persists via local chat storage only.</summary>
	virtual bool UsesLocalHistory(const ProviderProfile& profile) const = 0;
	/// <summary>Returns true when provider execution is internal engine backed.</summary>
	virtual bool UsesInternalEngine(const ProviderProfile& profile) const = 0;
	/// <summary>Returns true when provider output is fixed to CLI terminal mode.</summary>
	virtual bool UsesCliOutput(const ProviderProfile& profile) const = 0;
	/// <summary>Returns true when provider output is fixed to structured chat mode.</summary>
	virtual bool UsesStructuredOutput(const ProviderProfile& profile) const = 0;
	/// <summary>Returns true when prompt bootstrap should use @.gemini path injection.</summary>
	virtual bool UsesGeminiPathBootstrap(const ProviderProfile& profile) const = 0;
};

/// <summary>
/// Runtime registry that maps provider IDs to concrete runtime classes.
/// </summary>
class ProviderRuntimeRegistry
{
  public:
	/// <summary>Resolves runtime implementation by provider profile.</summary>
	static const IProviderRuntime& Resolve(const ProviderProfile& profile);
	/// <summary>Resolves runtime implementation by provider id.</summary>
	static const IProviderRuntime& ResolveById(const std::string& provider_id);
	/// <summary>Returns true when provider id maps to a known runtime id.</summary>
	static bool IsKnownRuntimeId(const std::string& provider_id);
};

/// <summary>
/// Provider-agnostic runtime adapter for prompt/command and history behavior.
/// </summary>
class ProviderRuntime
{
  public:
	/// <summary>Builds a provider prompt from user text and file references.</summary>
	static std::string BuildPrompt(const ProviderProfile& profile, const std::string& user_prompt, const std::vector<std::string>& files);

	/// <summary>Builds the provider command for one request.</summary>
	static std::string BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id);

	/// <summary>Builds interactive terminal argv for the active provider.</summary>
	static std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings);
	/// <summary>Returns true when runtime backend for this profile is enabled in current build.</summary>
	static bool IsRuntimeEnabled(const ProviderProfile& profile);
	/// <summary>Returns true when runtime backend for this provider id is enabled in current build.</summary>
	static bool IsRuntimeEnabled(const std::string& provider_id);
	/// <summary>Returns disabled reason string for this profile runtime backend, or empty when enabled.</summary>
	static std::string DisabledReason(const ProviderProfile& profile);
	/// <summary>Returns disabled reason string for this provider id runtime backend, or empty when enabled.</summary>
	static std::string DisabledReason(const std::string& provider_id);

	/// <summary>Maps provider-native message types to app message roles.</summary>
	static MessageRole RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type);
	/// <summary>Loads history according to runtime policy.</summary>
	static std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options = {});
	/// <summary>Saves chat according to runtime history policy.</summary>
	static bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat);
	/// <summary>Returns true when runtime uses Gemini-native history plus local overlay.</summary>
	static bool UsesNativeOverlayHistory(const ProviderProfile& profile);

	/// <summary>Returns true when provider uses Gemini JSON history files.</summary>
	static bool SupportsGeminiJsonHistory(const ProviderProfile& profile);
	/// <summary>Returns true when provider persists via local chat storage only.</summary>
	static bool UsesLocalHistory(const ProviderProfile& profile);
	/// <summary>Returns true when provider execution is internal engine backed.</summary>
	static bool UsesInternalEngine(const ProviderProfile& profile);
	/// <summary>Returns true when provider output is fixed to CLI terminal mode.</summary>
	static bool UsesCliOutput(const ProviderProfile& profile);
	/// <summary>Returns true when provider output is fixed to structured chat mode.</summary>
	static bool UsesStructuredOutput(const ProviderProfile& profile);
	/// <summary>Returns true when prompt bootstrap should use @.gemini path injection.</summary>
	static bool UsesGeminiPathBootstrap(const ProviderProfile& profile);
};

