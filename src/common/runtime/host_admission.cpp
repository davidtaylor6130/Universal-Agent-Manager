#include "common/runtime/host_admission.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace uam::host_admission
{
	namespace
	{
		std::string Percent(double value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.0f", std::clamp(value, 0.0, 100.0));
			return buffer;
		}

		std::string Seconds(double value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.0f", std::max(1.0, std::ceil(value)));
			return buffer;
		}

		int Priority(WorkClass work_class, double waited_s)
		{
			if (work_class == WorkClass::Interactive) return 2;
			if (work_class == WorkClass::Agent) return waited_s >= HostAdmission::kAgentAgingSeconds ? 2 : 1;
			return 0;
		}
	}

	LoadSample LoadSampleFromFigures(std::optional<std::uint32_t> cores, std::optional<double> load_average_1m, std::optional<double> cpu_percent,
	    std::optional<std::uint64_t> memory_available_bytes, std::optional<std::uint64_t> memory_total_bytes)
	{
		LoadSample sample;
		if (cpu_percent && std::isfinite(*cpu_percent))
			sample.cpu_percent = std::clamp(*cpu_percent, 0.0, 100.0);
		else if (load_average_1m && std::isfinite(*load_average_1m) && cores && *cores > 0)
			sample.cpu_percent = std::clamp(*load_average_1m / static_cast<double>(*cores) * 100.0, 0.0, 100.0);
		if (memory_available_bytes && memory_total_bytes && *memory_total_bytes > 0)
			sample.free_memory_percent = std::clamp(static_cast<double>(std::min(*memory_available_bytes, *memory_total_bytes)) / static_cast<double>(*memory_total_bytes) * 100.0, 0.0, 100.0);
		sample.known = sample.cpu_percent.has_value() || sample.free_memory_percent.has_value();
		return sample;
	}

	const char* HealthName(Health health)
	{
		switch (health)
		{
		case Health::Degraded: return "degraded";
		case Health::Overloaded: return "overloaded";
		case Health::Unreachable: return "unreachable";
		default: return "ready";
		}
	}

	const char* WorkClassName(WorkClass work_class)
	{
		switch (work_class)
		{
		case WorkClass::Agent: return "agent";
		case WorkClass::Background: return "background";
		default: return "interactive";
		}
	}

	void HostAdmission::SetThresholds(const Thresholds& thresholds)
	{
		std::scoped_lock lock(m_mutex);
		m_thresholds = thresholds;
	}

	void HostAdmission::UpdateHosts(const std::vector<HostLimits>& hosts, double now_s)
	{
		std::scoped_lock lock(m_mutex);
		PruneHeld(now_s);
		std::unordered_map<std::string, HostState> next;
		for (const HostLimits& limits : hosts)
		{
			if (limits.host_id.empty()) continue;
			const std::unordered_map<std::string, HostState>::const_iterator previous = m_hosts.find(limits.host_id);
			HostState state = previous != m_hosts.end() ? previous->second : HostState{};
			state.limits = limits;
			const Health before = state.health;
			if (!limits.reachable)
			{
				// A failed health check says nothing about load; the held load state resumes if the host returns busy.
				state.hard_cpu_since_s.reset();
				state.health = Health::Unreachable;
				next[limits.host_id] = std::move(state);
				continue;
			}
			Health raw = Health::Ready;
			if (limits.load.known)
			{
				const std::optional<double>& cpu = limits.load.cpu_percent;
				const std::optional<double>& memory = limits.load.free_memory_percent;
				const bool over_hard_cpu = cpu && *cpu > m_thresholds.hard_cpu_percent;
				if (!over_hard_cpu) state.hard_cpu_since_s.reset();
				else if (!state.hard_cpu_since_s) state.hard_cpu_since_s = now_s;
				const bool hard = (memory && *memory < m_thresholds.hard_free_memory_percent) ||
				    (over_hard_cpu && now_s - *state.hard_cpu_since_s >= m_thresholds.hard_cpu_seconds);
				const bool soft = (cpu && *cpu > m_thresholds.soft_cpu_percent) || (memory && *memory < m_thresholds.soft_free_memory_percent);
				raw = hard ? Health::Overloaded : soft ? Health::Degraded : Health::Ready;
			}
			else
			{
				// Unknown load is not a quiet computer, but it is no evidence of a busy one either: slots alone decide.
				state.hard_cpu_since_s.reset();
			}
			const bool was_limited = before == Health::Degraded || before == Health::Overloaded;
			if (raw != Health::Ready && !was_limited)
				state.backoff_s = state.backoff_s > 0.0 && now_s - state.cleared_at_s <= kBackoffMemorySeconds
				    ? std::min(state.backoff_s * 2.0, kBackoffMaxSeconds) : kBackoffInitialSeconds;
			// Each level keeps its own hold: a soft reading extends only the Degraded hold, so a host that was briefly
			// Overloaded falls back to Degraded once its hard hold ends, even while it stays busy.
			if (raw == Health::Overloaded) state.hard_hold_until_s = std::max(state.hard_hold_until_s, now_s + state.backoff_s);
			if (raw != Health::Ready) state.hold_until_s = std::max(state.hold_until_s, now_s + state.backoff_s);
			const Health held = now_s < state.hard_hold_until_s ? Health::Overloaded : now_s < state.hold_until_s ? Health::Degraded : Health::Ready;
			if (held == Health::Ready && was_limited) state.cleared_at_s = now_s;
			if (held == Health::Ready) state.hold_until_s = state.hard_hold_until_s = 0.0;
			state.health = held;
			next[limits.host_id] = std::move(state);
		}
		m_hosts = std::move(next);
	}

	void HostAdmission::PruneHeld(double now_s)
	{
		for (auto it = m_held_until.begin(); it != m_held_until.end();)
		{
			if (now_s < it->second) { ++it; continue; }
			m_held.erase(it->first);
			it = m_held_until.erase(it);
		}
	}

	void HostAdmission::SetLive(const std::vector<LiveWork>& live)
	{
		std::scoped_lock lock(m_mutex);
		m_live.clear();
		for (const LiveWork& work : live)
			if (!work.key.empty() && !work.host_id.empty()) m_live[work.key] = work;
		// Work that is running is no longer waiting.
		for (const std::pair<const std::string, LiveWork>& work : m_live) m_waiters.erase(work.first);
	}

	Decision HostAdmission::Admit(const Request& request)
	{
		std::scoped_lock lock(m_mutex);
		return Decide(request, true);
	}

	Decision HostAdmission::Peek(const Request& request)
	{
		std::scoped_lock lock(m_mutex);
		return Decide(request, false);
	}

	bool HostAdmission::Release(const std::string& key)
	{
		std::scoped_lock lock(m_mutex);
		m_held_until.erase(key);
		return m_held.erase(key) != 0;
	}

	bool HostAdmission::Withdraw(const std::string& key)
	{
		std::scoped_lock lock(m_mutex);
		return m_waiters.erase(key) != 0;
	}

	Health HostAdmission::HealthOf(const std::string& host_id) const
	{
		std::scoped_lock lock(m_mutex);
		const HostState* host = Find(host_id);
		return host != nullptr ? host->health : Health::Ready;
	}

	const HostAdmission::HostState* HostAdmission::Find(const std::string& host_id) const
	{
		const std::unordered_map<std::string, HostState>::const_iterator found = m_hosts.find(host_id);
		return found != m_hosts.end() ? &found->second : nullptr;
	}

	std::string HostAdmission::Label(const std::string& host_id) const
	{
		const HostState* host = Find(host_id);
		return host != nullptr && !host->limits.label.empty() ? host->limits.label : host_id;
	}

	HostAdmission::Counts HostAdmission::CountOn(const std::string& host_id, const std::string& excluding_key) const
	{
		Counts counts;
		const auto add = [&](const LiveWork& work)
		{
			if (work.host_id != host_id || work.key == excluding_key) return;
			if (work.work_class == WorkClass::Interactive) ++counts.interactive;
			else if (work.work_class == WorkClass::Agent) ++counts.agent;
			else ++counts.background;
		};
		for (const std::pair<const std::string, LiveWork>& work : m_live) add(work.second);
		// A held entry that is also live is counted once.
		for (const std::pair<const std::string, LiveWork>& work : m_held)
			if (!m_live.contains(work.first)) add(work.second);
		return counts;
	}

	bool HostAdmission::Precedes(const Waiter& earlier, const Waiter& later, double now_s) const
	{
		const int earlier_priority = Priority(earlier.work_class, now_s - earlier.first_seen_s);
		const int later_priority = Priority(later.work_class, now_s - later.first_seen_s);
		if (earlier_priority != later_priority) return earlier_priority > later_priority;
		return earlier.ticket < later.ticket;
	}

	Decision HostAdmission::Decide(const Request& request, bool register_waiter)
	{
		const double now = request.now_s;
		PruneHeld(now);
		std::erase_if(m_waiters, [now](const std::pair<const std::string, Waiter>& waiter) { return now - waiter.second.last_seen_s > kQueueForgetSeconds; });
		const HostState* host = Find(request.host_id);
		HostLimits unknown_host;
		unknown_host.host_id = unknown_host.label = request.host_id;
		const HostLimits& limits = host != nullptr ? host->limits : unknown_host;
		const Health health = host != nullptr ? host->health : Health::Ready;
		const Counts counts = CountOn(request.host_id, request.key);

		Waiter own;
		if (const std::unordered_map<std::string, Waiter>::const_iterator found = m_waiters.find(request.key); found != m_waiters.end())
			own = found->second;
		else
		{
			own.ticket = register_waiter ? ++m_next_ticket : std::numeric_limits<std::uint64_t>::max();
			own.first_seen_s = now;
		}
		// A waiter that moved to another computer or class joins that line now.
		if (own.host_id != request.host_id || own.work_class != request.work_class)
		{
			if (!own.host_id.empty() && register_waiter) own.ticket = ++m_next_ticket;
			if (!own.host_id.empty()) own.first_seen_s = now;
			own.host_id = request.host_id;
			own.work_class = request.work_class;
		}
		own.last_seen_s = now;

		const std::string label = Label(request.host_id);
		const auto load_detail = [&]() -> std::string
		{
			if (host != nullptr && host->limits.load.known)
			{
				const LoadSample& load = host->limits.load;
				if (load.free_memory_percent && *load.free_memory_percent < m_thresholds.soft_free_memory_percent)
					return " (" + Percent(*load.free_memory_percent) + "% memory free)";
				if (load.cpu_percent && *load.cpu_percent > m_thresholds.soft_cpu_percent) return " (CPU " + Percent(*load.cpu_percent) + "%)";
			}
			if (host != nullptr)
			{
				const double until = host->health == Health::Overloaded ? host->hard_hold_until_s : host->hold_until_s;
				if (until > now) return ", rechecking in " + Seconds(until - now) + " s";
			}
			return {};
		};
		const auto refuse = [&](Reason reason, std::size_t in_use, std::size_t limit) -> Decision
		{
			if (register_waiter) m_waiters[request.key] = own;
			Decision decision;
			decision.reason = reason;
			decision.in_use = in_use;
			decision.limit = limit;
			decision.run_anyway_offered = request.work_class == WorkClass::Interactive && (reason == Reason::SlotsFull || reason == Reason::Overloaded);
			switch (reason)
			{
			case Reason::SlotsFull: decision.message = "Waiting for " + label + " (" + std::to_string(in_use) + "/" + std::to_string(limit) + ")"; break;
			case Reason::BackgroundBudgetFull: decision.message = "Waiting for " + label + " (background " + std::to_string(in_use) + "/" + std::to_string(limit) + ")"; break;
			case Reason::Busy: decision.message = "Waiting for " + label + ": busy" + load_detail(); break;
			case Reason::Overloaded: decision.message = "Waiting for " + label + ": overloaded" + load_detail(); break;
			case Reason::Unreachable: decision.message = "Waiting for " + label + " to be reachable"; break;
			default: break;
			}
			return decision;
		};
		const auto admit = [&]() -> Decision
		{
			if (register_waiter)
			{
				m_waiters.erase(request.key);
				if (request.hold)
				{
					m_held[request.key] = {request.key, request.host_id, request.work_class};
					if (request.hold_seconds > 0.0) m_held_until[request.key] = now + request.hold_seconds;
					else m_held_until.erase(request.key);
				}
			}
			Decision decision;
			decision.admitted = true;
			return decision;
		};
		const auto live_waiter = [&](const std::pair<const std::string, Waiter>& waiter)
		{
			return waiter.first != request.key && waiter.second.host_id == request.host_id && now - waiter.second.last_seen_s <= kQueueStaleSeconds &&
			    !m_live.contains(waiter.first) && !m_held.contains(waiter.first);
		};

		if (request.run_anyway && request.work_class != WorkClass::Background) return admit();
		if (request.work_class == WorkClass::Background)
		{
			if (health == Health::Unreachable) return refuse(Reason::Unreachable, 0, 0);
			if (health == Health::Overloaded) return refuse(Reason::Overloaded, 0, 0);
			if (health == Health::Degraded) return refuse(Reason::Busy, 0, 0);
			const std::size_t ahead = static_cast<std::size_t>(std::ranges::count_if(m_waiters, [&](const std::pair<const std::string, Waiter>& waiter)
			{
				return live_waiter(waiter) && waiter.second.work_class == WorkClass::Background && waiter.second.ticket < own.ticket;
			}));
			if (counts.background + ahead >= limits.background_budget)
				return refuse(Reason::BackgroundBudgetFull, std::max(counts.background, std::min(counts.background + ahead, limits.background_budget)), limits.background_budget);
			return admit();
		}
		if (health == Health::Unreachable && request.work_class == WorkClass::Agent) return refuse(Reason::Unreachable, 0, 0);
		if (health == Health::Overloaded) return refuse(Reason::Overloaded, 0, 0);
		if (limits.slot_cap == 0) return admit();
		const std::size_t in_use = counts.interactive + counts.agent;
		const std::size_t ahead = static_cast<std::size_t>(std::ranges::count_if(m_waiters, [&](const std::pair<const std::string, Waiter>& waiter)
		{
			return live_waiter(waiter) && waiter.second.work_class != WorkClass::Background && Precedes(waiter.second, own, now);
		}));
		if (in_use + ahead >= limits.slot_cap)
			return refuse(Reason::SlotsFull, std::max(in_use, std::min(in_use + ahead, limits.slot_cap)), limits.slot_cap);
		return admit();
	}

	std::vector<HostUsage> HostAdmission::Usage(double now_s) const
	{
		std::scoped_lock lock(m_mutex);
		std::vector<std::string> ids;
		for (const std::pair<const std::string, HostState>& host : m_hosts) ids.push_back(host.first);
		const auto note = [&ids](const std::string& id) { if (std::ranges::find(ids, id) == ids.end()) ids.push_back(id); };
		for (const std::pair<const std::string, LiveWork>& work : m_live) note(work.second.host_id);
		for (const std::pair<const std::string, LiveWork>& work : m_held) note(work.second.host_id);
		std::ranges::sort(ids);
		std::vector<HostUsage> usage;
		for (const std::string& id : ids)
		{
			const Counts counts = CountOn(id, {});
			HostUsage entry{id, counts.interactive, counts.agent, counts.background};
			entry.waiting = static_cast<std::size_t>(std::ranges::count_if(m_waiters, [&](const std::pair<const std::string, Waiter>& waiter)
			{ return waiter.second.host_id == id && now_s - waiter.second.last_seen_s <= kQueueStaleSeconds; }));
			if (const HostState* host = Find(id); host != nullptr)
			{
				entry.slot_cap = host->limits.slot_cap;
				entry.background_budget = host->limits.background_budget;
				entry.health = host->health;
				const double until = host->health == Health::Overloaded ? host->hard_hold_until_s : host->health == Health::Degraded ? host->hold_until_s : 0.0;
				if (until > now_s) entry.backoff_remaining_s = until - now_s;
			}
			usage.push_back(std::move(entry));
		}
		return usage;
	}

	std::vector<std::string> HostAdmission::QueuedKeys(const std::string& host_id, double now_s) const
	{
		std::scoped_lock lock(m_mutex);
		std::vector<std::pair<std::string, const Waiter*>> waiting;
		for (const std::pair<const std::string, Waiter>& waiter : m_waiters)
			if (waiter.second.host_id == host_id && now_s - waiter.second.last_seen_s <= kQueueStaleSeconds) waiting.emplace_back(waiter.first, &waiter.second);
		std::ranges::sort(waiting, [&](const std::pair<std::string, const Waiter*>& left, const std::pair<std::string, const Waiter*>& right)
		{
			const bool left_background = left.second->work_class == WorkClass::Background;
			const bool right_background = right.second->work_class == WorkClass::Background;
			if (left_background != right_background) return !left_background;
			if (left_background) return left.second->ticket < right.second->ticket;
			return Precedes(*left.second, *right.second, now_s);
		});
		std::vector<std::string> keys;
		for (const std::pair<std::string, const Waiter*>& entry : waiting) keys.push_back(entry.first);
		return keys;
	}
}
