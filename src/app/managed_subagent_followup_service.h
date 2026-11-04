#pragma once

#include "common/state/app_state.h"

#include <functional>

namespace uam
{
	enum class ManagedFollowupAdmission
	{
		Rejected, Pending
	};

	enum class ManagedFollowupCode
	{
		None, NotImplemented, InvalidRequest, LegacyIdentityUnverified,
		ScopeChanged, Busy, CancelPending, PermissionDenied, QueueFull,
		ReceiptLimit, TranscriptUnavailable, ModelIdentityUnverified,
		NativeResumeUnavailable, PersistenceFailed, DeliveryUnknown,
		IdempotencyConflict
	};

	struct ManagedFollowupRequest
	{
		std::string after_turn_id;
		std::string client_request_id;
		std::string text;
		ManagedRunContinuity continuity = ManagedRunContinuity::Native;
		ManagedRunRecoveryDecision recovery_decision = ManagedRunRecoveryDecision::None;
		std::string retry_of_turn_id;
	};

	struct ManagedFollowupAcceptance
	{
		std::string root_chat_id;
		std::string run_id;
		std::string transcript_chat_id;
		std::string client_request_id;
		std::string turn_id;
		std::uint32_t turn_number = 0;
		ManagedRunTurnStatus status_at_admission = ManagedRunTurnStatus::Queued;
		bool operator==(const ManagedFollowupAcceptance&) const = default;
	};

	struct ManagedFollowupState
	{
		bool can_send = false;
		ManagedFollowupCode code = ManagedFollowupCode::NotImplemented;
		std::string current_turn_id;
		std::uint32_t current_turn_number = 0;
	};

	struct ManagedFollowupSubmitResult
	{
		bool accepted = false;
		bool replayed = false;
		ManagedFollowupCode code = ManagedFollowupCode::NotImplemented;
		std::optional<ManagedFollowupAcceptance> acceptance;
	};

	class ManagedSubagentFollowupService
	{
	  public:
		static ManagedFollowupState GetState(const AppState& app,
			const std::string& root_chat_id, const std::string& run_id);
		// Opt-in local named-Claude/Codex UserRoot continuation; Codex requires read-only Plan proof, never startup adoption.
		// Pending owns preparation, not acceptance; completion bool acknowledges classified delivery only.
		static ManagedFollowupAdmission TrySubmit(AppState& app,
			const std::string& root_chat_id, const std::string& run_id,
			const ManagedFollowupRequest& request,
			std::function<bool(const ManagedFollowupSubmitResult&)> completion,
			ManagedFollowupCode* rejection_code = nullptr);
		static ManagedFollowupSubmitResult Submit(AppState& app,
			const std::string& root_chat_id, const std::string& run_id,
			const ManagedFollowupRequest& request);
	};
} // namespace uam
