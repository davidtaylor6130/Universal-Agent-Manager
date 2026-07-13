#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
struct AcpSessionState;
struct AppState;
} // namespace uam

/// <summary>
/// Runtime history load policy options.
/// </summary>
struct ProviderRuntimeHistoryLoadOptions
{
	std::uintmax_t native_max_file_bytes = 0;
	std::size_t native_max_messages = 0;
};

/// <summary>
/// A discovered chat source with its folder label and chat directory.
/// </summary>
struct ProviderChatSource
{
	std::string folder_title;
	std::string folder_directory;
	std::filesystem::path chats_dir;
};

/// <summary>
/// Result of a runtime's chat discovery scan.
/// </summary>
struct ProviderDiscoveryResult
{
	std::vector<ProviderChatSource> sources;
	std::string error;
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
	virtual bool IsEnabled() const { return true; }
	/// <summary>Returns a concise reason when runtime is disabled in this build.</summary>
	virtual const char* DisabledReason() const { return ""; }

	/// <summary>Builds interactive terminal argv for the active provider.</summary>
	virtual std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const = 0;
	/// <summary>Maps provider-native message types to app message roles.</summary>
	virtual MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const = 0;
	/// <summary>Loads history according to runtime policy.</summary>
	virtual std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const = 0;
	/// <summary>Saves chat according to runtime history policy.</summary>
	virtual bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const = 0;
	/// <summary>Returns true when runtime uses Gemini-native history plus local overlay.</summary>
	virtual bool UsesNativeOverlayHistory(const ProviderProfile& profile) const { (void)profile; return false; }
	/// <summary>Discovers all chat sources this runtime manages. Returns empty result when not supported.</summary>
	virtual ProviderDiscoveryResult DiscoverChatSources(const ProviderProfile& profile) const
	{
		(void)profile;
		return ProviderDiscoveryResult{};
	}

	/// <summary>Generates a UUID in the provider's native format.</summary>
	virtual std::string GenerateSessionUUID() const
	{
		return "";
	}
	/// <summary>Builds a session filename from chat data.</summary>
	virtual std::string BuildSessionFilename(const ChatSession& chat) const
	{
		(void)chat;
		return "session.json";
	}
	/// <summary>Maps UAM MessageRole to provider-native type string.</summary>
	virtual std::string NativeTypeFromRole(MessageRole role) const
	{
		(void)role;
		return "unknown";
	}
	/// <summary>Returns the native session directory for a workspace path.</summary>
	virtual std::filesystem::path GetNativeSessionDirectory(const std::filesystem::path& workspace_path) const
	{
		(void)workspace_path;
		return {};
	}
	/// <summary>Rebuilds a provider-native session file from UAM source of truth. Returns true on success.</summary>
	virtual bool RebuildNativeSessionFile(const ProviderProfile& profile, const ChatSession& chat, const std::filesystem::path& workspace_path) const
	{
		(void)profile;
		(void)chat;
		(void)workspace_path;
		return true;
	}

	/// <summary>Returns true when provider uses Gemini JSON history files.</summary>
	virtual bool SupportsGeminiJsonHistory(const ProviderProfile& profile) const
	{
		(void)profile;
		return false;
	}
	/// <summary>Returns true when provider persists via local chat storage only.</summary>
	virtual bool UsesLocalHistory(const ProviderProfile& profile) const
	{
		(void)profile;
		return true;
	}
	/// <summary>Returns true when provider execution is internal engine backed.</summary>
	virtual bool UsesInternalEngine(const ProviderProfile& profile) const
	{
		(void)profile;
		return false;
	}
	/// <summary>Returns true when provider output is fixed to CLI terminal mode.</summary>
	virtual bool UsesCliOutput(const ProviderProfile& profile) const
	{
		(void)profile;
		return true;
	}
	/// <summary>Returns true when prompt bootstrap should use @.gemini path injection.</summary>
	virtual bool UsesGeminiPathBootstrap(const ProviderProfile& profile) const
	{
		(void)profile;
		return false;
	}

	/// <summary>Builds the argv for a provider worker command (batch mode, no terminal).</summary>
	virtual std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const = 0;

	/// <summary>Builds the argv that launches the provider's structured (ACP/app-server) process over stdio.</summary>
	virtual std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const = 0;

	/// <summary>
	/// Returns true when <paramref name="tool_name"/> is a tool this provider treats as a sub-agent
	/// invocation, even when the ACP update does not carry explicit sub-agent metadata. The shared
	/// implementation recognizes the common task/delegate/spawn names; providers may extend it.
	/// </summary>
	virtual bool ProviderRecognizesSubagentTool(std::string_view tool_name) const;

	// == ACP Session Strategy ==

	/// <summary>ACP protocol variant for message routing.</summary>
	virtual const char* AcpProtocolKind() const { return ""; }

	/// <summary>Display name for this provider in ACP mode.</summary>
	virtual const char* GetAcpDisplayName() const { return RuntimeId(); }

	/// <summary>
	/// Build the initialize request. May set session fields if skipping wire (Claude).
	/// Returns empty to skip wire initialize.
	/// </summary>
	virtual nlohmann::json OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const;

	/// <summary>Called after initialize response received.</summary>
	virtual void OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const;

	/// <summary>
	/// Build the session setup request and return the method name via out_method.
	/// chat has resolved native_session_id. can_load indicates session load is supported.
	/// </summary>
	virtual nlohmann::json OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
	    const std::string& cwd, bool can_load, std::string& out_method) const;

	/// <summary>Validates a resume ID for this provider. Returns valid id or empty.</summary>
	virtual std::string OnAcpValidateResumeId(const ChatSession& chat) const;

	/// <summary>Build the prompt request and return the method name via out_method.</summary>
	virtual nlohmann::json OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
	    const std::string& prompt, const ChatSession& chat, std::string& out_method) const;

	/// <summary>True when prompt can be sent without session_id (Claude stream-json).</summary>
	virtual bool OnAcpCanSendPromptWithoutSessionId() const { return false; }

	/// <summary>
	/// Build cancel message and return method name via out_method (empty for notifications).
	/// Returns null/empty to skip cancel.
	/// </summary>
	virtual nlohmann::json OnAcpBuildCancel(const uam::AcpSessionState& session,
	    int request_id, std::string& out_method) const;

	/// <summary>Set mode locally. Return true if handled locally (no wire call).</summary>
	virtual bool OnAcpSetModeLocally(uam::AcpSessionState& session, const std::string& mode_id) const;

	/// <summary>Set model locally. Return true if handled locally (no wire call).</summary>
	virtual bool OnAcpSetModelLocally(uam::AcpSessionState& session, const std::string& model_id) const;

	/// <summary>Build permission response.</summary>
	virtual nlohmann::json OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
	    const std::string& option_id, bool cancelled) const;

	/// <summary>Try to auto-approve pending permission. Return true if approved.</summary>
	virtual bool OnAcpTryAutoApprove(uam::AcpSessionState& session, const ChatSession& chat,
	    std::string* error_out) const;

	/// <summary>Map approval mode ID for this provider.</summary>
	virtual std::string OnAcpMapApprovalModeId(const std::string& mode_id) const;

	/// <summary>True when this is a generic ACP provider (OpenCode, Copilot).</summary>
	virtual bool IsGenericAcpSession() const { return false; }
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
	static const IProviderRuntime& ResolveById(std::string_view provider_id);
	/// <summary>Returns true when provider id maps to an enabled runtime in this build.</summary>
	static bool IsEnabledRuntimeId(std::string_view provider_id);
};

/// <summary>
/// Provider-agnostic runtime adapter for prompt/command and history behavior.
/// </summary>
class ProviderRuntime
{
  public:

	/// <summary>Builds interactive terminal argv for the active provider.</summary>
	static std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings);
	/// <summary>Returns true when runtime backend for this profile is enabled in current build.</summary>
	static bool IsRuntimeEnabled(const ProviderProfile& profile);
	/// <summary>Returns true when runtime backend for this provider id is enabled in current build.</summary>
	static bool IsRuntimeEnabled(std::string_view provider_id);
	/// <summary>Returns disabled reason string for this profile runtime backend, or empty when enabled.</summary>
	static std::string DisabledReason(const ProviderProfile& profile);
	/// <summary>Returns disabled reason string for this provider id runtime backend, or empty when enabled.</summary>
	static std::string DisabledReason(std::string_view provider_id);

	/// <summary>Maps provider-native message types to app message roles.</summary>
	static MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type);
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
	/// <summary>Returns true when prompt bootstrap should use @.gemini path injection.</summary>
	static bool UsesGeminiPathBootstrap(const ProviderProfile& profile);
	/// <summary>Discovers all chat sources this runtime manages.</summary>
	static ProviderDiscoveryResult DiscoverChatSources(const ProviderProfile& profile);
	/// <summary>Rebuilds a provider-native session file from UAM source of truth.</summary>
	static bool RebuildNativeSessionFile(const ProviderProfile& profile, const ChatSession& chat, const std::filesystem::path& workspace_path);
	/// <summary>Returns true when this provider treats <paramref name="tool_name"/> as a sub-agent invocation.</summary>
	static bool ProviderRecognizesSubagentTool(const ProviderProfile& profile, std::string_view tool_name);
};
