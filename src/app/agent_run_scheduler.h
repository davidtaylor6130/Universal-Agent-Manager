#pragma once

#include "common/state/app_state.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <string>

enum class ChatPublicationFile;
namespace uam::io
{
	enum class AtomicWriteStage;
}

namespace uam
{
	enum class ManagedFollowupAdmission;
	enum class ManagedFollowupCode;
	struct ManagedFollowupRequest;
	struct ManagedFollowupSubmitResult;
	struct ManagedFollowupState;
	enum class ManagedInitialMentionAdmission
	{
		NotApplicable,
		Pending,
		Rejected
	};
	enum class ManagedInitialDelegationState
	{
		Committed,
		Rejected,
		Uncertain
	};
	struct ManagedInitialMentionResult
	{
		bool input_recorded = false;
		ManagedInitialDelegationState delegation_state = ManagedInitialDelegationState::Rejected;
		std::optional<Message> parent_message;
		std::size_t parent_message_index = 0;
		std::string error, run_id, transcript_chat_id;
	};

	/// Immutable correlation for a native read-policy transaction, never execution authority by itself.
	struct ManagedNativeReadScope
	{
		std::string run_id, turn_id, process_id, root_birth_id, transcript_birth_id;
		std::string model_id, workspace_directory, native_session_id, configuration, child_policy;
		std::int64_t turn_serial = 0;
		bool resume_only = false;
		bool operator==(const ManagedNativeReadScope&) const = default;
	};

	struct AgentRunResult
	{
		std::string delivery_id;
		std::string run_id;
		std::string status;
		std::string agent_id;
		std::string output;
		std::string diagnostic_code;
		std::string diagnostic;
	};

	class AgentRunScheduler
	{
	  public:
		static ManagedInitialMentionAdmission TryBeginInitialMention(AppState&, const std::string& root_chat_id, const std::string& text, std::function<bool(const ManagedInitialMentionResult&)> completion, std::string* error_out = nullptr, const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& root_stage_hook_for_tests = {}, const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& parent_stage_hook_for_tests = {});
		static ManagedFollowupState GetFollowupState(const AppState&, const std::string& root_chat_id, const std::string& run_id);
		static ManagedFollowupAdmission TryBeginFollowup(AppState&, const std::string& root_chat_id, const std::string& run_id, const ManagedFollowupRequest&, std::function<bool(const ManagedFollowupSubmitResult&)> completion, ManagedFollowupCode* rejection_code = nullptr);
		static bool BindCurrentManagedPrompt(const AppState&, const ChatSession&, AcpQueuedUserPromptState&, std::string* error_out = nullptr);
		static bool ManagedResumeIdentityAllowed(AppState&, AcpSessionState&, const ChatSession&, std::string_view native_id);
		static std::optional<ManagedNativeReadScope> ManagedReadOnlyNativeScope(const AppState&, const AcpSessionState&, const ChatSession&);
		static bool ManagedReadPolicyRetained(const AppState&, const AcpSessionState&, const ChatSession&, const ManagedNativeReadScope&);
		static bool ManagedReadPolicyObserved(AppState&, AcpSessionState&, const ChatSession&, const ManagedNativeReadScope&, std::string_view native, std::string_view model, std::string_view workspace, std::string_view mode);
		static void RejectManagedReadPolicy(AppState&, AcpSessionState&, const ChatSession&, const ManagedNativeReadScope&, std::string_view diagnostic);
		static bool OwnsInitialRoutingPreparation(const AppState&, const ChatSession&);
		static void CancelPendingInitialMention(AppState&, std::string_view chat_id);
		static bool HasPendingInitialMention(const AppState&, std::string_view chat_id);
		/// A possible committed private ROOT prefix, not ordinary preparation or a confirmed live ACK.
		static bool HasUnconfirmedInitialRootPublication(const AppState&, const ChatSession&);
		static bool InitialSendAllowed(const AppState&, const ChatSession&, const std::string& text);
		static bool InitialLaunchAllowed(const AppState&, const AcpSessionState&, const ChatSession&, std::string* error_out);
		static bool PrepareInitialSetupRow(AppState&, AcpSessionState&, ChatSession&);
		static bool InitialRowAdmitted(AppState&, AcpSessionState&, const ChatSession&, std::string* error_out);
		static bool InitialInputReady(AppState&, AcpSessionState&, const ChatSession&, bool final_write = false);
		static void InitialInputAttempted(AcpSessionState&);
		static void InitialInputWritten(AcpSessionState&);
		static void InitialProcessStarted(AppState& app, AcpSessionState& session, const ChatSession& chat);
		/// Adapter-owned raw evidence only. Complete init can attest; later metadata can only contradict.
		static void InitialExecutionObserved(AppState&, AcpSessionState&, const ChatSession&, std::string_view native, std::string_view model, std::string_view native_mode, bool complete_init);
		/// Provisional correlated setup evidence; adapter parses native policy before applying defaults.
		static void InitialSetupExecutionObserved(AppState&, AcpSessionState&, const ChatSession&, std::string_view request_id, std::string_view native, std::string_view model, std::string_view workspace, bool read_only_openai_policy_confirmed);
		/// Exact owned Codex resume reply, or same-thread live effective settings when request_id is empty.
		/// Missing reply collaboration mode holds input; contradictory or incomplete live proof permanently refuses it.
		static bool InitialResumeSetupExecutionObserved(AppState&, AcpSessionState&, const ChatSession&, std::string_view request_id, std::string_view native, std::string_view model, std::string_view workspace, bool read_only_openai_policy_confirmed, std::string_view effective_mode, std::string_view collaboration_model, bool collaboration_supplied);
		/// Invalidation-only raw metadata comparison; absent fields cannot create or refresh proof.
		static void InitialExecutionProofObserved(AcpSessionState&, std::string_view native, std::string_view model, std::string_view workspace, bool supplied_fields_valid);
		static void InitialChatSaved(AppState&, const ChatSession&);
		static void InitialNativeResponse(AcpSessionState&, const std::string& method, int request_id);
		static bool InitialStopCompletionOwned(const AppState&, const std::shared_ptr<ManagedInitialRuntimeOwner>&);
		static void InitialProcessStopQueued(AcpSessionState&, AsyncAcpProcessStopTask&);
		static void InitialStopRequested(AcpSessionState& session);
		static void InitialTurnSettled(AcpSessionState&, std::string_view outcome, std::string_view error);

		/// Revokes initial input and drains real preparation/Stop work before the first force barrier.
		/// False means actual Stop or persisted terminal state remains unconfirmed; no replay authority is returned.
		static bool QuiesceInitialForShutdown(AppState& app);
		static bool InitialForceSaveAllowed(const AppState& app, std::string_view chat_id);
		static bool InitialSessionOwned(const AppState& app, const AcpSessionState& session, const ChatSession& chat);

		static bool Enqueue(AppState& app, const std::string& root_chat_id,
		                    const std::string& parent_run_id, const std::string& agent_id,
		                    const std::string& task, std::string* run_id_out = nullptr,
		                    std::string* error_out = nullptr, const std::string& provider_id = {},
		                    const std::string& model_id = {});
		static bool TryEnqueueMention(AppState& app, const std::string& root_chat_id,
		                              const std::string& text, bool* handled_out,
		                              std::string* run_id_out = nullptr,
		                              std::string* error_out = nullptr);
		static bool ResumeInterrupted(AppState& app, const std::string& run_id,
		                              std::string* new_run_id_out = nullptr,
		                              std::string* error_out = nullptr);
		static bool Poll(AppState& app);
		static bool GetResultForParent(const AppState& app, const std::string& run_id,
		                               const std::string& root_chat_id,
		                               const std::string& parent_run_id,
		                               AgentRunResult* result_out,
		                               std::string* error_out = nullptr);
		static bool CancelTree(AppState& app, const std::string& run_id,
		                       std::string* error_out = nullptr);
		static bool InterruptForShutdown(AppState& app);
		static bool PollAtForTests(AppState& app, int64_t now_epoch_ms);
	};
} // namespace uam
