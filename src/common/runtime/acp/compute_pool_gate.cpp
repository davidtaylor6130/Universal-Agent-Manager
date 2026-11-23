#include "common/runtime/acp/compute_pool_gate.h"

#include "app/chat_domain_service.h"
#include "app/host_admission_integration.h"
#include "common/config/compute_pools.h"
#include "common/runtime/acp/acp_session_internal.h"

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Occupancy is derived from live turn state on every check. There is no slot count to release, so a turn that
// completes, fails or is cancelled, a dead process and a deleted chat cannot leave a slot taken.
namespace uam
{
	namespace
	{
		// A waiter that stops reaching the gate (its provider was disabled, for example) must not block the pool.
		constexpr double kStaleWaiterSeconds = 5.0;

		std::string TurnModelId(const AcpSessionState& session, const ChatSession& chat)
		{
			// A chat on its provider default has no model id; the provider reports the one in use.
			return strings::NonEmptyOrFallback(session.goal_turn_model_id, strings::NonEmptyOrFallback(chat.model_id, session.current_model_id));
		}

		bool InPool(const AppState& app, const AcpSessionState& session, const ComputePool& pool)
		{
			const ChatSession* chat = ChatDomainService().FindChatById(app, session.chat_id);
			return chat != nullptr && compute_pools::IsMember(pool, session.provider_id, TurnModelId(session, *chat));
		}

		/// A parent blocked in agent_wait uses no model time; counting it would deadlock a pool shared with its child.
		bool BlockedOnAgentWait(const AppState& app, const AcpSessionState& session)
		{
			return std::ranges::any_of(app.uam_control_capabilities, [&](const UamControlCapability& capability)
			{
				return capability.pending_agent_wait != nullptr && capability.session_chat_id == session.chat_id;
			});
		}

		/// A turn held by host admission waits for its computer, not for a pool: it must not hold a pool's place.
		bool HeldByHost(const AcpSessionState& session)
		{
			return !session.compute_pool_wait_message.empty();
		}

		bool Occupies(const AppState& app, const AcpSessionState& session)
		{
			// A remote turn being reconnected is still running on its host, unless the user has already stopped it.
			return (session.running || (session.recovering_remote_turn && !session.remote_stop_unconfirmed)) && session.processing &&
			    (session.queued_prompt.empty() || session.compute_pool_admitted_turn_serial == session.turn_serial) &&
			    !BlockedOnAgentWait(app, session);
		}

		int InUse(const AppState& app, const ComputePool& pool)
		{
			return static_cast<int>(std::ranges::count_if(app.acp_sessions, [&](const std::unique_ptr<AcpSessionState>& session)
			{
				return Occupies(app, *session) && InPool(app, *session, pool);
			}));
		}
	}

	namespace compute_pools
	{
		bool OccupiesTurnSlot(const AppState& app, const AcpSessionState& session)
		{
			return Occupies(app, session);
		}

		bool IsWaiting(const AcpSessionState& session, double now_seconds)
		{
			return session.compute_pool_ticket != 0 && session.compute_pool_ticket_turn_serial == session.turn_serial &&
			    session.compute_pool_admitted_turn_serial != session.turn_serial && session.running && session.processing &&
			    !session.queued_prompt.empty() && !session.queue_held && now_seconds - session.compute_pool_ticket_seen_s < kStaleWaiterSeconds;
		}

		bool HostStartHeld(const AcpSessionState& session, double now_seconds)
		{
			return !session.running && !session.compute_pool_wait_message.empty() && now_seconds - session.compute_pool_ticket_seen_s < kStaleWaiterSeconds;
		}

		bool HeldBeforeDelivery(const AcpSessionState& session)
		{
			return session.compute_pool_ticket != 0 && session.compute_pool_ticket_turn_serial == session.turn_serial && !session.queued_prompt.empty();
		}

		nlohmann::json UsageForFrontend(const AppState& app)
		{
			nlohmann::json usage = nlohmann::json::object();
			const double now = GetAppTimeSeconds();
			for (const ComputePool& pool : app.settings.compute_pools)
			{
				const int waiting = static_cast<int>(std::ranges::count_if(app.acp_sessions, [&](const std::unique_ptr<AcpSessionState>& session)
				{
					return IsWaiting(*session, now) && !HeldByHost(*session) && InPool(app, *session, pool);
				}));
				usage[pool.id] = {{"inUse", InUse(app, pool)}, {"waiting", waiting}};
			}
			return usage;
		}
	}

	namespace acp_detail
	{
		bool AdmitTurnToComputePools(AppState& app, AcpSessionState& session, const ChatSession& chat, bool& status_changed)
		{
			if (session.compute_pool_admitted_turn_serial == session.turn_serial) return true;
			const bool host_gate = app.host_admission != nullptr;
			// A turn held when the last pool was removed still needs the admission below, which resets its inactivity clocks.
			if (!host_gate && app.settings.compute_pools.empty() && session.compute_pool_wait_name.empty()) return true;
			const double now = GetAppTimeSeconds();
			const bool continuing = session.compute_pool_ticket != 0 && session.compute_pool_ticket_turn_serial == session.turn_serial;
			const double previous_seen = continuing ? session.compute_pool_ticket_seen_s : now;
			if (!continuing)
			{
				session.compute_pool_ticket = ++app.compute_pool_next_ticket;
				session.compute_pool_ticket_turn_serial = session.turn_serial;
			}
			session.compute_pool_ticket_seen_s = now;

			const auto hold = [&](const std::string& name, int in_use, int limit, const std::string& message, bool run_anyway)
			{
				status_changed = !continuing || session.compute_pool_wait_name != name || session.compute_pool_wait_in_use != in_use || session.compute_pool_wait_limit != limit ||
				    session.compute_pool_wait_message != message || session.compute_pool_wait_run_anyway != run_anyway;
				session.compute_pool_wait_name = name;
				session.compute_pool_wait_in_use = in_use;
				session.compute_pool_wait_limit = limit;
				session.compute_pool_wait_message = message;
				session.compute_pool_wait_run_anyway = run_anyway;
				// Time spent held is not time spent running: a sub-agent's run limit starts counting once it is admitted.
				if (const auto deadline = app.agent_run_deadline_steady_ms.find(session.managed_agent_run_id); deadline != app.agent_run_deadline_steady_ms.end())
					deadline->second += static_cast<std::int64_t>((now - previous_seen) * 1000.0);
				return false;
			};

			// Host admission first: the chat's computer, whatever routing chose it, applies its slot cap and load limits.
			// A refused turn takes its place in that computer's line. One the computer would admit is only looked at
			// here and admitted after the pools, so a turn a pool holds keeps no host slot it cannot use.
			host_admission::Request host_request;
			if (host_gate)
			{
				RefreshHostAdmission(app, now);
				host_request = HostAdmissionTurnRequest(app, &session, chat, now);
				if (!app.host_admission->Peek(host_request).admitted)
				{
					const host_admission::Decision decision = app.host_admission->Admit(host_request);
					if (!decision.admitted)
						return hold(HostAdmissionLabel(app, host_request.host_id), static_cast<int>(decision.in_use), static_cast<int>(decision.limit), decision.message, decision.run_anyway_offered);
				}
			}

			const std::string model_id = TurnModelId(session, chat);
			for (const ComputePool& pool : app.settings.compute_pools)
			{
				if (!compute_pools::IsMember(pool, session.provider_id, model_id)) continue;
				const int in_use = InUse(app, pool);
				const int ahead = static_cast<int>(std::ranges::count_if(app.acp_sessions, [&](const std::unique_ptr<AcpSessionState>& other)
				{
					return other.get() != &session && compute_pools::IsWaiting(*other, now) && !HeldByHost(*other) && other->compute_pool_ticket < session.compute_pool_ticket && InPool(app, *other, pool);
				}));
				if (in_use + ahead < pool.limit) continue;
				// A slot promised to an earlier waiter is as unavailable as one in use.
				return hold(pool.name, std::min(in_use + ahead, pool.limit), pool.limit, std::string(), false);
			}
			if (host_gate)
			{
				const host_admission::Decision decision = app.host_admission->Admit(host_request);
				if (!decision.admitted)
					return hold(HostAdmissionLabel(app, host_request.host_id), static_cast<int>(decision.in_use), static_cast<int>(decision.limit), decision.message, decision.run_anyway_offered);
				// The turn is live from now on; the reservation its provider start held is no longer needed.
				(void)app.host_admission->Release(host_request.key);
			}

			// The inactivity limits measure provider silence, which starts now for a turn that was held.
			if (!session.compute_pool_wait_name.empty()) session.turn_started_time_s = session.last_runtime_activity_time_s = now;
			session.compute_pool_admitted_turn_serial = session.turn_serial;
			session.compute_pool_ticket = 0;
			session.compute_pool_wait_name.clear();
			session.compute_pool_wait_message.clear();
			session.compute_pool_wait_run_anyway = false;
			session.host_admission_run_anyway = false;
			return true;
		}
	}
}
