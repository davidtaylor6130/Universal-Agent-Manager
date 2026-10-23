#pragma once

#include "include/cef_server.h"
#include "include/wrapper/cef_message_router.h"
#include "common/platform/platform_state_fields.h"
#include "common/companion/protocol.h"
#include "common/companion/connection_lifecycle.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <atomic>

/// <summary>Opt-in loopback transport for the React companion; dispatch remains on the CEF UI thread.</summary>
class UamCompanionServer final : public CefServerHandler
{
public:
	using Callback = CefMessageRouterBrowserSide::Callback;
	using Dispatch = std::function<void(const nlohmann::json&, CefRefPtr<Callback>)>;
	static CefRefPtr<UamCompanionServer> StartFromEnvironment(Dispatch dispatch, const std::filesystem::path& data_root = {});
	void Stop();
	bool StopAndWait(std::chrono::milliseconds timeout);
	uam::companion::ConnectionObservation ObserveConnection();
	bool IsProxyRunning() const { return m_proxy_running.load(); }
	void OnServerCreated(CefRefPtr<CefServer> server) override;
	void OnServerDestroyed(CefRefPtr<CefServer> server) override;
	void OnClientConnected(CefRefPtr<CefServer>, int) override {}
	void OnClientDisconnected(CefRefPtr<CefServer>, int) override {}
	void OnHttpRequest(CefRefPtr<CefServer> server, int connection_id, const CefString&, CefRefPtr<CefRequest> request) override;
	void OnWebSocketRequest(CefRefPtr<CefServer>, int, const CefString&, CefRefPtr<CefRequest>, CefRefPtr<CefCallback> callback) override { callback->Cancel(); }
	void OnWebSocketConnected(CefRefPtr<CefServer>, int) override {}
	void OnWebSocketMessage(CefRefPtr<CefServer>, int, const void*, size_t) override {}
private:
	UamCompanionServer(std::string token, std::string origin, Dispatch dispatch, std::filesystem::path proxy_executable = {}, std::filesystem::path proxy_config = {})
		: m_token(std::move(token)), m_origin(std::move(origin)), m_dispatch(std::move(dispatch)), m_proxy_executable(std::move(proxy_executable)), m_proxy_config(std::move(proxy_config)) {}
	void StartProxy();
	void DrainProxy(std::stop_token stop_token);
	void ProcessApiRequest(std::string body, CefRefPtr<Callback> reply, bool transferred = false);
	void DispatchCompanion(nlohmann::json envelope, CefRefPtr<Callback> reply);
	void ScheduleTransferExpiry(CefRefPtr<CefServer> server);
	std::optional<std::string> StoreTransfer(std::string body);
	std::optional<nlohmann::json> ReadTransferChunk(const std::string& id, std::size_t offset);
	std::string m_token;
	std::string m_origin;
	Dispatch m_dispatch;
	std::filesystem::path m_proxy_executable;
	std::filesystem::path m_proxy_config;
	uam::platform::StdioProcessPlatformFields m_proxy_process;
	std::jthread m_proxy_drainer;
	bool m_proxy_started = false;
	std::atomic<bool> m_proxy_start_failed{false};
	std::atomic<bool> m_proxy_exited{false};
	std::atomic<bool> m_proxy_running{false};
	std::atomic<bool> m_loopback_running{false};
	std::mutex m_mutex;
	std::condition_variable m_destroyed_condition;
	bool m_server_created = false;
	bool m_server_destroyed = false;
	CefRefPtr<CefServer> m_server;
	bool m_stopped = false;
	uam::companion::ResponseTransfers m_response_transfers{[] { return NewTransferId(); }};
	uam::companion::RequestAdmission m_request_admission;
	uam::companion::RequestTransfers m_request_transfers{[] { return NewTransferId(); }};
	uam::companion::StagedAttachments m_staged_attachments;
	static std::string NewTransferId();
	IMPLEMENT_REFCOUNTING(UamCompanionServer);
};

/// Owns asynchronous, serialized publication changes independently of desktop/provider state.
class UamCompanionService
{
public:
	static std::shared_ptr<UamCompanionService> Create(const std::filesystem::path& data_root, UamCompanionServer::Dispatch dispatch);
	static void BeginShutdownAll();
	static bool AllStopped();
	static nlohmann::json ConfigurationStatus(const std::filesystem::path& data_root);
	void Reload(std::optional<bool> enabled_override = std::nullopt);
	void Apply(bool configured, bool enabled, std::string desired_url) { m_lifecycle.Request(configured, enabled, std::move(desired_url)); }
	void Stop() { m_lifecycle.Close(); }
	bool IsProxyRunning() const { return m_lifecycle.Snapshot().value("proxyRunning", false); }
	nlohmann::json Status() const;
private:
	explicit UamCompanionService(std::filesystem::path data_root, uam::companion::ConnectionLifecycle::Factory factory) : m_data_root(std::move(data_root)), m_lifecycle(std::move(factory)) {}
	std::filesystem::path m_data_root;
	uam::companion::ConnectionLifecycle m_lifecycle;
};
