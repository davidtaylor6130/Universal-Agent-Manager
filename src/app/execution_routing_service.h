#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace uam
{
	struct ExecutionRoutingMapping
	{
		std::string source_workspace_identity;
		std::string destination_folder_id;
		std::string destination_directory;
		bool operator==(const ExecutionRoutingMapping&) const = default;
	};

	struct ExecutionRoutingCandidate
	{
		std::string host_id;
		std::string connection_identity;
		std::string confirmed_connection_identity;
		std::string provider_configuration_identity;
		std::string confirmed_provider_configuration_identity;
		bool pool_enabled = false;
		bool provider_confirmed = false;
		bool runner_ready = false;
		bool runner_compatible = false;
		bool provider_supported = false;
		std::size_t max_active_turns = 0;
		/// Host admission reports it over its hard load limit: no new automatic work is placed there.
		bool overloaded = false;
		/// Host admission reports every slot taken, by any work: treated as full whatever the routing count says.
		bool full = false;
		std::vector<ExecutionRoutingMapping> mappings;
	};

	/// Durable ownership supplied by the caller, including unresolved accepted deliveries.
	struct ExecutionRoutingAssignment
	{
		std::string chat_id;
		std::string host_id;
		std::string connection_identity;
		std::string provider_configuration_identity;
		std::string destination_folder_id;
		std::string destination_directory;
		bool automatic = false;
		bool operator==(const ExecutionRoutingAssignment&) const = default;
	};

	struct ExecutionRoutingRequest
	{
		std::string chat_id;
		bool automatic_enabled = false;
		bool recovery_of_owned_work = false;
		std::string source_workspace_identity;
		std::string explicit_host_id;
		std::string explicit_folder_id;
		std::string explicit_directory;
		ExecutionRoutingAssignment assigned;
		/// The assigned chat has not started: no provider session, process, delivered input or unsettled
		/// reservation. Such work may move to another eligible remote computer when its own is full, down or
		/// overloaded; it never moves to the local computer. Started work stays on its computer.
		bool movable = false;
		/// Monotonic time of this attempt. It orders nothing by itself: it only lets a waiter that stopped asking age out of the queue.
		double now_seconds = 0.0;
	};

	struct ExecutionRoutingOccupancy
	{
		std::string chat_id;
		std::string host_id;
	};

	enum class ExecutionRoutingStatus { Disabled, Assigned, Waiting, Invalid };
	enum class ExecutionRoutingReason { None, NoEligibleHost, HostUnavailable, OwnershipChanged, InconsistentSnapshot };

	/// Why one computer could not take a waiting chat, first match in this order: Use this computer is off, the helper
	/// is not Ready or compatible, the provider is not usable there, its sign-in is not confirmed for the current
	/// configuration, no folder maps the chat's workspace, or every concurrent session is in use.
	enum class ExecutionRoutingHostSkip { Off, Unreachable, ProviderUnavailable, Unconfirmed, NoMapping, Full };

	struct ExecutionRoutingSkippedHost
	{
		std::string host_id;
		ExecutionRoutingHostSkip reason = ExecutionRoutingHostSkip::Off;
		bool operator==(const ExecutionRoutingSkippedHost&) const = default;
	};

	struct ExecutionRoutingDecision
	{
		ExecutionRoutingStatus status = ExecutionRoutingStatus::Waiting;
		ExecutionRoutingReason reason = ExecutionRoutingReason::NoEligibleHost;
		ExecutionRoutingAssignment assignment;
		/// Diagnostics only, for a Waiting decision: each remote computer considered and why it was passed over.
		/// Filling it never changes which computer is chosen or whether the chat waits.
		std::vector<ExecutionRoutingSkippedHost> skipped;
		/// Set when an unstarted chat was given another computer; the caller must save the new destination.
		bool moved = false;
	};

	/// Serializes admission only. The caller must persist assignment before launch and
	/// supply active turns, workers and uncertain outboxes as a deduplicated snapshot.
	///
	/// Automatic chats that wait for capacity keep a ticket, so a freed slot goes to the chat that has waited
	/// longest on that computer rather than to whichever chat asks first. Each ticket names the computers its chat
	/// can use: an assigned chat only its own, a new chat every ready computer mapped for its workspace. A slot on a
	/// computer is open to a request only once every earlier waiter that can use it has been given one, earliest
	/// first, so an earlier waiter that fits elsewhere does not keep a computer idle. A waiter that stops asking
	/// ages out: after kQueueStaleSeconds it no longer holds a place, and after kQueueForgetSeconds its ticket is gone.
	class ExecutionRoutingService
	{
	public:
		/// Waiters re-ask about once a second; one silent this long (stopped, deleted, disabled) holds no slot.
		static constexpr double kQueueStaleSeconds = 5.0;
		/// A waiter silent this long loses its ticket and joins the back of the queue when it asks again.
		static constexpr double kQueueForgetSeconds = 60.0;

		ExecutionRoutingDecision Admit(const ExecutionRoutingRequest& request,
		    const std::vector<ExecutionRoutingCandidate>& candidates,
		    const std::vector<ExecutionRoutingOccupancy>& occupancy);
		/// Add durable reservations after restart. Contradictory ownership is rejected atomically.
		bool RestoreReservations(const std::vector<ExecutionRoutingAssignment>& assignments);
		/// Release after confirmed settlement/prelaunch failure, or explicit acknowledgement of unresolved ownership.
		bool Release(const std::string& chat_id, bool release_authorized);
		/// Drops a chat's place in the admission queue, for a chat that no longer exists. Returns whether it had one.
		bool Withdraw(const std::string& chat_id);
		/// The chats with a ticket, earliest first, including silent ones not yet forgotten. For diagnostics and tests.
		std::vector<std::string> QueuedChatIds();

		/// Chats occupying each computer exactly as admission counts them: the supplied occupancy plus held reservations.
		std::unordered_map<std::string, std::size_t> OccupiedCounts(const std::vector<ExecutionRoutingOccupancy>& occupancy) const;

	private:
		struct Waiter
		{
			std::uint64_t ticket = 0;
			double last_seen_s = 0.0;
			std::vector<std::string> hosts;
		};

		mutable std::mutex m_mutex;
		std::unordered_map<std::string, ExecutionRoutingAssignment> m_reservations;
		std::unordered_map<std::string, Waiter> m_waiters;
		std::uint64_t m_next_ticket = 0;
	};
}
