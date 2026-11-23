#pragma once

#include "common/runtime/host_admission.h"
#include "common/state/app_state.h"

#include <nlohmann/json_fwd.hpp>

#include <string>

// Connects the host admission policy (common/runtime/host_admission.h) to app state: which computers exist and their
// limits, what is running on each, and the one call each process-start path makes. The gate is active once
// app.host_admission exists (the application creates it at startup); tools and tests that do not create it keep
// their previous behaviour.
namespace uam
{
	/// The host id admission uses: trimmed, and "local" when empty.
	std::string HostAdmissionHostId(const std::string& host_id);

	/// "this computer" for the local host, otherwise the computer's label.
	std::string HostAdmissionLabel(const AppState& app, const std::string& host_id);

	/// The class of a chat's work: model discovery is background; sub-agents, goal iterations and automation chats
	/// are agent work; every other chat is its user's own interactive work.
	host_admission::WorkClass HostAdmissionClassForChat(const AcpSessionState* session, const ChatSession& chat);

	/// Limits per computer: the local computer's own limit (default processors minus two), a remote computer's
	/// routing slot count or the remote default, the background budget, reachability and the latest load reading.
	std::vector<host_admission::HostLimits> HostAdmissionLimits(const AppState& app);

	/// Live work derived from app state: running chat turns, open CLI Views and model discovery sessions.
	std::vector<host_admission::LiveWork> HostAdmissionLiveWork(const AppState& app);

	/// Refreshes thresholds, limits, health and live work. Cheap; every gate calls it before deciding.
	void RefreshHostAdmission(AppState& app, double now_s);

	/// The app poll: refreshes health about once a second even when nothing asks, so the hard-limit timer, the
	/// back-off and routing's view of overloaded computers stay current, and drops stale background waits.
	bool PollHostAdmission(AppState& app);

	/// A chat turn's admission request: the chat's computer and class, and the user's Run anyway choice.
	host_admission::Request HostAdmissionTurnRequest(const AppState& app, const AcpSessionState* session, const ChatSession& chat, double now_s);

	/// One background start (model discovery, a memory worker, history import, a version or repository check). When
	/// admitted with hold, the returned lease keeps the place until it is destroyed: move it into the worker so it is
	/// released when the worker ends. Refused starts record their one-line wait and must be retried later.
	/// Always admitted when the gate is not active.
	struct BackgroundAdmission
	{
		bool admitted = false;
		std::string message;
		host_admission::HeldLease lease;
	};
	BackgroundAdmission AdmitBackgroundStart(AppState& app, const std::string& key, const std::string& host_id, bool hold = true);

	/// Settings page projection: per computer counts by class, waiting, limits and health.
	nlohmann::json HostAdmissionUsageForFrontend(const AppState& app);
}
