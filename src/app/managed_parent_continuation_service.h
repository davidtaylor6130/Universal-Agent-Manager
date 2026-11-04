#pragma once

#include "common/state/app_state.h"
#include "common/chat/chat_repository.h"
#include <functional>

namespace uam
{
	enum class ManagedNativeTurnObservation { Unrelated, Owned, Quarantined };
	/// Runtime authority only; no serializer or constructor from persisted DATA.
	struct ManagedParentContinuationLease
	{
		std::filesystem::path data_root;
		std::shared_ptr<const std::uint8_t> controller, chat_birth;
		std::shared_ptr<const char> policy_birth;
		const AcpSessionState* session = nullptr;
		std::string id, chat_id, process, native_thread, configuration, user_digest;
		std::vector<std::pair<std::string, std::string>> runs;
		SavedChatPrimaryWitness primary;
		ManagedParentResultRecord expected_record;
		int origin_serial = 0, expected_serial = 0, request_id = 0;
		std::string origin_native_turn, tentative_started_turn, tentative_completed_turn, accepted_turn, retired_rpc_turn;
		std::string input_receipt_id;
		std::vector<ManagedParentInputReceipt> expected_input_receipts;
		std::string pending_retired_ack_turn;
		bool retired_ack_save_attempted = false;
		bool admission_pending = false;
		bool committed = false, normal_origin = false, revoked = false, internal_turn = false;
	};

	/// Only the runtime admission path constructs this typed non-human input.
	struct ManagedResultPromptInput
	{
		std::string text;
		std::function<bool()> owned;
		std::function<bool(int)> prepared;
		std::function<void(int)> written;
		std::function<void()> failed;
	};

	class ManagedParentContinuationService
	{
	public:
		static std::string StageDelegation(AppState&, const UamControlCapability&, const AgentRun&);
		static bool DelegationCommitConfirmed(AppState&, std::string_view root_id);
		static void Revoke(AppState&, std::string_view root_id, std::string_view fixed_reason);
		static void ObserveNormalCompletion(AppState&, AcpSessionState&, ChatSession&, std::string_view native_turn);
		static bool ObserveNativeStartResult(AppState&, AcpSessionState&, ChatSession&, std::string_view request_id, std::string_view turn_id);
		static bool ObserveNativeStartFailure(AppState&, AcpSessionState&, ChatSession&, std::string_view request_id);
		static ManagedNativeTurnObservation ObserveNativeTurnNotification(AppState&, AcpSessionState&, ChatSession&, std::string_view thread, std::string_view turn, bool started);
		static int PendingNativeAckRequest(const AcpSessionState&);
		static void ObservePrimarySaved(AcpSessionState&, const SavedChatPrimaryWitness&);
		static bool Poll(AppState&);
	};
}
