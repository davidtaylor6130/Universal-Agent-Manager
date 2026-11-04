#pragma once

#include <cstddef>
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
	};

	struct ExecutionRoutingOccupancy
	{
		std::string chat_id;
		std::string host_id;
	};

	enum class ExecutionRoutingStatus { Disabled, Assigned, Waiting, Invalid };
	enum class ExecutionRoutingReason { None, NoEligibleHost, HostUnavailable, OwnershipChanged, InconsistentSnapshot };

	struct ExecutionRoutingDecision
	{
		ExecutionRoutingStatus status = ExecutionRoutingStatus::Waiting;
		ExecutionRoutingReason reason = ExecutionRoutingReason::NoEligibleHost;
		ExecutionRoutingAssignment assignment;
	};

	/// Serializes admission only. The caller must persist assignment before launch and
	/// supply active turns, workers and uncertain outboxes as a deduplicated snapshot.
	class ExecutionRoutingService
	{
	public:
		ExecutionRoutingDecision Admit(const ExecutionRoutingRequest& request,
		    const std::vector<ExecutionRoutingCandidate>& candidates,
		    const std::vector<ExecutionRoutingOccupancy>& occupancy);
		/// Add durable reservations after restart. Contradictory ownership is rejected atomically.
		bool RestoreReservations(const std::vector<ExecutionRoutingAssignment>& assignments);
		/// Release after confirmed settlement/prelaunch failure, or explicit acknowledgement of unresolved ownership.
		bool Release(const std::string& chat_id, bool release_authorized);

	private:
		std::mutex m_mutex;
		std::unordered_map<std::string, ExecutionRoutingAssignment> m_reservations;
	};
}
