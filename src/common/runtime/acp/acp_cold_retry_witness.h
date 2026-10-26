#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::cold_retry
{
	// Transition fence shared by the owning launch/save paths and process-free fixtures.
	// Supplied facts are DATA, never a saved-row permission or a process-launch API.
	struct Identity
	{
		const void* runtime = nullptr;
		std::string process;
		std::weak_ptr<const void> epoch, writer, observer;
	};
	struct Launch
	{
		std::string working_directory;
		std::vector<std::string> argv;
		std::vector<std::pair<std::string, std::string>> environment;
		bool operator==(const Launch&) const = default;
	};

	class Sequence final
	{
	  public:
		enum class Phase { Prepared, Launching, Launched, Observing, Observed, BindingPending, Bound, Ready, PacketPending, FinalReady, Attempted, Refused };
		using Current = std::function<bool()>;

		Sequence(const void* owner, Current owner_current, Current publication_current)
		    : m_owner(owner), m_owner_current(std::move(owner_current)), m_publication_current(std::move(publication_current))
		{
		}
		Sequence(const Sequence&) = delete;
		Sequence& operator=(const Sequence&) = delete;
		Phase State() const { return m_phase; }
		bool InputAttempted() const { return m_attempted; }
		bool Refuse()
		{
			m_phase = Phase::Refused;
			return false;
		}
		bool BeginProcessAttempt(Launch launch)
		{
			if (m_phase != Phase::Prepared || !CheckOwner() || !Call(m_publication_current) || !Bounded(launch))
			{
				return Refuse();
			}
			m_launch = std::move(launch);
			m_phase = Phase::Launching;
			return true;
		}
		bool ProcessStarted(const Identity& identity, const Launch& actual_launch)
		{
			if (m_phase != Phase::Launching || actual_launch != m_launch || !CheckOwner() || !Call(m_publication_current) ||
			    identity.runtime != m_owner || identity.process.empty() || identity.process == "0" || identity.epoch.expired())
			{
				return Refuse();
			}
			m_identity = identity;
			m_phase = Phase::Launched;
			return true;
		}
		bool InitializeWritten(const Identity& identity)
		{
			if (m_phase != Phase::Launched || !Check(identity) || identity.writer.expired() || identity.observer.expired())
			{
				return Refuse();
			}
			m_identity = identity;
			m_phase = Phase::Observing;
			return true;
		}
		bool NativeObserved(const Identity& identity, Current native_current)
		{
			if (m_phase != Phase::Observing || !Check(identity) || !Call(native_current))
			{
				return Refuse();
			}
			m_native_current = std::move(native_current);
			m_phase = Phase::Observed;
			return true;
		}
		bool BeginBinding(const Identity& identity)
		{
			return Advance(identity, Phase::Observed, Phase::BindingPending);
		}
		bool BindingPublished(const Identity& identity, Current own_publication)
		{
			return Published(identity, Phase::BindingPending, Phase::Bound, std::move(own_publication));
		}
		bool Handoff(const Identity& identity, const Current& handoff)
		{
			if (m_phase != Phase::Bound || !Check(identity) || !Call(handoff))
			{
				return Refuse();
			}
			m_phase = Phase::Ready;
			return Check(identity);
		}
		bool BeginPacket(const Identity& identity, std::string wire, std::string delivery, int turn, int request)
		{
			if (wire.empty() || wire.size() > 4 * 1024 * 1024 || delivery.empty() || turn < 0 || request < 0 ||
			    !Advance(identity, Phase::Ready, Phase::PacketPending))
			{
				return Refuse();
			}
			m_wire = std::move(wire);
			m_delivery = std::move(delivery);
			m_turn = turn;
			m_request = request;
			return true;
		}
		bool PacketPublished(const Identity& identity, Current own_publication)
		{
			return Published(identity, Phase::PacketPending, Phase::FinalReady, std::move(own_publication));
		}
		bool WriteOnce(const Identity& identity, std::string_view wire, std::string_view delivery, int turn, int request, const Current& write)
		{
			if (m_phase != Phase::FinalReady || !Check(identity) || wire != m_wire || delivery != m_delivery || turn != m_turn || request != m_request)
			{
				return Refuse();
			}
			// Even a throwing or failed enqueue can have written bytes. Never restore permission.
			m_attempted = true;
			m_phase = Phase::Attempted;
			return Call(write);
		}
		bool Check(const Identity& identity)
		{
			if (m_phase == Phase::Refused || m_phase == Phase::Attempted || !CheckOwner() ||
			    identity.runtime != m_owner || identity.process != m_identity.process || !Same(m_identity.epoch, identity.epoch) ||
			    (m_phase != Phase::Launched && (!Same(m_identity.writer, identity.writer) || !Same(m_identity.observer, identity.observer))) ||
			    (m_native_current && !Call(m_native_current)) ||
			    ((m_phase != Phase::BindingPending && m_phase != Phase::PacketPending) && !Call(m_publication_current)))
			{
				return Refuse();
			}
			return true;
		}

	  private:
		static bool Bounded(const Launch& launch)
		{
			if (launch.working_directory.empty() || launch.argv.empty() || launch.argv.front().empty() || launch.argv.size() > 4096 || launch.environment.size() > 1024)
			{
				return false;
			}
			std::size_t remaining = 8 * 1024 * 1024;
			const auto add = [&](const std::string& value)
			{
				if (value.find('\0') != std::string::npos || value.size() > remaining) return false;
				remaining -= value.size();
				return true;
			};
			if (!add(launch.working_directory)) return false;
			for (const auto& argument : launch.argv) if (!add(argument)) return false;
			for (const auto& [name, value] : launch.environment) if (name.empty() || !add(name) || !add(value)) return false;
			return true;
		}
		static bool Call(const Current& check)
		{
			try { return check && check(); }
			catch (...) { return false; }
		}
		static bool Same(const std::weak_ptr<const void>& a, const std::weak_ptr<const void>& b)
		{
			const auto original = a.lock();
			return original && original == b.lock();
		}
		bool CheckOwner()
		{
			return m_owner && Call(m_owner_current);
		}
		bool Advance(const Identity& identity, Phase from, Phase to)
		{
			if (m_phase != from || !Check(identity))
			{
				return Refuse();
			}
			m_phase = to;
			return true;
		}
		bool Published(const Identity& identity, Phase from, Phase to, Current own_publication)
		{
			if (m_phase != from || !Check(identity) || !Call(own_publication))
			{
				return Refuse();
			}
			m_publication_current = std::move(own_publication);
			m_phase = to;
			return Check(identity);
		}
		const void* const m_owner;
		Current m_owner_current, m_publication_current, m_native_current;
		Identity m_identity;
		Launch m_launch;
		Phase m_phase = Phase::Prepared;
		std::string m_wire, m_delivery;
		int m_turn = -1, m_request = -1;
		bool m_attempted = false;
	};
}
