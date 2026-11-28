#include "execution_routing_service.h"

#include <algorithm>
#include <optional>
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

		/// The least occupied automatic candidate with a free slot and a mapping for the workspace, other than
		/// excluded_host. Overloaded computers take no new work, and the local computer is never a candidate.
		std::pair<const ExecutionRoutingCandidate*, const ExecutionRoutingMapping*> SelectAutomaticHost(
		    const std::vector<ExecutionRoutingCandidate>& candidates, std::unordered_map<std::string, std::size_t>& counts,
		    const std::string& source_workspace_identity, const std::string& excluded_host)
		{
			const ExecutionRoutingCandidate* selected = nullptr;
			const ExecutionRoutingMapping* selected_mapping = nullptr;
			std::size_t selected_count = 0;
			for (const ExecutionRoutingCandidate& candidate : candidates)
			{
				if (!AutomaticCandidateReady(candidate) || candidate.overloaded || candidate.full || candidate.host_id == excluded_host) continue;
				const std::size_t count = counts[candidate.host_id];
				if (count >= candidate.max_active_turns) continue;
				for (const ExecutionRoutingMapping& mapping : candidate.mappings)
				{
					if (mapping.source_workspace_identity != source_workspace_identity) continue;
					if (selected == nullptr || count < selected_count || (count == selected_count && candidate.host_id < selected->host_id))
					{
						selected = &candidate;
						selected_mapping = &mapping;
						selected_count = count;
					}
				}
			}
			return {selected, selected_mapping};
		}

		using HostCandidates = std::unordered_map<std::string, const ExecutionRoutingCandidate*>;
		using HostCounts = std::unordered_map<std::string, std::size_t>;

		/// The least occupied of these computers that still has a slot, by the same rule a new chat is placed with.
		const ExecutionRoutingCandidate* LeastOccupiedWithSlot(const std::vector<std::string>& names, const HostCandidates& hosts, HostCounts& counts)
		{
			const ExecutionRoutingCandidate* chosen = nullptr;
			for (const std::string& name : names)
			{
				const HostCandidates::const_iterator host = hosts.find(name);
				// A computer that is off or not ready cannot take the waiter, so it holds nothing there.
				if (host == hosts.end() || !host->second->pool_enabled || !host->second->runner_ready || counts[name] >= host->second->max_active_turns) continue;
				if (chosen == nullptr || counts[name] < counts[chosen->host_id] || (counts[name] == counts[chosen->host_id] && name < chosen->host_id))
					chosen = host->second;
			}
			return chosen;
		}

		/// The first condition of AutomaticCandidateReady and the capacity and mapping checks that this computer fails.
		ExecutionRoutingHostSkip SkipReason(const ExecutionRoutingCandidate& candidate, const std::string& source_workspace_identity)
		{
			if (!candidate.pool_enabled) return ExecutionRoutingHostSkip::Off;
			if (!candidate.runner_ready || !candidate.runner_compatible) return ExecutionRoutingHostSkip::Unreachable;
			if (!candidate.provider_supported) return ExecutionRoutingHostSkip::ProviderUnavailable;
			if (!candidate.provider_confirmed || candidate.connection_identity.empty() || candidate.connection_identity != candidate.confirmed_connection_identity ||
			    candidate.provider_configuration_identity.empty() || candidate.provider_configuration_identity != candidate.confirmed_provider_configuration_identity)
				return ExecutionRoutingHostSkip::Unconfirmed;
			if (!source_workspace_identity.empty() && std::ranges::none_of(candidate.mappings, [&source_workspace_identity](const ExecutionRoutingMapping& mapping)
			    { return mapping.source_workspace_identity == source_workspace_identity; }))
				return ExecutionRoutingHostSkip::NoMapping;
			return ExecutionRoutingHostSkip::Full;
		}

		ExecutionRoutingDecision Waiting(ExecutionRoutingReason reason, ExecutionRoutingAssignment assignment, std::string host_id, ExecutionRoutingHostSkip skip)
		{
			return {ExecutionRoutingStatus::Waiting, reason, std::move(assignment), {{std::move(host_id), skip}}};
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
		// Admission queue: drop waiters long gone, then give each earlier waiter that is still asking a slot on the
		// least occupied computer it can use, earliest ticket first. Automatic admission sees only what is left.
		const double now = request.now_seconds;
		std::erase_if(m_waiters, [now](const std::pair<const std::string, Waiter>& waiter) { return now - waiter.second.last_seen_s > kQueueForgetSeconds; });
		{
			const std::unordered_map<std::string, Waiter>::const_iterator own_ticket = m_waiters.find(request.chat_id);
			std::vector<const Waiter*> earlier;
			for (const std::pair<const std::string, Waiter>& waiter : m_waiters)
				if (waiter.first != request.chat_id && !occupied_hosts.contains(waiter.first) && now - waiter.second.last_seen_s <= kQueueStaleSeconds &&
				    (own_ticket == m_waiters.end() || waiter.second.ticket < own_ticket->second.ticket))
					earlier.push_back(&waiter.second);
			std::ranges::sort(earlier, {}, &Waiter::ticket);
			for (const Waiter* waiter : earlier)
				if (const ExecutionRoutingCandidate* held = LeastOccupiedWithSlot(waiter->hosts, hosts, counts); held != nullptr)
					++counts[held->host_id];
		}
		const auto wait_in_queue = [this, &request, now](std::vector<std::string> eligible)
		{
			const std::pair<std::unordered_map<std::string, Waiter>::iterator, bool> entry = m_waiters.try_emplace(request.chat_id);
			if (entry.second)
			{
				if (eligible.empty())
				{
					m_waiters.erase(entry.first);
					return;
				}
				entry.first->second.ticket = ++m_next_ticket;
			}
			entry.first->second.last_seen_s = now;
			entry.first->second.hosts = std::move(eligible);
		};
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
			// Work that has not started may move to another eligible remote computer when its own is full, down or
			// overloaded. It keeps its workspace (the mapping must match), never moves to the local computer, and
			// started work never moves.
			const auto move_unstarted = [&]() -> std::optional<ExecutionRoutingDecision>
			{
				if (!request.movable || !existing.automatic || request.recovery_of_owned_work || !request.automatic_enabled ||
				    request.source_workspace_identity.empty()) return std::nullopt;
				// The caller's snapshot is the evidence of running work; this service's own reservation is not.
				if (std::ranges::any_of(occupancy, [&request](const ExecutionRoutingOccupancy& work) { return work.chat_id == request.chat_id; })) return std::nullopt;
				const auto [target, mapping] = SelectAutomaticHost(candidates, counts, request.source_workspace_identity, existing.host_id);
				if (target == nullptr) return std::nullopt;
				ExecutionRoutingAssignment moved{request.chat_id, target->host_id, target->connection_identity,
				    target->provider_configuration_identity, mapping->destination_folder_id, mapping->destination_directory, true};
				m_reservations[request.chat_id] = moved;
				m_waiters.erase(request.chat_id);
				ExecutionRoutingDecision decision{ExecutionRoutingStatus::Assigned, ExecutionRoutingReason::None, moved};
				decision.moved = true;
				return decision;
			};
			const std::unordered_map<std::string, const ExecutionRoutingCandidate*>::const_iterator owner = hosts.find(existing.host_id);
			if (owner == hosts.end() || !owner->second->runner_ready ||
			    !owner->second->runner_compatible || !owner->second->provider_supported)
			{
				if (std::optional<ExecutionRoutingDecision> moved = move_unstarted()) return *moved;
				return Waiting(ExecutionRoutingReason::HostUnavailable, existing, existing.host_id,
				    owner != hosts.end() && owner->second->runner_ready && owner->second->runner_compatible ? ExecutionRoutingHostSkip::ProviderUnavailable : ExecutionRoutingHostSkip::Unreachable);
			}
			if (existing.connection_identity != owner->second->connection_identity ||
			    existing.provider_configuration_identity != owner->second->provider_configuration_identity)
				return Waiting(ExecutionRoutingReason::OwnershipChanged, existing, existing.host_id, ExecutionRoutingHostSkip::Unconfirmed);
			if (existing.automatic && !request.recovery_of_owned_work)
			{
				const std::size_t owned_slot = occupied != occupied_hosts.end() ? 1 : 0;
				if (!request.automatic_enabled || !AutomaticCandidateReady(*owner->second))
					return Waiting(ExecutionRoutingReason::NoEligibleHost, existing, existing.host_id,
					    request.automatic_enabled ? SkipReason(*owner->second, {}) : ExecutionRoutingHostSkip::Off);
				// Host admission's full or overloaded view only offers unstarted work a move. Otherwise the chat stays
				// assigned and waits in its computer's host admission line, with its one-line wait (and Run anyway).
				if (owner->second->overloaded || owner->second->full)
					if (std::optional<ExecutionRoutingDecision> moved = move_unstarted()) return *moved;
				if (counts[existing.host_id] - owned_slot >= owner->second->max_active_turns)
				{
					if (std::optional<ExecutionRoutingDecision> moved = move_unstarted()) return *moved;
					// An assigned chat that cannot move can only ever run on its own computer, so it queues for that one alone.
					wait_in_queue({existing.host_id});
					return Waiting(ExecutionRoutingReason::NoEligibleHost, existing, existing.host_id, ExecutionRoutingHostSkip::Full);
				}
			}
			m_reservations[request.chat_id] = existing;
			m_waiters.erase(request.chat_id);
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
		std::vector<std::string> eligible;
		// Every ready computer with a mapping can take the chat later, even one host admission holds full or overloaded now.
		for (const ExecutionRoutingCandidate& candidate : candidates)
			if (AutomaticCandidateReady(candidate) && std::ranges::any_of(candidate.mappings, [&request](const ExecutionRoutingMapping& mapping)
			    { return mapping.source_workspace_identity == request.source_workspace_identity; }))
				eligible.push_back(candidate.host_id);
		const auto [selected, selected_mapping] = SelectAutomaticHost(candidates, counts, request.source_workspace_identity, {});
		if (selected == nullptr)
		{
			// Only a chat some ready computer could take is waiting for capacity; one with none keeps no new place.
			wait_in_queue(std::move(eligible));
			ExecutionRoutingDecision waiting;
			for (const ExecutionRoutingCandidate& candidate : candidates)
				if (candidate.host_id != "local") waiting.skipped.push_back({candidate.host_id, SkipReason(candidate, request.source_workspace_identity)});
			return waiting;
		}
		ExecutionRoutingAssignment assignment{request.chat_id, selected->host_id, selected->connection_identity,
		    selected->provider_configuration_identity, selected_mapping->destination_folder_id, selected_mapping->destination_directory, true};
		m_reservations[request.chat_id] = assignment;
		m_waiters.erase(request.chat_id);
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

	std::unordered_map<std::string, std::size_t> ExecutionRoutingService::OccupiedCounts(const std::vector<ExecutionRoutingOccupancy>& occupancy) const
	{
		std::scoped_lock lock(m_mutex);
		std::unordered_map<std::string, std::string> occupied_hosts;
		for (const ExecutionRoutingOccupancy& occupied : occupancy)
			if (!occupied.chat_id.empty() && !occupied.host_id.empty()) occupied_hosts.emplace(occupied.chat_id, occupied.host_id);
		for (const std::pair<const std::string, ExecutionRoutingAssignment>& reserved : m_reservations) occupied_hosts.emplace(reserved.first, reserved.second.host_id);
		std::unordered_map<std::string, std::size_t> counts;
		for (const std::pair<const std::string, std::string>& occupied : occupied_hosts) ++counts[occupied.second];
		return counts;
	}

	bool ExecutionRoutingService::Release(const std::string& chat_id, bool release_authorized)
	{
		if (!release_authorized) return false;
		std::scoped_lock lock(m_mutex);
		return m_reservations.erase(chat_id) != 0;
	}

	bool ExecutionRoutingService::Withdraw(const std::string& chat_id)
	{
		std::scoped_lock lock(m_mutex);
		return m_waiters.erase(chat_id) != 0;
	}

	std::vector<std::string> ExecutionRoutingService::QueuedChatIds()
	{
		std::scoped_lock lock(m_mutex);
		std::vector<std::pair<std::uint64_t, std::string>> queued;
		for (const std::pair<const std::string, Waiter>& waiter : m_waiters) queued.emplace_back(waiter.second.ticket, waiter.first);
		std::ranges::sort(queued);
		std::vector<std::string> ids;
		for (std::pair<std::uint64_t, std::string>& entry : queued) ids.push_back(std::move(entry.second));
		return ids;
	}
}
