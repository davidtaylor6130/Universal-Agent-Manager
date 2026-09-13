#pragma once

#include "include/cef_server.h"
#include "include/wrapper/cef_message_router.h"
#include "common/platform/platform_state_fields.h"
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <chrono>
#include <optional>

/// <summary>Opt-in loopback transport for the React companion; dispatch remains on the CEF UI thread.</summary>
class UamCompanionServer final : public CefServerHandler
{
public:
	using Callback = CefMessageRouterBrowserSide::Callback;
	using Dispatch = std::function<void(const nlohmann::json&, CefRefPtr<Callback>)>;
	static CefRefPtr<UamCompanionServer> StartFromEnvironment(Dispatch dispatch, const std::filesystem::path& data_root = {});
	void Stop();
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
	std::mutex m_mutex;
	CefRefPtr<CefServer> m_server;
	bool m_stopped = false;
	struct Transfer { std::string body; std::chrono::steady_clock::time_point touched; };
	std::unordered_map<std::string, Transfer> m_transfers;
	IMPLEMENT_REFCOUNTING(UamCompanionServer);
};
