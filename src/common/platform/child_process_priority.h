#pragma once

#include <string_view>

namespace uam::platform
{
	/// <summary>
	/// Why a child process is started, which decides its scheduling priority. A user turn keeps normal
	/// priority. Agent work (sub-agents, goal iterations, automation chats) and background work (model
	/// discovery, memory workers, history import, version and repository checks) run below it, so many
	/// agents or a background sweep cannot starve the interface or the turn the user is waiting on.
	/// Children inherit the lowered priority, so a provider's own tool processes stay lowered too.
	/// </summary>
	enum class ChildProcessClass
	{
		UserTurn,
		Agent,
		Background
	};

	/// <summary>The spawn options a class maps to on each platform.</summary>
	struct ChildProcessSpawnPriority
	{
		/// macOS: posix_spawnattr_set_qos_class_np(QOS_CLASS_UTILITY), a clamp on every thread of the child.
		bool macos_utility_qos = false;
		/// Linux and macOS: setpriority niceness applied before the child runs (0 leaves it unchanged).
		int unix_nice = 0;
		/// Windows: BELOW_NORMAL_PRIORITY_CLASS in the CreateProcess creation flags.
		bool windows_below_normal = false;

		constexpr bool Lowered() const { return macos_utility_qos || unix_nice != 0 || windows_below_normal; }
		constexpr bool operator==(const ChildProcessSpawnPriority&) const = default;
	};

	/// Same niceness #647 gives UAM's own background threads; CFS still grants it a weighted share.
	inline constexpr int kLoweredChildProcessNice = 10;

	constexpr ChildProcessSpawnPriority SpawnPriorityFor(ChildProcessClass process_class)
	{
		if (process_class == ChildProcessClass::UserTurn) return {};
		return {true, kLoweredChildProcessNice, true};
	}

	/// The lower of two classes: a lowered caller never raises a child, and a lowered child is never raised by its thread.
	constexpr ChildProcessClass LowerOf(ChildProcessClass left, ChildProcessClass right)
	{
		return static_cast<int>(left) >= static_cast<int>(right) ? left : right;
	}

	/// Wire names for the helper protocol and proxy spec; a user turn sends nothing, so older helpers see no change.
	constexpr std::string_view ChildProcessClassName(ChildProcessClass process_class)
	{
		switch (process_class)
		{
		case ChildProcessClass::Agent: return "agent";
		case ChildProcessClass::Background: return "background";
		default: return "user";
		}
	}

	/// Unknown or missing names keep normal priority, so a newer controller never makes an older helper fail a start.
	constexpr ChildProcessClass ParseChildProcessClass(std::string_view name)
	{
		if (name == "agent") return ChildProcessClass::Agent;
		if (name == "background") return ChildProcessClass::Background;
		return ChildProcessClass::UserTurn;
	}

	namespace detail
	{
		inline thread_local ChildProcessClass g_child_process_class = ChildProcessClass::UserTurn;
	}

	/// <summary>The class of processes this thread starts now. Every platform spawn path reads it.</summary>
	inline ChildProcessClass CurrentChildProcessClass() noexcept
	{
		return detail::g_child_process_class;
	}

	/// <summary>
	/// Processes this thread starts while the scope lives use at most this class's priority. Scopes nest and
	/// only lower: a background worker that starts a user-turn helper still starts it lowered.
	/// </summary>
	class ScopedChildProcessClass
	{
	public:
		explicit ScopedChildProcessClass(ChildProcessClass process_class) noexcept
		    : m_previous(detail::g_child_process_class)
		{
			detail::g_child_process_class = LowerOf(m_previous, process_class);
		}
		~ScopedChildProcessClass() { detail::g_child_process_class = m_previous; }
		ScopedChildProcessClass(const ScopedChildProcessClass&) = delete;
		ScopedChildProcessClass& operator=(const ScopedChildProcessClass&) = delete;

	private:
		ChildProcessClass m_previous;
	};
} // namespace uam::platform
