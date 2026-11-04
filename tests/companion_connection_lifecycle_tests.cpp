#include "common/companion/connection_lifecycle.h"
#include "common/companion/connection_config.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
	using namespace uam::companion;
	using namespace std::chrono_literals;
	void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
	template <typename Predicate> void Await(Predicate predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + 3s;
		while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
		Require(predicate(), "Lifecycle did not reach the required state");
	}
	struct Fixture
	{
		std::atomic<int> started{0}, stopped{0}, live{0}, maximum_live{0};
		std::atomic<bool> listener_ready{true}, proxy_alive{true}, loopback_ready{true}, hold_stop{false}, stopping{false}, permit_stop{true};
		std::mutex mutex;
		std::vector<std::string> origins;
		struct Runtime final : ConnectionRuntime
		{
			Fixture& fixture;
			bool stopped = false;
			explicit Runtime(Fixture& value) : fixture(value)
			{
				++fixture.started;
				const int live = ++fixture.live;
				fixture.maximum_live = std::max(live, fixture.maximum_live.load());
			}
			ConnectionObservation Observe() override
			{
				return {fixture.loopback_ready, true, fixture.proxy_alive, fixture.listener_ready, fixture.proxy_alive ? "" : "proxyExited"};
			}
			bool Stop() override
			{
				fixture.stopping = true;
				while (fixture.hold_stop) std::this_thread::sleep_for(1ms);
				if (!fixture.permit_stop) return false;
				if (!stopped) { stopped = true; --fixture.live; ++fixture.stopped; }
				return true;
			}
		};
		std::unique_ptr<ConnectionRuntime> Create(const ConnectionDesired& desired)
		{
			std::lock_guard<std::mutex> lock(mutex);
			origins.push_back(desired.url);
			return std::make_unique<Runtime>(*this);
		}
	};
	struct StopFixture
	{
		ConnectionLifecycle& lifecycle;
		Fixture& fixture;
		~StopFixture()
		{
			fixture.hold_stop = false;
			fixture.permit_stop = true;
			lifecycle.Close();
			if (!lifecycle.WaitForStopped(3s)) std::terminate();
		}
	};

	void CompanionConnectionToggleActuallyStopsAndStartsOwnedRuntime()
	{
		Fixture fixture;
		ConnectionLifecycle lifecycle([&](const auto& desired) { return fixture.Create(desired); });
		StopFixture cleanup{lifecycle, fixture};
		lifecycle.Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "ready"; });
		lifecycle.Request(true, false, "https://192.168.1.20:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "disabled"; });
		Require(fixture.stopped == 1 && fixture.live == 0, "Disabled must confirm actual runtime teardown");
		lifecycle.Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "ready"; });
		Require(fixture.started == 2 && fixture.maximum_live == 1, "Re-enable starts exactly one replacement");
		lifecycle.Close(); Require(lifecycle.WaitForStopped(3s), "Closing worker must finish");
		Require(fixture.live == 0, "Owned runtime cannot survive lifecycle shutdown");
	}

	void CompanionConnectionReadinessRequiresBothListeners()
	{
		Fixture fixture;
		fixture.listener_ready = false;
		ConnectionLifecycle lifecycle([&](const auto& desired) { return fixture.Create(desired); });
		StopFixture cleanup{lifecycle, fixture};
		lifecycle.Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return fixture.started == 1; });
		std::this_thread::sleep_for(120ms);
		Require(lifecycle.Snapshot()["state"] == "starting" && lifecycle.Snapshot()["effectiveUrl"] == "", "Spawn success must not publish a usable address");
		fixture.loopback_ready = false;
		fixture.listener_ready = true;
		std::this_thread::sleep_for(120ms);
		Require(lifecycle.Snapshot()["state"] == "starting", "HTTPS listener alone cannot establish native API readiness");
		fixture.loopback_ready = true;
		Await([&]() { return lifecycle.Snapshot()["state"] == "ready"; });
		lifecycle.Close(); Require(lifecycle.WaitForStopped(3s), "Closing worker must finish");
	}

	void CompanionConnectionProxyFailureCannotMasqueradeAsReady()
	{
		Fixture fixture;
		fixture.proxy_alive = false;
		ConnectionLifecycle lifecycle([&](const auto& desired) { return fixture.Create(desired); });
		StopFixture cleanup{lifecycle, fixture};
		lifecycle.Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "failed"; });
		Require(lifecycle.Snapshot()["reasonCode"] == "proxyExited" && lifecycle.Snapshot()["effectiveUrl"] == "" && fixture.live == 0, "Failed proxy must stop access and retain a safe reason");
		fixture.proxy_alive = true;
		lifecycle.Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "ready"; });
		Require(fixture.started == 2, "Explicit restart must recover a failed process");
		lifecycle.Close(); Require(lifecycle.WaitForStopped(3s), "Closing worker must finish");
	}

	void CompanionConnectionLatestAddressOwnsOriginAfterSlowTeardown()
	{
		Fixture fixture;
		ConnectionLifecycle lifecycle([&](const auto& desired) { return fixture.Create(desired); });
		StopFixture cleanup{lifecycle, fixture};
		lifecycle.Request(true, true, "https://192.168.1.10:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "ready"; });
		fixture.hold_stop = true;
		lifecycle.Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return fixture.stopping.load(); });
		const auto started = std::chrono::steady_clock::now();
		lifecycle.Request(true, false, "https://192.168.1.20:58950/companion");
		lifecycle.Request(true, true, "https://192.168.1.30:58950/companion");
		Require(std::chrono::steady_clock::now() - started < 100ms, "Changing desired configuration must not wait for a process join");
		fixture.hold_stop = false;
		Await([&]() { return lifecycle.Snapshot()["state"] == "ready"; });
		Require(lifecycle.Snapshot()["effectiveUrl"] == "https://192.168.1.30:58950/companion" && lifecycle.Snapshot()["generation"] == 4, "Only the latest address/generation may publish ready");
		{
			std::lock_guard<std::mutex> lock(fixture.mutex);
			Require(fixture.origins.size() == 2 && fixture.origins.back() == "https://192.168.1.30:58950/companion", "Superseded configurations cannot start a stale API origin");
		}
		Require(fixture.maximum_live == 1, "Old listener must be destroyed before replacement starts");
		lifecycle.Close(); Require(lifecycle.WaitForStopped(3s), "Closing worker must finish");
	}

	void CompanionConnectionReadinessTimeoutStopsFailedRuntime()
	{
		Fixture fixture;
		fixture.listener_ready = false;
		ConnectionLifecycle lifecycle([&](const auto& desired) { return fixture.Create(desired); }, 150ms, 10ms);
		StopFixture cleanup{lifecycle, fixture};
		lifecycle.Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "failed"; });
		Require(lifecycle.Snapshot()["reasonCode"] == "readinessTimeout" && fixture.live == 0 && lifecycle.Snapshot()["effectiveUrl"] == "", "Readiness deadline stops the failed service and never publishes its route");
		lifecycle.Close(); Require(lifecycle.WaitForStopped(3s), "Closing worker must finish");
	}

	void CompanionConnectionUnconfirmedTeardownCannotBindReplacement()
	{
		Fixture fixture;
		ConnectionLifecycle lifecycle([&](const auto& desired) { return fixture.Create(desired); });
		StopFixture cleanup{lifecycle, fixture};
		lifecycle.Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "ready"; });
		fixture.permit_stop = false;
		lifecycle.Request(true, true, "https://192.168.1.30:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "failed"; });
		Require(lifecycle.Snapshot()["reasonCode"] == "stopTimeout" && fixture.started == 1, "A failed shutdown cannot permit a colliding listener");
		Require(lifecycle.Snapshot()["proxyRunning"] == true && lifecycle.Snapshot()["loopbackRunning"] == true, "Unconfirmed teardown must preserve observed live runtime flags rather than invent stopped listeners");
		fixture.proxy_alive = false;
		fixture.loopback_ready = false;
		Await([&]() { return lifecycle.Snapshot()["proxyRunning"] == false && lifecycle.Snapshot()["loopbackRunning"] == false; });
		Require(lifecycle.Snapshot()["reasonCode"] == "stopTimeout" && fixture.started == 1, "Failed cleanup must refresh observations without treating the old runtime as a ready replacement");
		fixture.proxy_alive = true;
		fixture.loopback_ready = true;
		fixture.permit_stop = true;
		lifecycle.Request(true, true, "https://192.168.1.30:58950/companion");
		Await([&]() { return lifecycle.Snapshot()["state"] == "ready"; });
		Require(fixture.maximum_live == 1 && fixture.started == 2, "Later confirmed shutdown permits exactly one replacement");
		lifecycle.Close(); Require(lifecycle.WaitForStopped(3s), "Closing worker must finish");
	}

	void CompanionConnectionShutdownFenceOutlivesOwnerAndReleasesFactory()
	{
		Fixture fixture;
		auto factory_resource = std::make_shared<int>(42);
		std::weak_ptr<int> weak_resource = factory_resource;
		auto lifecycle = std::make_unique<ConnectionLifecycle>([&, factory_resource](const auto& desired) { return fixture.Create(desired); });
		factory_resource.reset();
		lifecycle->Request(true, true, "https://192.168.1.20:58950/companion");
		Await([&]() { return lifecycle->Snapshot()["state"] == "ready"; });
		fixture.hold_stop = true;
		const auto fence = lifecycle->ShutdownCompletion();
		struct FenceCleanup
		{
			Fixture& fixture;
			ConnectionLifecycle::ShutdownControl fence;
			~FenceCleanup()
			{
				fixture.hold_stop = false;
				fixture.permit_stop = true;
				if (!fence(true, 3s)) std::terminate();
			}
		} cleanup{fixture, fence};
		lifecycle.reset();
		Require(!fence(false, 0ms) && !weak_resource.expired(), "Destroying UI ownership must not report quiescence while worker still owns runtime/factory objects");
		fixture.hold_stop = false;
		Require(fence(true, 3s), "A retained fence confirms shutdown after owner destruction");
		Require(weak_resource.expired() && fixture.live == 0, "Quiescence must release factory/CEF resources as well as the child runtime");
	}

	void CompanionConnectionMalformedConfigurationPreservesKnownIntent()
	{
		const auto malformed = ResolveConnectionConfiguration(std::string("{broken"), {});
		Require(!malformed.valid && !malformed.configured && !malformed.enabled, "Corrupt JSON cannot invent configured or enabled intent");
		const auto known_disabled = ResolveConnectionConfiguration(std::string(R"({"enabled":false,"token_file":"fixture-token","port":{}})"), {});
		Require(!known_disabled.valid && known_disabled.configured && !known_disabled.enabled, "Invalid fields must preserve independently known disabled intent");
		const auto known_enabled = ResolveConnectionConfiguration(std::string(R"({"enabled":true,"token_file":"fixture-token","port":70000})"), {});
		Require(!known_enabled.valid && known_enabled.enabled, "Known enabled intent can remain visible without treating invalid configuration as usable");
	}

	void CompanionConnectionConfigurationEnvironmentPrecedence()
	{
		const std::string saved = R"({"enabled":false,"token_file":"saved-token","port":"58949","origin":"https://192.168.1.10:58950","proxy_config":"proxy.json"})";
		const auto port = ResolveConnectionConfiguration(saved, {{}, std::string("63331"), {}});
		Require(port.valid && port.port == 63331 && !port.enabled && port.token_file == "saved-token", "Port overrides apply without a token-file override and preserve saved intent");
		const auto origin = ResolveConnectionConfiguration(saved, {{}, std::string("63331"), std::string("https://127.0.0.1:63332")});
		Require(origin.valid && origin.origin == "https://127.0.0.1:63332" && origin.Url() == "https://127.0.0.1:63332/companion", "The published desired route and runtime must use identical origin precedence");
		const auto loopback = ResolveConnectionConfiguration(std::string(R"({"enabled":false,"token_file":"saved-token","port":58949})"), {{}, std::string("63331"), {}});
		Require(loopback.Url() == "http://127.0.0.1:63331/companion", "A port override must reach the loopback desired URL too");
		const auto environment = ResolveConnectionConfiguration(std::string("{broken"), {std::string("environment-token"), std::string("63331"), {}});
		Require(environment.valid && environment.configured && environment.enabled && environment.token_file == "environment-token" && environment.proxy_config.empty(), "Explicit environment configuration remains independent of corrupt persisted configuration");
	}

	void CompanionConnectionInvalidConfigurationFailsWithoutStartingRuntime()
	{
		Fixture fixture;
		ConnectionLifecycle lifecycle([&](const auto& desired) { return fixture.Create(desired); });
		StopFixture cleanup{lifecycle, fixture};
		lifecycle.Request(false, false, "", "configurationInvalid");
		Await([&]() { return lifecycle.Snapshot()["state"] == "failed"; });
		Require(lifecycle.Snapshot()["configured"] == false && lifecycle.Snapshot()["enabled"] == false && fixture.started == 0, "Invalid unknown configuration must fail honestly without attempting a service start");
	}
}

int main(int argc, char** argv)
{
	const std::vector<std::pair<std::string, void (*)()>> tests = {
	    {"CompanionConnectionToggleActuallyStopsAndStartsOwnedRuntime", CompanionConnectionToggleActuallyStopsAndStartsOwnedRuntime},
	    {"CompanionConnectionReadinessRequiresBothListeners", CompanionConnectionReadinessRequiresBothListeners},
	    {"CompanionConnectionProxyFailureCannotMasqueradeAsReady", CompanionConnectionProxyFailureCannotMasqueradeAsReady},
	    {"CompanionConnectionLatestAddressOwnsOriginAfterSlowTeardown", CompanionConnectionLatestAddressOwnsOriginAfterSlowTeardown},
	    {"CompanionConnectionReadinessTimeoutStopsFailedRuntime", CompanionConnectionReadinessTimeoutStopsFailedRuntime},
	    {"CompanionConnectionUnconfirmedTeardownCannotBindReplacement", CompanionConnectionUnconfirmedTeardownCannotBindReplacement},
	    {"CompanionConnectionShutdownFenceOutlivesOwnerAndReleasesFactory", CompanionConnectionShutdownFenceOutlivesOwnerAndReleasesFactory},
	    {"CompanionConnectionMalformedConfigurationPreservesKnownIntent", CompanionConnectionMalformedConfigurationPreservesKnownIntent},
	    {"CompanionConnectionConfigurationEnvironmentPrecedence", CompanionConnectionConfigurationEnvironmentPrecedence},
	    {"CompanionConnectionInvalidConfigurationFailsWithoutStartingRuntime", CompanionConnectionInvalidConfigurationFailsWithoutStartingRuntime}};
	for (const auto& [name, test] : tests)
	{
		if (argc > 1 && name != argv[1]) continue;
		try { test(); std::cout << "PASS " << name << '\n'; }
		catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
	}
}
