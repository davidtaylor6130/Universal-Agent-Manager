#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace uam::companion
{
	struct ConnectionDesired
	{
		bool configured = false;
		bool enabled = false;
		std::string url;
		std::uint64_t generation = 0;
		std::string failure;
	};

	struct ConnectionObservation
	{
		bool loopback_running = false;
		bool proxy_required = false;
		bool proxy_running = false;
		bool listener_ready = false;
		std::string failure;
	};

	/// All runtime methods execute on one owned worker, never the UI thread.
	class ConnectionRuntime
	{
	public:
		virtual ~ConnectionRuntime() = default;
		virtual ConnectionObservation Observe() = 0;
		// False forbids starting a replacement until a later Stop confirms teardown.
		virtual bool Stop() = 0;
	};

	inline std::string ConnectionReason(const std::string& code)
	{
		if (code == "disabled") return "Phone access is turned off.";
		if (code == "starting") return "Universal Agent Manager is starting phone access.";
		if (code == "stopping") return "Universal Agent Manager is turning off phone access.";
		if (code == "ready") return "Phone access is ready on this computer. The iPhone must use the same network and trust its HTTPS certificate.";
		if (code == "addressUnavailable") return "The saved network address is no longer on this computer. Choose a current LAN address and repair phone access.";
		if (code == "proxyStartFailed") return "Universal Agent Manager could not start its HTTPS service. Restart phone access and check the connection settings.";
		if (code == "proxyExited") return "The HTTPS service stopped. Restart phone access to reconnect.";
		if (code == "loopbackFailed") return "Universal Agent Manager could not open its local phone connection. Another instance may be using the port.";
		if (code == "readinessTimeout") return "The phone connection did not become ready. Check the LAN address and ports, then restart phone access.";
		if (code == "stopTimeout") return "The previous phone connection is still closing. Try restarting phone access again.";
		return "Phone access configuration is invalid or unavailable. Repair the connection settings in Universal Agent Manager.";
	}

	inline nlohmann::json ConnectionStatus(const ConnectionDesired& desired, const std::string& status, const std::string& reason,
	                                      const std::string& effective_url = {}, const ConnectionObservation& observed = {})
	{
		return {{"configured", desired.configured}, {"enabled", desired.enabled}, {"url", desired.url}, {"desiredUrl", desired.url},
		    {"effectiveUrl", effective_url}, {"state", status}, {"reasonCode", reason}, {"reason", ConnectionReason(reason)},
		    {"proxyRunning", observed.proxy_running}, {"loopbackRunning", observed.loopback_running},
		    {"localAddresses", nlohmann::json::array()}, {"restartRequired", false}, {"generation", desired.generation}};
	}

	/// A single serial worker owns teardown/startup; requests supersede by generation.
	/// Destruction requests shutdown without joining a process or server on the UI thread.
	class ConnectionLifecycle
	{
	public:
		using Factory = std::function<std::unique_ptr<ConnectionRuntime>(const ConnectionDesired&)>;
		using ShutdownControl = std::function<bool(bool, std::chrono::milliseconds)>;
		explicit ConnectionLifecycle(Factory factory, std::chrono::milliseconds startup_timeout = std::chrono::seconds(10), std::chrono::milliseconds poll_interval = std::chrono::milliseconds(100))
		    : m_state(std::make_shared<Shared>())
		{
			const auto shared = m_state;
			std::thread([shared, factory = std::move(factory), startup_timeout, poll_interval]() mutable
			{
				Run(shared, factory, startup_timeout, poll_interval);
				// Factory captures may own CEF objects. Release them before confirming quiescence.
				factory = {};
				std::lock_guard<std::mutex> lock(shared->mutex);
				shared->worker_stopped = true;
				shared->changed.notify_all();
			}).detach();
		}
		~ConnectionLifecycle() { Close(); }
		void Request(bool configured, bool enabled, std::string url, std::string failure = {})
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			if (m_state->closed) return;
			m_state->desired = {configured, enabled, std::move(url), m_state->desired.generation + 1, std::move(failure)};
			m_state->status = "starting";
			m_state->reason = enabled ? "starting" : "stopping";
			m_state->effective_url.clear();
			m_state->changed.notify_all();
		}
		nlohmann::json Snapshot() const
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			const auto& desired = m_state->desired;
			return ConnectionStatus(desired, m_state->status, m_state->reason, m_state->effective_url, m_state->observed);
		}
		void Close()
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			m_state->closed = true;
			m_state->changed.notify_all();
		}
		bool WaitForStopped(std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(m_state->mutex);
			return m_state->changed.wait_for(lock, timeout, [&]() { return m_state->worker_stopped; });
		}
		ShutdownControl ShutdownCompletion() const
		{
			return [state = m_state](bool stop, std::chrono::milliseconds timeout)
			{
				std::unique_lock<std::mutex> lock(state->mutex);
				if (stop) { state->closed = true; state->changed.notify_all(); }
				return state->changed.wait_for(lock, timeout, [&]() { return state->worker_stopped; });
			};
		}
	private:
		struct Shared
		{
			std::mutex mutex;
			std::condition_variable changed;
			ConnectionDesired desired;
			ConnectionObservation observed;
			std::string status = "disabled", reason = "disabled", effective_url;
			bool closed = false, worker_stopped = false;
		};
		std::shared_ptr<Shared> m_state;
		static void Publish(const std::shared_ptr<Shared>& state, const ConnectionDesired& desired, const std::string& status, const std::string& reason, ConnectionObservation observation = {})
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->closed || state->desired.generation != desired.generation) return;
			state->status = status;
			state->reason = reason;
			state->observed = std::move(observation);
			state->effective_url = status == "ready" ? desired.url : std::string{};
			state->changed.notify_all();
		}
		static void Run(const std::shared_ptr<Shared>& state, const Factory& factory, std::chrono::milliseconds startup_timeout, std::chrono::milliseconds poll_interval)
		{
			std::unique_ptr<ConnectionRuntime> runtime;
			std::uint64_t handled = 0;
			bool observing = false;
			bool cleanup_pending = false;
			const auto remaining_observation = [&]()
			{
				ConnectionObservation result;
				{ std::lock_guard<std::mutex> lock(state->mutex); result = state->observed; }
				try { if (runtime) result = runtime->Observe(); } catch (...) {}
				return result;
			};
			auto deadline = std::chrono::steady_clock::now();
			for (;;)
			{
				ConnectionDesired desired;
				{
					std::unique_lock<std::mutex> lock(state->mutex);
					state->changed.wait_for(lock, observing ? poll_interval : std::chrono::hours(24), [&]() { return state->closed || state->desired.generation != handled; });
					if (state->closed) break;
					desired = state->desired;
				}
				if (desired.generation != handled)
				{
					observing = false;
					cleanup_pending = false;
					if (runtime && !runtime->Stop())
					{
						handled = desired.generation;
						cleanup_pending = observing = true;
						Publish(state, desired, "failed", "stopTimeout", remaining_observation());
						continue;
					}
					runtime.reset();
					{
						std::lock_guard<std::mutex> lock(state->mutex);
						if (state->closed) break;
						if (state->desired.generation != desired.generation) continue;
					}
					handled = desired.generation;
					if (!desired.failure.empty()) { Publish(state, desired, "failed", desired.failure); continue; }
					if (!desired.enabled) { Publish(state, desired, "disabled", "disabled"); continue; }
					try { runtime = factory(desired); }
					catch (...) { runtime.reset(); }
					if (!runtime) { Publish(state, desired, "failed", "configurationInvalid"); continue; }
					deadline = std::chrono::steady_clock::now() + startup_timeout;
					observing = true;
				}
				if (!observing) continue;
				if (cleanup_pending)
				{
					Publish(state, desired, "failed", "stopTimeout", remaining_observation());
					continue;
				}
				ConnectionObservation observation;
				try { observation = runtime->Observe(); }
				catch (...) { observation.failure = "configurationInvalid"; }
				const bool ready = observation.loopback_running && (!observation.proxy_required || (observation.proxy_running && observation.listener_ready));
				if (observation.failure.empty() && !ready && std::chrono::steady_clock::now() >= deadline) observation.failure = "readinessTimeout";
				if (!observation.failure.empty())
				{
					observing = false;
					const std::string failure = observation.failure;
					if (runtime->Stop())
					{
						runtime.reset();
						Publish(state, desired, "failed", failure);
					}
					else
					{
						cleanup_pending = observing = true;
						Publish(state, desired, "failed", "stopTimeout", remaining_observation());
					}
				}
				else if (ready)
				{
					deadline = std::chrono::steady_clock::now() + startup_timeout;
					Publish(state, desired, "ready", "ready", observation);
				}
				else Publish(state, desired, "starting", "starting", observation);
			}
			// Keep the shutdown fence closed if a server has not confirmed destruction.
			// The application uses its own bounded quiescence deadline before CefShutdown.
			while (runtime && !runtime->Stop()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
			runtime.reset();
		}
	};
}
