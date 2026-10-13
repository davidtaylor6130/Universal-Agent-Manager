#pragma once

#include "common/state/app_state.h"

namespace uam::acp_detail
{
	inline constexpr const char* kClaudeQuotaContinuationPrompt = "Continue the interrupted turn from this native conversation. Preserve completed work. Do not repeat accepted prompts or completed tool calls. Check the outcome of any incomplete tool before deciding whether more work is needed.";

	std::string ClaudeQuotaContext(const AppState& app, const ChatSession& chat);
	void ObserveClaudeQuota(AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message, int64_t now_epoch_s);
	bool OwnClaudeQuotaWait(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& result, int64_t now_epoch_s);
	bool StartClaudeQuotaContinuation(AppState& app, ChatSession& chat, std::string* error_out);
	bool ValidateClaudeQuotaDelivery(const AppState& app, const AcpSessionState& session, const ChatSession& chat);
	bool CancelClaudeQuotaWait(AppState& app, ChatSession& chat);
	/// <summary>Eligible manual input persists its cancellation with the full unsent admission.</summary>
	bool PrepareClaudeQuotaCancellationForManualAdmission(AppState& app, ChatSession& chat);
	bool PollClaudeQuotaContinuations(AppState& app, int64_t now_epoch_s);
}
