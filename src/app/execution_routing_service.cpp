#include "execution_routing_service.h"

#include <unordered_set>

namespace uam
{
	namespace
	{
		ExecutionRoutingDecision InvalidSnapshot()
		{
			return {ExecutionRoutingStatus::Invalid, ExecutionRoutingReason::InconsistentSnapshot, {}};
		}

		bool AutomaticCandidateReady(const ExecutionRoutingCandidate& candidate)
		{
			return candidate.host_id != "local" && candidate.pool_enabled &&
			    candidate.provider_confirmed && candidate.runner_ready &&
			    candidate.runner_compatible && candidate.provider_supported &&
			    candidate.max_active_turns > 0 && !candidate.connection_identity.empty() &&
			    candidate.connection_identity == candidate.confirmed_connection_identity &&
			    !candidate.provider_configuration_identity.empty() &&
			    candidate.provider_configuration_identity == candidate.confirmed_provider_configuration_identity;
		}
	}

	ExecutionRoutingDecision ExecutionRoutingService::Admit(const ExecutionRoutingRequest& request,
	    const std::vector<ExecutionRoutingCandidate>& candidates,
	    const std::vector<ExecutionRoutingOccupancy>& occupancy)
	{
		std::scoped_lock lock(m_mutex);
		if (request.chat_id.empty()) return InvalidSnapshot();
		std::unordered_map<std::string, const ExecutionRoutingCandidate*> hosts;
		for (const ExecutionRoutingCandidate& candidate : candidates)
		{
			if (candidate.host_id.empty() || !hosts.emplace(candidate.host_id, &candidate).second)
				return InvalidSnapshot();
			std::unordered_set<std::string> sources;
			for (const ExecutionRoutingMapping& mapping : candidate.mappings)
				if (mapping.source_workspace_identity.empty() || mapping.destination_folder_id.empty() ||
				    mapping.destination_directory.empty() || !sources.insert(mapping.source_workspace_identity).second)
					return InvalidSnapshot();
		}
		std::unordered_map<std::string, std::string> occupied_hosts;
		for (const ExecutionRoutingOccupancy& occupied : occupancy)
		{
			if (occupied.chat_id.empty() || occupied.host_id.empty()) return InvalidSnapshot();
			const std::pair<std::unordered_map<std::string, std::string>::iterator, bool> inserted =
			    occupied_hosts.emplace(occupied.chat_id, occupied.host_id);
			if (!inserted.second && inserted.first->second != occupied.host_id) return InvalidSnapshot();
		}
		for (const std::pair<const std::string, ExecutionRoutingAssignment>& reserved : m_reservations)
		{
			const std::pair<std::unordered_map<std::string, std::string>::iterator, bool> inserted =
			    occupied_hosts.emplace(reserved.first, reserved.second.host_id);
			if (!inserted.second && inserted.first->second != reserved.second.host_id) return InvalidSnapshot();
		}
		std::unordered_map<std::string, std::size_t> counts;
		for (const std::pair<const std::string, std::string>& occupied : occupied_hosts) ++counts[occupied.second];
		ExecutionRoutingAssignment existing = request.assigned;
		const std::unordered_map<std::string, ExecutionRoutingAssignment>::const_iterator reserved = m_reservations.find(request.chat_id);
		if (reserved != m_reservations.end())
		{
			if (!existing.host_id.empty() && existing != reserved->second) return InvalidSnapshot();
			existing = reserved->second;
		}
		if (!existing.host_id.empty())
		{
			if (existing.chat_id != request.chat_id || existing.destination_folder_id.empty() || existing.destination_directory.empty())
				return InvalidSnapshot();
			const std::unordered_map<std::string, std::string>::const_iterator occupied = occupied_hosts.find(request.chat_id);
			if (occupied != occupied_hosts.end() && occupied->second != existing.host_id) return InvalidSnapshot();
			if (request.recovery_of_owned_work) m_reservations[request.chat_id] = existing;
			const std::unordered_map<std::string, const ExecutionRoutingCandidate*>::const_iterator owner = hosts.find(existing.host_id);
			if (owner == hosts.end() || !owner->second->runner_ready ||
			    !owner->second->runner_compatible || !owner->second->provider_supported)
				return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::HostUnavailable, existing};
			if (existing.connection_identity != owner->second->connection_identity ||
			    existing.provider_configuration_identity != owner->second->provider_configuration_identity)
				return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::OwnershipChanged, existing};
			if (existing.automatic && !request.recovery_of_owned_work)
			{
				if (!request.automatic_enabled || !AutomaticCandidateReady(*owner->second))
					return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::NoEligibleHost, existing};
				const std::size_t owned_slot = occupied != occupied_hosts.end() ? 1 : 0;
				if (counts[existing.host_id] - owned_slot >= owner->second->max_active_turns)
					return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::NoEligibleHost, existing};
			}
			m_reservations[request.chat_id] = existing;
			return {ExecutionRoutingStatus::Assigned, ExecutionRoutingReason::None, existing};
		}
		if (request.recovery_of_owned_work || occupied_hosts.contains(request.chat_id)) return InvalidSnapshot();
		// Explicit launches retain their existing policy. Only automatic selection
		// consumes readiness confirmations and configured pool limits.
		if (!request.explicit_host_id.empty())
		{
			const std::unordered_map<std::string, const ExecutionRoutingCandidate*>::const_iterator selected = hosts.find(request.explicit_host_id);
			if (selected == hosts.end()) return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::HostUnavailable, {}};
			if (request.explicit_folder_id.empty() || request.explicit_directory.empty()) return InvalidSnapshot();
			ExecutionRoutingAssignment assignment{request.chat_id, request.explicit_host_id,
			    selected->second->connection_identity, selected->second->provider_configuration_identity,
			    request.explicit_folder_id, request.explicit_directory};
			m_reservations[request.chat_id] = assignment;
			return {ExecutionRoutingStatus::Assigned, ExecutionRoutingReason::None, assignment};
		}
		if (!request.automatic_enabled)
			return {ExecutionRoutingStatus::Disabled, ExecutionRoutingReason::None, {}};
		if (request.source_workspace_identity.empty()) return InvalidSnapshot();
		const ExecutionRoutingCandidate* selected = nullptr;
		const ExecutionRoutingMapping* selected_mapping = nullptr;
		std::size_t selected_count = 0;
		for (const ExecutionRoutingCandidate& candidate : candidates)
		{
			if (!AutomaticCandidateReady(candidate)) continue;
			const std::size_t count = counts[candidate.host_id];
			if (count >= candidate.max_active_turns) continue;
			for (const ExecutionRoutingMapping& mapping : candidate.mappings)
			{
				if (mapping.source_workspace_identity != request.source_workspace_identity) continue;
				if (selected == nullptr || count < selected_count || (count == selected_count && candidate.host_id < selected->host_id))
				{
					selected = &candidate;
					selected_mapping = &mapping;
					selected_count = count;
				}
			}
		}
		if (selected == nullptr) return {};
		ExecutionRoutingAssignment assignment{request.chat_id, selected->host_id, selected->connection_identity,
		    selected->provider_configuration_identity, selected_mapping->destination_folder_id, selected_mapping->destination_directory, true};
		m_reservations[request.chat_id] = assignment;
		return {ExecutionRoutingStatus::Assigned, ExecutionRoutingReason::None, assignment};
	}

	bool ExecutionRoutingService::RestoreReservations(const std::vector<ExecutionRoutingAssignment>& assignments)
	{
		std::scoped_lock lock(m_mutex);
		std::unordered_map<std::string, ExecutionRoutingAssignment> restored = m_reservations;
		for (const ExecutionRoutingAssignment& assignment : assignments)
		{
			if (assignment.chat_id.empty() || assignment.host_id.empty() || assignment.destination_folder_id.empty() || assignment.destination_directory.empty()) return false;
			const std::pair<std::unordered_map<std::string, ExecutionRoutingAssignment>::iterator, bool> inserted = restored.emplace(assignment.chat_id, assignment);
			if (!inserted.second && inserted.first->second != assignment) return false;
		}
		m_reservations = std::move(restored);
		return true;
	}

	bool ExecutionRoutingService::Release(const std::string& chat_id, bool release_authorized)
	{
		if (!release_authorized) return false;
		std::scoped_lock lock(m_mutex);
		return m_reservations.erase(chat_id) != 0;
	}
}
