#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"

#include "include/internal/cef_ptr.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class CefBrowser;

namespace uam
{
struct AcpSessionState;
struct AcpPendingPermissionState;
struct AcpPermissionOptionState;
struct AcpToolCallState;
struct AppState;
struct CliTerminalState;
namespace acp_detail
{
struct AcpResponseFailureDetails;
}
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

/// <summary>How a structured session applies a setting change.</summary>
enum class ProviderAcpSettingChangeAction
{
	SendRequest,
	KeepCurrent,
	ApplyLocally,
	RestartSession,
	SendRequestAndAwaitConfigOptions,
};

enum class SavedImageRetryPreflight
{
	Ready,
	Pending,
	Rejected,
};

/// Provider-owned observations can hold a request without allocating its wire identity.
enum class AcpRequestPreflight
{
	Ready,
	Pending,
	Rejected,
};

/// <summary>Authority granted by an offered native permission choice.</summary>
enum class ProviderPermissionChoice
{
	Denied,
	AllowedOnce,
	Other,
};

enum class ProviderCliVersionPolicy
{
	MinimumSemver,
	AnySafeToken,
};

/// <summary>Provider-owned native turn activity, independent of terminal rendering.</summary>
enum class ProviderTerminalActivity
{
	Unavailable,
	Busy,
	Complete,
};

/// <summary>Provider-owned CLI distribution and version compatibility policy.</summary>
struct ProviderCliPolicy
{
	std::string_view provider_id;
	std::string_view npm_package;
	std::string_view fallback_title;
	std::string_view executable_name;
	std::string_view version_probe_command;
	std::string_view homebrew_package;
	std::string_view winget_package;
	bool homebrew_cask = false;
	const char* preferred_version = nullptr;
	const char* fallback_version = nullptr;
	const char* minimum_version = nullptr;
	ProviderCliVersionPolicy version_policy = ProviderCliVersionPolicy::AnySafeToken;
	bool provider_managed = false;
	const char* verified_at = nullptr;
};

/// <summary>
/// Runtime-polymorphic provider backend contract.
/// </summary>
class IProviderRuntime
{
  public:
	virtual ~IProviderRuntime() = default;
	/// Converts native setup payloads immediately before wire encoding; must be idempotent.
	virtual void PrepareStructuredMessage(nlohmann::json&) const {}
	/// Native side channels may accept a provider-owned response without writing it to ACP stdio.
	virtual std::optional<bool> TryWriteAcpOutOfBandMessage(uam::AcpSessionState&, const nlohmann::json&, std::string*) const { return std::nullopt; }
	/// Prepare a native decision before the shared caller durably admits any policy change.
	virtual bool PrepareAcpUserInputResponse(uam::AppState&, uam::AcpSessionState&, ChatSession&,
	    const std::map<std::string, std::vector<std::string>>&, nlohmann::json&, std::string*) const { return true; }
	/// Register an admitted user decision after persistence and before transport delivery.
	virtual void NoteAcpUserInputResponse(uam::AcpSessionState&, const nlohmann::json&) const {}
	/// Register permission reply ownership only after its bytes enter the transport.
	virtual void NoteAcpPermissionResponse(uam::AcpSessionState&, const nlohmann::json&) const {}

	/// <summary>Canonical provider runtime id (for example `gemini-cli`).</summary>
	virtual const char* RuntimeId() const = 0;
	/// <summary>Reads the provider's local model cache without starting a process or fetching models.</summary>
	virtual nlohmann::json ReadLocalModelCatalog() const { return nlohmann::json::array(); }
	/// <summary>Returns CLI update and compatibility policy, or null for unsupported runtimes.</summary>
	virtual const ProviderCliPolicy* CliVersionPolicy() const { return nullptr; }
	/// <summary>Whether isolated text workers can run without native tools.</summary>
	virtual bool SupportsTextWorkers() const { return true; }
	/// <summary>Returns a local CLI compatibility error, or empty when this runtime permits launch.</summary>
	virtual std::string LocalCliCompatibilityError(const uam::AppState&) const { return {}; }
	/// <summary>Checks structured protocol compatibility for a checked executing-host CLI version, separately from installation policy.</summary>
	virtual std::string StructuredCliVersionError(const ProviderProfile&, std::string_view, bool = false, const ChatSession* = nullptr) const { return {}; }
	virtual bool RequiresStructuredCliVersion(const ProviderProfile&) const { return false; }
	/// Provider-owned generation selection after captured setup, before process creation.
	virtual bool PrepareStructuredLaunch(uam::AcpSessionState&, const ProviderProfile&, std::string_view, bool,
	    std::vector<std::string>&, std::vector<std::pair<std::string, std::string>>&, std::string&) const { return true; }
	/// <summary>Returns whether this runtime backend is enabled in the current build.</summary>
	virtual bool IsEnabled() const { return true; }
	/// <summary>Returns a concise reason when runtime is disabled in this build.</summary>
	virtual const char* DisabledReason() const { return ""; }

	/// <summary>Builds interactive terminal argv for the active provider.</summary>
	virtual std::vector<std::string> BuildNativeDiscoveryArgv(const ProviderProfile&) const { return {}; }
	/// Returns a direct native transcript export command, or empty when unsupported.
	virtual std::vector<std::string> BuildNativeExportArgv(const ProviderProfile&, const ChatSession&) const { return {}; }
	virtual std::vector<std::string> BuildLocalNativeDiscoveryArgv(const ProviderProfile& profile) const { return BuildNativeDiscoveryArgv(profile); }
	virtual std::vector<std::string> BuildLocalNativeExportArgv(const ProviderProfile& profile, const ChatSession& chat) const { return BuildNativeExportArgv(profile, chat); }
	virtual std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const = 0;
	/// <summary>Validates provider-specific terminal options before runtime handoff.</summary>
	virtual std::string InteractiveConfigurationError(const ProviderProfile& profile, const AppSettings& settings) const;
	/// <summary>Returns environment overrides for a local interactive provider process.</summary>
	virtual std::vector<std::pair<std::string, std::string>> BuildInteractiveEnvironment(const ProviderProfile& profile) const;
	/// Capture optional per-terminal native observation before private launch arguments and environment are staged.
	virtual bool PrepareInteractiveLaunch(uam::AppState&, uam::CliTerminalState&, const ChatSession&, const ExecutionHost&,
	    std::vector<std::string>&, std::vector<std::pair<std::string, std::string>>&, std::string*) const { return true; }
	/// <summary>Recognizes the provider's idle prompt in recent native terminal output.</summary>
	virtual bool RecentOutputIndicatesInputPrompt(std::string_view recent_output) const;
	/// <summary>Reads bounded native lifecycle changes for a verified local terminal binding.</summary>
	virtual ProviderTerminalActivity PollInteractiveActivity(uam::CliTerminalState&, std::string_view,
	    const std::filesystem::path&, bool) const { return ProviderTerminalActivity::Unavailable; }
	/// Whether this terminal owns a native observer for the captured execution host.
	virtual bool InteractiveObservationMatchesHost(const uam::CliTerminalState&, const ExecutionHost&) const { return false; }
	/// <summary>Checkpoints native history before deliberate terminal submission.</summary>
	virtual void CheckpointInteractiveSubmission(uam::CliTerminalState&) const {}
	/// <summary>Whether terminal output can confirm completion after a submitted turn.</summary>
	virtual bool SupportsInteractivePromptTracking() const { return true; }
	/// <summary>True when a native session must be created and saved before starting the CLI.</summary>
	virtual bool RequiresNativeSessionCreation() const { return false; }
	/// <summary>Creates an empty native session without sending a model prompt. Run off the UI thread.</summary>
	virtual std::string CreateNativeSession(const ProviderProfile& profile, const std::filesystem::path& workspace,
	    std::stop_token stop_token, std::string* error_out = nullptr, const ExecutionHost* remote_host = nullptr) const;
	/// <summary>Resolves the saved terminal binding according to the provider's native ID rules.</summary>
	virtual std::string ResolveInteractiveResumeId(const uam::AppState& app, const ChatSession& chat) const;
	/// <summary>Validates and persists provider session state before launching a local or remote CLI.</summary>
	virtual bool PrepareInteractiveSession(uam::AppState& app, ChatSession& chat, const ProviderProfile& profile,
	    const std::string& resume_id, const ExecutionHost& host, std::string* error_out) const;
	/// <summary>Captures native IDs before an unbound local CLI starts.</summary>
	virtual std::vector<std::string> SnapshotInteractiveSessionIds() const { return {}; }

	/// Reads saved native names keyed by session identity for local history imports.
	virtual std::unordered_map<std::string, std::string> ReadNativeSessionNames() const { return {}; }
	/// <summary>Discovers a new native binding using the provider's index and workspace rules.</summary>
	virtual std::string DiscoverInteractiveSessionId(const std::vector<std::string>&, const std::filesystem::path&, bool* ambiguous_out = nullptr) const
	{
		if (ambiguous_out != nullptr) *ambiguous_out = false;
		return {};
	}
	/// <summary>Maps provider-native message types to app message roles.</summary>
	virtual MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const = 0;
	/// <summary>Loads history according to runtime policy.</summary>
	virtual std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const = 0;
	/// <summary>Restores or validates native identity after loading local history, including recovered backups.</summary>
	virtual void NormalizeLoadedNativeSessionId(ChatSession& chat) const;
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

	/// Actual local passive-reader originals may use the sealed media transaction.
	/// This grants no provider input/resume authority and is opt-in per adapter.
	virtual bool SupportsLocalPassiveNativeMedia(const ProviderProfile&) const { return false; }

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
	/// Selects the checked local route before writing generation-specific isolation.
	virtual bool PrepareLocalWorkerLaunch(const uam::AppState&, const ProviderProfile&, std::vector<std::string>&, std::string&, bool* private_isolation = nullptr) const
	{
		if (private_isolation) *private_isolation = false;
		return true;
	}

	/// <summary>Builds the argv that launches the provider's structured (ACP/app-server) process over stdio.</summary>
	virtual std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const = 0;
	/// <summary>Returns environment values applied only to the structured provider child.</summary>
	virtual std::vector<std::pair<std::string, std::string>> BuildStructuredLaunchEnvironment(const ProviderProfile& profile, const ChatSession& chat) const
	{
		(void)profile;
		(void)chat;
		return {};
	}

	/// <summary>
	/// Returns true when <paramref name="tool_name"/> is a tool this provider treats as a sub-agent
	/// invocation, even when the ACP update does not carry explicit sub-agent metadata. The shared
	/// implementation recognizes the common task/delegate/spawn names; providers may extend it.
	/// </summary>
	virtual bool ProviderRecognizesSubagentTool(std::string_view tool_name) const;

	/// <summary>Restores provider-specific tool metadata from a native history item.</summary>
	virtual void ApplyNativeToolMetadata(ToolCall& tool, const nlohmann::json& native_item) const
	{
		(void)tool;
		(void)native_item;
	}
	/// <summary>Decodes provider extensions after common ACP tool metadata is applied.</summary>
	virtual void ApplyAcpToolMetadata(uam::AcpToolCallState& tool, const nlohmann::json& update) const
	{
		(void)tool;
		(void)update;
	}

	// == ACP Session Strategy ==

	/// <summary>ACP protocol variant for message routing.</summary>
	virtual const char* AcpProtocolKind() const { return ""; }

	/// <summary>Provider-owned active turn identity retained across remote attachment.</summary>
	virtual std::string AcpTurnIdentity(const uam::AcpSessionState&) const { return {}; }
	virtual void RestoreAcpTurnIdentity(uam::AcpSessionState&, const std::string&) const {}

	/// <summary>Display name for this provider in ACP mode.</summary>
	virtual const char* GetAcpDisplayName() const { return RuntimeId(); }

	/// <summary>
	/// Build the initialize request. May set session fields if skipping wire (Claude).
	/// Returns empty to skip wire initialize.
	/// </summary>
	virtual nlohmann::json OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const;

	/// <summary>Recognizes legacy prompt completion when a saved request map is unavailable.</summary>
	virtual std::string RecoverAcpResponseMethod(const nlohmann::json& message) const
	{
		const nlohmann::json::const_iterator result = message.find("result");
		if (result != message.end() && result->is_object())
		{
			const nlohmann::json::const_iterator reason = result->find("stopReason");
			if (reason != result->end() && reason->is_string()) return "session/prompt";
		}
		return {};
	}

	/// <summary>Handles a provider-specific successful response; false delegates to shared ACP handling.</summary>
	virtual bool OnAcpHandleResult(uam::AppState&, uam::AcpSessionState&, ChatSession&,
	    const std::string&, const std::string&, const nlohmann::json&) const { return false; }

	/// <summary>Observes a correlated cancelled prompt's result without ordinary completion or input admission.</summary>
	virtual void OnAcpObserveCancelledPromptResult(uam::AppState&, uam::AcpSessionState&,
	    ChatSession&, const nlohmann::json&) const {}

	/// <summary>Handles provider-specific response failures after shared diagnostics; false uses common recovery.</summary>
	virtual bool OnAcpHandleError(uam::AppState&, uam::AcpSessionState&, ChatSession&,
	    const uam::acp_detail::AcpResponseFailureDetails&) const { return false; }

	/// <summary>Retires ambiguous owned input without automatic reconnect/replay. False keeps legacy recovery.</summary>
	virtual bool OnAcpHoldUnconfirmedInput(uam::AppState&, uam::AcpSessionState&, ChatSession&,
	    const std::string&, std::optional<int> = std::nullopt) const { return false; }

	/// <summary>Projects provider-owned child activity; managed UAM child runs are counted separately.</summary>
	virtual std::size_t AcpBackgroundTaskCount(const uam::AcpSessionState& session) const;

	/// <summary>Optional captured-process guard before shared decoded-frame projection.
	/// A null frame reports decode failure before ordinary transport invalidation; it carries no raw payload.
	/// True consumes a decoded frame, never grants input/readiness. Ordinary adapters default to false.</summary>
	virtual bool OnAcpPreDispatch(uam::AppState&, uam::AcpSessionState&, const ChatSession&,
	    const nlohmann::json*) const noexcept { return false; }

	/// <summary>Consumes provider-specific stream messages; false delegates to shared ACP dispatch.</summary>
	virtual bool OnAcpHandleMessage(uam::AppState&, uam::AcpSessionState&, ChatSession&,
	    const nlohmann::json&, const CefRefPtr<CefBrowser>&) const { return false; }

	/// <summary>Called after initialize response received.</summary>
	virtual void OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const;

	/// <summary>
	/// Build the session setup request and return the method name via out_method.
	/// chat has resolved native_session_id. can_load indicates session load is supported.
	/// </summary>
	virtual nlohmann::json OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
	    const std::string& cwd, bool can_load, std::string& out_method) const;

	/// Runs before setup allocation or configuration; Pending must not be represented by an empty setup packet.
	virtual AcpRequestPreflight OnAcpPrepareSetup(uam::AppState&, uam::AcpSessionState&,
	    const ChatSession&, const std::string&, std::string&) const { return AcpRequestPreflight::Ready; }

	/// Session-aware adapters bind setup restrictions to their exact live observation generation.
	/// An empty packet with a nonempty method rejects setup; an empty method permits a no-wire setup.
	virtual nlohmann::json OnAcpBuildSessionSetupRequest(uam::AcpSessionState&, int request_id,
	    const ChatSession& chat, const std::string& cwd, bool can_load, std::string& out_method) const;

	/// Called under managed input ownership before preparation and again at final delivery.
	virtual AcpRequestPreflight OnAcpPrepareInput(uam::AppState&, uam::AcpSessionState&,
	    const ChatSession&, bool, std::string&) const { return AcpRequestPreflight::Ready; }

	/// <summary>Validates a resume ID for this provider. Returns valid id or empty.</summary>
	virtual std::string OnAcpValidateResumeId(const ChatSession& chat) const;

	/// <summary>Build the prompt request and return the method name via out_method.</summary>
	virtual nlohmann::json OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
	    const std::string& prompt, const ChatSession& chat, std::string& out_method) const;

	/// <summary>Builds native manual compaction; null means the provider has no direct request.</summary>
	virtual nlohmann::json OnAcpBuildCompact(const uam::AcpSessionState&, int, std::string&) const { return nullptr; }
	virtual bool SupportsSameLiveParentResultContinuation() const { return false; }
	virtual bool SupportsAcpCompaction() const { return false; }

	/// <summary>True when input can be appended to an active turn without interrupting it.</summary>
	virtual bool SupportsAcpSteering() const
	{
		return false;
	}
	/// Explicit saved-row original-byte input, through the bounded inline-image worker only.
	virtual bool SupportsSavedImageRetry(const uam::AcpSessionState&) const
	{
		return false;
	}
	/// Adapter opt-in for owned-image Retry on an already bound remote process;
	/// the capability grants no host, path, native-session or delivery authority.
	virtual bool SupportsRemoteOwnedImageRetry(const uam::AcpSessionState&) const { return false; }
	/// Read-only native evidence, before any original-image worker; existing routes need none.
	virtual SavedImageRetryPreflight PrepareSavedImageRetry(uam::AcpSessionState&, const ChatSession&,
	    const std::vector<MessageAttachment>&, std::string&) const
	{
		return SavedImageRetryPreflight::Ready;
	}
	/// Pure native settings witness before image preparation; never allocates an ID or writes.
	virtual nlohmann::json SavedImageRetryMetadata(const uam::AcpSessionState&, const ChatSession&) const
	{
		return nullptr;
	}
	/// Adapter-owned conversion of validated worker images; ordinary input never calls this hook.
	virtual bool EncodePreparedSavedImageRetry(nlohmann::json&, const nlohmann::json&, std::string& error,
	    const std::vector<MessageAttachment>& = {}, const nlohmann::json& = nullptr) const
	{
		error = "This provider has no prepared saved-image input route.";
		return false;
	}

	/// <summary>Builds native steering and records provider turn identity for its acknowledgment.</summary>
	virtual nlohmann::json OnAcpBuildSteer(uam::AcpSessionState&, int, const std::string&, std::string&) const
	{
		return nullptr;
	}

	/// <summary>True when prompt can be sent without session_id (Claude stream-json).</summary>
	virtual bool OnAcpCanSendPromptWithoutSessionId() const { return false; }

	/// <summary>
	/// Build cancel message and return method name via out_method (empty for notifications).
	/// Returns null/empty to skip cancel.
	/// </summary>
	virtual nlohmann::json OnAcpBuildCancel(const uam::AcpSessionState& session,
	    int request_id, std::string& out_method) const;

	/// <summary>Chooses mode-change transport, retaining provider-owned agent selection when needed.</summary>
	virtual ProviderAcpSettingChangeAction AcpModeChangeAction(const uam::AcpSessionState&) const { return ProviderAcpSettingChangeAction::SendRequest; }

	/// <summary>Chooses model-change transport and whether refreshed config options are required.</summary>
	virtual ProviderAcpSettingChangeAction AcpModelChangeAction() const { return ProviderAcpSettingChangeAction::SendRequest; }

	/// <summary>Named model efforts proven for one checked executing-host CLI version; empty means unavailable.</summary>
	virtual std::vector<std::string> ModelReasoningEfforts(std::string_view, std::string_view) const { return {}; }
	/// <summary>Launch-only model options require an idle process restart before taking effect.</summary>
	virtual bool ModelOptionsRequireRestart() const { return false; }
	/// <summary>Compare the actual launch request, never a provider-reported alias, for launch-only option changes.</summary>
	bool AcpLaunchOptionsNeedModelRestart(const uam::AcpSessionState& session, std::string_view model) const;

	/// <summary>Updates provider model metadata from native config options; returns whether it changed.</summary>
	virtual bool OnAcpConfigOptionsUpdated(uam::AcpSessionState&, const nlohmann::json&) const { return false; }
	/// <summary>Reconciles saved model options with the active provider session; returns whether it changed.</summary>
	virtual bool OnAcpReconcileModelOptions(uam::AppState&, uam::AcpSessionState&, ChatSession&) const { return false; }


	/// <summary>Expires adapter-owned native option requests during Poll; returns projection changes without dispatching or controlling a process.</summary>
	virtual bool OnAcpPollNativeModelOptions(uam::AppState&, uam::AcpSessionState&, ChatSession&) const { return false; }

	/// <summary>Refreshes native model options on an existing owned session; never launches or sends a user turn.</summary>
	virtual bool OnAcpRefreshNativeModelOptions(uam::AppState&, uam::AcpSessionState&, ChatSession&, std::string* error_out) const
	{
		if (error_out != nullptr) *error_out = "This provider does not expose native model options.";
		return false;
	}
	/// <summary>Admits a native option request under projected ownership; true means written, never committed.</summary>
	virtual bool OnAcpRequestNativeModelOption(uam::AppState&, uam::AcpSessionState&, ChatSession&, std::string_view,
	    const nlohmann::json&, const nlohmann::json&, std::string* error_out) const
	{
		if (error_out != nullptr) *error_out = "This provider does not expose native model options.";
		return false;
	}
	/// <summary>Allowlisted native option observations; excludes private request records and lifetime nonce.</summary>
	virtual nlohmann::json OnAcpNativeModelOptionsForFrontend(const uam::AcpSessionState&) const { return nlohmann::json::object(); }

	/// <summary>Build permission response.</summary>
	virtual nlohmann::json OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
	    const uam::AcpPendingPermissionState& pending, const std::string& option_id, bool cancelled) const;
	/// <summary>Encodes the current pending request through the explicit immutable request contract.</summary>
	nlohmann::json OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
	    const std::string& option_id, bool cancelled) const;

	/// <summary>Builds the provider's response to an active question, including cancellation.</summary>
	virtual nlohmann::json OnAcpBuildUserInputResponse(const uam::AcpSessionState& session, const std::map<std::string, std::vector<std::string>>& answers) const;

	/// <summary>Preserves provider recovery decisions that must not be automatically approved.</summary>
	virtual bool AcpPermissionRequiresUserDecision(const uam::AcpPendingPermissionState&) const { return false; }

	/// <summary>Identifies a native continuation question that grants no tool or filesystem action.</summary>
	virtual bool AcpPermissionIsNonActionQuestion(const uam::AcpPendingPermissionState&) const { return false; }

	/// <summary>Classifies native option authority without trusting its display label.</summary>
	virtual ProviderPermissionChoice AcpPermissionChoice(const uam::AcpPendingPermissionState&, const uam::AcpPermissionOptionState&) const;

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
	/// <summary>Restores or validates native identity after loading local history, including recovered backups.</summary>
	virtual void NormalizeLoadedNativeSessionId(ChatSession& chat) const;
	/// <summary>Saves chat according to runtime history policy.</summary>
	static bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat);
	/// <summary>Returns true when runtime uses Gemini-native history plus local overlay.</summary>
	static bool UsesNativeOverlayHistory(const ProviderProfile& profile);
	/// <summary>Returns true when provider uses Gemini JSON history files.</summary>
	static bool SupportsGeminiJsonHistory(const ProviderProfile& profile);
	/// <summary>Returns true when provider persists via local chat storage only.</summary>
	static bool UsesLocalHistory(const ProviderProfile& profile);
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

namespace uam::claude_input
{
	std::vector<MessageAttachment> TurnImages(const ChatSession& chat, int first, int last);
	/// Exact Claude stream envelope. Delivery mode validates pixels and fits images on a worker.
	nlohmann::json Build(const std::string& prompt, const std::filesystem::path& workspace,
	    const std::vector<MessageAttachment>& images, std::string& error,
	    bool prepare_delivery, std::stop_token cancel = {});
}
