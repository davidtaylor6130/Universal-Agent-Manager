#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Host admission: the one gate every provider or helper process start asks before it runs on a computer.
// It keeps a ledger of live work per computer by class, applies each computer's slot cap, background budget and
// load thresholds, and orders waiters first come, first served with aging. It knows nothing about chats, sessions
// or settings files: callers describe computers, live work and requests, so the policy is testable on its own.
namespace uam::host_admission
{
	/// <summary>
	/// Why work runs, in priority order. Interactive is a user's own chat turn or CLI View; Agent is sub-agents, goal
	/// iterations and automation chats; Background is model discovery, memory workers, history import and version and
	/// repository checks. The order matches platform::ChildProcessClass (UserTurn, Agent, Background).
	/// </summary>
	enum class WorkClass
	{
		Interactive,
		Agent,
		Background
	};

	/// <summary>
	/// Ready: no limit is exceeded, or the load is unknown. Degraded: over the soft limit, so background work waits.
	/// Overloaded: over the hard limit, so new agent and background work waits and user turns wait with Run anyway.
	/// Unreachable: the last health check failed. Degraded and Overloaded are held for a back-off after the load drops.
	/// </summary>
	enum class Health
	{
		Ready,
		Degraded,
		Overloaded,
		Unreachable
	};

	/// <summary>
	/// One load reading for a computer, as the host load signal reports it. Unknown load (no reading, an older helper,
	/// a stale or failed read) uses the slot counts only and is never treated as an idle computer.
	/// </summary>
	struct LoadSample
	{
		bool known = false;
		/// Share of all processors busy, 0 to 100.
		std::optional<double> cpu_percent;
		/// Share of memory available, 0 to 100.
		std::optional<double> free_memory_percent;
		bool operator==(const LoadSample&) const = default;
	};

	/// <summary>
	/// Builds a sample from the figures the host load signal reports (platform::HostLoad): CPU use where the platform
	/// has it, otherwise the one-minute load average over the processor count, and available over total memory. A
	/// sample with neither figure is unknown.
	/// </summary>
	LoadSample LoadSampleFromFigures(std::optional<std::uint32_t> cores, std::optional<double> load_average_1m, std::optional<double> cpu_percent,
	    std::optional<std::uint64_t> memory_available_bytes, std::optional<std::uint64_t> memory_total_bytes);

	struct Thresholds
	{
		double soft_cpu_percent = 70.0;
		double soft_free_memory_percent = 15.0;
		double hard_cpu_percent = 90.0;
		/// CPU must stay over the hard limit this long; low memory is hard at once.
		double hard_cpu_seconds = 10.0;
		double hard_free_memory_percent = 10.0;
		bool operator==(const Thresholds&) const = default;
	};

	/// <summary>What the caller knows about one computer at this moment.</summary>
	struct HostLimits
	{
		std::string host_id;
		/// Shown in wait messages, for example "host A" or "this computer".
		std::string label;
		/// Interactive and agent work running at once; 0 means no cap.
		std::size_t slot_cap = 0;
		/// Background work running at once; 0 means background work never starts there.
		std::size_t background_budget = 2;
		bool reachable = true;
		LoadSample load;
	};

	/// <summary>A unit of live work derived from app state (a running turn, an open CLI View, a discovery session).</summary>
	struct LiveWork
	{
		std::string key;
		std::string host_id;
		WorkClass work_class = WorkClass::Interactive;
	};

	struct Request
	{
		/// Stable identity of this unit of work across attempts; it keeps the waiter's place in line.
		std::string key;
		std::string host_id;
		WorkClass work_class = WorkClass::Interactive;
		/// The user chose Run anyway: skip the slot cap and load limits for this start.
		bool run_anyway = false;
		/// Keep a held entry in the ledger after admission until Release(key). Work whose life is derived from app
		/// state each time (turns, terminals) leaves this false and appears through SetLive instead.
		bool hold = false;
		double now_s = 0.0;
	};

	enum class Reason
	{
		None,
		SlotsFull,
		BackgroundBudgetFull,
		Busy,
		Overloaded,
		Unreachable
	};

	struct Decision
	{
		bool admitted = false;
		Reason reason = Reason::None;
		/// Slots shown in the wait message: in use plus places promised to earlier waiters, at most the limit.
		std::size_t in_use = 0;
		std::size_t limit = 0;
		/// Only a user's own turn may skip a full or overloaded computer.
		bool run_anyway_offered = false;
		/// One plain line; empty when admitted.
		std::string message;
	};

	/// <summary>Per-computer counts for Settings and diagnostics.</summary>
	struct HostUsage
	{
		std::string host_id;
		std::size_t interactive = 0;
		std::size_t agent = 0;
		std::size_t background = 0;
		std::size_t waiting = 0;
		std::size_t slot_cap = 0;
		std::size_t background_budget = 0;
		Health health = Health::Ready;
		/// Seconds left of a Degraded or Overloaded back-off once the load has dropped; 0 otherwise.
		double backoff_remaining_s = 0.0;
	};

	const char* HealthName(Health health);
	const char* WorkClassName(WorkClass work_class);

	/// <summary>
	/// Thread safe. Waiters re-ask about once a second; each holds a ticket so a freed slot goes to the one that has
	/// waited longest. Interactive work goes before agent work on the shared slot cap, and an agent that has waited
	/// kAgentAgingSeconds is served as if it were interactive, so a stream of user turns cannot starve it. Background
	/// work has its own budget and its own line. A waiter silent for kQueueStaleSeconds holds no place, and after
	/// kQueueForgetSeconds its ticket is gone, matching the routing admission queue.
	/// </summary>
	class HostAdmission
	{
	public:
		static constexpr double kQueueStaleSeconds = 5.0;
		static constexpr double kQueueForgetSeconds = 60.0;
		static constexpr double kAgentAgingSeconds = 120.0;
		static constexpr double kBackoffInitialSeconds = 10.0;
		static constexpr double kBackoffMaxSeconds = 300.0;
		/// A host that becomes Degraded or Overloaded again within this long of clearing doubles its back-off.
		static constexpr double kBackoffMemorySeconds = 300.0;

		void SetThresholds(const Thresholds& thresholds);
		/// <summary>Replaces the known computers and re-evaluates each one's health from its load at now_s.</summary>
		void UpdateHosts(const std::vector<HostLimits>& hosts, double now_s);
		/// <summary>Replaces the live work derived from app state. Held entries from Admit stay until released.</summary>
		void SetLive(const std::vector<LiveWork>& live);
		/// <summary>Decides one start. A refused request keeps (or takes) its place in line for its computer and class.</summary>
		Decision Admit(const Request& request);
		/// <summary>Would this request be admitted now? Takes no ticket and changes nothing.</summary>
		Decision Peek(const Request& request);
		/// <summary>Drops a held entry. Returns whether one existed.</summary>
		bool Release(const std::string& key);
		/// <summary>Drops a waiter's place in line, for work that was stopped or no longer exists.</summary>
		bool Withdraw(const std::string& key);
		Health HealthOf(const std::string& host_id) const;
		std::vector<HostUsage> Usage(double now_s) const;
		/// <summary>Waiting keys for a computer in the order they would be served, for diagnostics and tests.</summary>
		std::vector<std::string> QueuedKeys(const std::string& host_id, double now_s) const;

	private:
		struct Waiter
		{
			std::uint64_t ticket = 0;
			std::string host_id;
			WorkClass work_class = WorkClass::Interactive;
			double first_seen_s = 0.0;
			double last_seen_s = 0.0;
		};

		struct HostState
		{
			HostLimits limits;
			Health health = Health::Ready;
			std::optional<double> hard_cpu_since_s;
			double hold_until_s = 0.0;
			double backoff_s = 0.0;
			double cleared_at_s = -1.0e18;
		};

		struct Counts
		{
			std::size_t interactive = 0;
			std::size_t agent = 0;
			std::size_t background = 0;
		};

		Decision Decide(const Request& request, bool register_waiter);
		Counts CountOn(const std::string& host_id, const std::string& excluding_key) const;
		bool Precedes(const Waiter& earlier, const Waiter& later, double now_s) const;
		std::string Label(const std::string& host_id) const;
		const HostState* Find(const std::string& host_id) const;

		mutable std::mutex m_mutex;
		Thresholds m_thresholds;
		std::unordered_map<std::string, HostState> m_hosts;
		std::unordered_map<std::string, LiveWork> m_live;
		std::unordered_map<std::string, LiveWork> m_held;
		std::unordered_map<std::string, Waiter> m_waiters;
		std::uint64_t m_next_ticket = 0;
	};

	/// <summary>Releases a held admission when it goes out of scope, for background workers on their own threads.</summary>
	class HeldLease
	{
	public:
		HeldLease() = default;
		HeldLease(std::shared_ptr<HostAdmission> owner, std::string key) : m_owner(std::move(owner)), m_key(std::move(key)) {}
		~HeldLease() { Reset(); }
		HeldLease(HeldLease&& other) noexcept : m_owner(std::move(other.m_owner)), m_key(std::move(other.m_key)) { other.m_owner.reset(); }
		HeldLease& operator=(HeldLease&& other) noexcept
		{
			if (this != &other)
			{
				Reset();
				m_owner = std::move(other.m_owner);
				m_key = std::move(other.m_key);
				other.m_owner.reset();
			}
			return *this;
		}
		HeldLease(const HeldLease&) = delete;
		HeldLease& operator=(const HeldLease&) = delete;
		void Reset()
		{
			if (m_owner) (void)m_owner->Release(m_key);
			m_owner.reset();
		}
		bool Holds() const { return m_owner != nullptr; }

	private:
		std::shared_ptr<HostAdmission> m_owner;
		std::string m_key;
	};
}
