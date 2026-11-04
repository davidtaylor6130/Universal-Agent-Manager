#include "cef/companion_server.h"
#include "common/companion/connection_config.h"

#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif
#include "cef/uam_bridge_request.h"
#include "common/utils/env_utils.h"
#include "common/platform/platform_services.h"
#include "common/platform/companion_network_platform.h"
#include <filesystem>
#include <map>
#include "include/cef_parser.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <limits>

namespace
{
	constexpr std::size_t kGzipThreshold = 64 * 1024;
	constexpr std::size_t kMaxWireBody = 900 * 1024;
	constexpr std::size_t kCompanionTransferChunkBytes = uam::companion::kResponseChunkBytes;

	enum class BodyEncodingResult { Plain, Gzip, TooLarge, Failed };

	BodyEncodingResult EncodeBody(const std::string& body, bool accepts_gzip, std::string* wire_body)
	{
		if (!accepts_gzip || body.size() < kGzipThreshold)
		{
			if (body.size() > kMaxWireBody) return BodyEncodingResult::TooLarge;
			*wire_body = body;
			return BodyEncodingResult::Plain;
		}
		z_stream stream{};
		if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
			return BodyEncodingResult::Failed;
		std::string compressed(kMaxWireBody + 1, '\0');
		stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(body.data()));
		stream.avail_in = static_cast<uInt>(body.size());
		stream.next_out = reinterpret_cast<Bytef*>(compressed.data());
		stream.avail_out = static_cast<uInt>(compressed.size());
		const int result = deflate(&stream, Z_FINISH);
		const uLong compressed_size = stream.total_out;
		deflateEnd(&stream);
		if (result != Z_STREAM_END) return stream.avail_out == 0 ? BodyEncodingResult::TooLarge : BodyEncodingResult::Failed;
		compressed.resize(compressed_size);
		if (compressed.size() > kMaxWireBody) return BodyEncodingResult::TooLarge;
		*wire_body = std::move(compressed);
		return BodyEncodingResult::Gzip;
	}

	bool AcceptsGzip(const CefString& value)
	{
		std::string header = value.ToString();
		std::transform(header.begin(), header.end(), header.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		for (std::size_t start = 0; start < header.size(); )
		{
			const std::size_t end = header.find(',', start);
			const std::string token = header.substr(start, end == std::string::npos ? end : end - start);
			const std::size_t separator = token.find(';');
			std::string name = token.substr(0, separator);
			const std::size_t first = name.find_first_not_of(" \t");
			const std::size_t last = name.find_last_not_of(" \t");
			if (first != std::string::npos) name = name.substr(first, last - first + 1);
			const std::size_t quality = token.find("q=");
			const bool disabled = quality != std::string::npos && std::strtod(token.c_str() + quality + 2, nullptr) <= 0.0;
			if (name == "gzip" && !disabled)
				return true;
			if (end == std::string::npos) break;
			start = end + 1;
		}
		return false;
	}

	class Task final : public CefTask
	{
	public:
		explicit Task(std::function<void()> run) : m_run(std::move(run)) {}
		void Execute() override { m_run(); }
	private:
		std::function<void()> m_run;
		IMPLEMENT_REFCOUNTING(Task);
	};

	class JsonReply final : public CefMessageRouterBrowserSide::Callback
	{
	  public:
		using SuccessHandler = std::function<void(nlohmann::json)>;
		using FailureHandler = std::function<void(int)>;
		JsonReply(CefRefPtr<Callback> target, SuccessHandler success, FailureHandler failure = {}) : m_target(target), m_success(std::move(success)), m_failure(std::move(failure))
		{
		}
		void Success(const CefString& response) override
		{
			try
			{
				m_success(nlohmann::json::parse(response.ToString()));
			}
			catch (const std::exception&)
			{
				m_target->Failure(500, "Invalid companion service response.");
			}
		}
		void Success(const void*, size_t) override
		{
			m_target->Failure(500, "Binary replies are unavailable to the companion.");
		}
		void Failure(int status, const CefString& error) override
		{
			if (m_failure) m_failure(status);
			else m_target->Failure(status, error);
		}

	  private:
		CefRefPtr<Callback> m_target;
		SuccessHandler m_success;
		FailureHandler m_failure;
		IMPLEMENT_REFCOUNTING(JsonReply);
	};

	class AdmittedReply final : public CefMessageRouterBrowserSide::Callback
	{
	  public:
		AdmittedReply(CefRefPtr<Callback> target, std::shared_ptr<uam::companion::RequestAdmission::Lease> lease) : m_target(target), m_lease(std::move(lease)) {}
		void Success(const CefString& response) override
		{
			const auto lease = TakeLease();
			m_target->Success(response);
		}
		void Success(const void* data, size_t size) override
		{
			const auto lease = TakeLease();
			m_target->Success(data, size);
		}
		void Failure(int status, const CefString& error) override
		{
			const auto lease = TakeLease();
			m_target->Failure(status, error);
		}
	  private:
		std::shared_ptr<uam::companion::RequestAdmission::Lease> TakeLease()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return std::exchange(m_lease, {});
		}
		CefRefPtr<Callback> m_target;
		std::mutex m_mutex;
		std::shared_ptr<uam::companion::RequestAdmission::Lease> m_lease;
		IMPLEMENT_REFCOUNTING(AdmittedReply);
	};

	// Replies run on the server thread so disconnected connection IDs cannot be reused between check and send.
	class Reply final : public CefMessageRouterBrowserSide::Callback
	{
	public:
		Reply(CefRefPtr<CefServer> server, int id, bool accepts_gzip, std::function<std::optional<std::string>(std::string)> store_transfer) : m_server(server), m_id(id), m_store_transfer(std::move(store_transfer)), m_accepts_gzip(accepts_gzip)
		{
		}
		void Success(const CefString& response) override { Send(200, response.ToString()); }
		void Success(const void*, size_t) override { Failure(500, "Binary replies are unavailable to the companion."); }
		void Failure(int status, const CefString& error) override
		{
			Send(status >= 400 && status <= 599 ? status : 500, nlohmann::json{{"error", error.ToString()}}.dump());
		}
	private:
		void Send(int status, std::string body)
		{
			if (body.size() > uam::companion::kMaxResponseBytes)
			{
				status = 413;
				body = R"({"error":"Response exceeds 32 MiB. Request a smaller page."})";
			}
			std::string wire_body;
			const BodyEncodingResult encoding = EncodeBody(body, m_accepts_gzip, &wire_body);
			bool gzip = encoding == BodyEncodingResult::Gzip;
			if (encoding == BodyEncodingResult::TooLarge)
			{
				// Store now under its aggregate bound; never queue the complete raw body on the server runner.
				const std::optional<std::string> descriptor = m_store_transfer ? m_store_transfer(std::move(body)) : std::nullopt;
				if (descriptor) wire_body = *descriptor;
				else { status = 503; wire_body = R"({"error":"Response transfer capacity exhausted."})"; }
				gzip = false;
			}
			else if (encoding == BodyEncodingResult::Failed)
			{
				status = 500;
				wire_body = R"({"error":"Response compression failed."})";
				gzip = false;
			}
			CefRefPtr<CefServer> server = m_server;
			const int id = m_id;
			server->GetTaskRunner()->PostTask(new Task([server, id, status, gzip, body = std::move(wire_body)]()
			{
				if (!server->IsValidConnection(id)) return;
				CefServer::HeaderMap headers = {{"Cache-Control", "no-store"}, {"Connection", "close"}};
				if (gzip) { headers.emplace("Content-Encoding", "gzip"); headers.emplace("Vary", "Accept-Encoding"); }
				server->SendHttpResponse(id, status, "application/json", static_cast<int64_t>(body.size()), headers);
				server->SendRawData(id, body.data(), body.size());
			}));
		}

		CefRefPtr<CefServer> m_server;
		int m_id;
		std::function<std::optional<std::string>(std::string)> m_store_transfer;
		bool m_accepts_gzip;
		IMPLEMENT_REFCOUNTING(Reply);
	};
}

namespace
{
uam::companion::ConnectionConfiguration ReadConnectionConfiguration(const std::filesystem::path& data_root)
{
	const uam::companion::ConnectionEnvironment environment{
	    uam::env::GetNonEmptyString("UAM_COMPANION_TOKEN_FILE"),
	    uam::env::GetNonEmptyString("UAM_COMPANION_PORT"),
	    uam::env::GetNonEmptyString("UAM_COMPANION_ORIGIN")};
	std::optional<std::string> persisted;
	if (!environment.token_file && !data_root.empty())
	{
		const auto path = data_root / "companion.json";
		std::error_code error;
		const bool exists = std::filesystem::exists(path, error);
		if (exists || error)
		{
			persisted = ""; // A present but unreadable/oversized file is invalid, not absent.
			const auto size = std::filesystem::file_size(path, error);
			if (!error && size <= 64 * 1024)
			{
				std::ifstream input(path, std::ios::binary);
				if (input) persisted = std::string(std::istreambuf_iterator<char>(input), {});
			}
		}
	}
	return uam::companion::ResolveConnectionConfiguration(persisted, environment);
}
}

CefRefPtr<UamCompanionServer> UamCompanionServer::StartFromEnvironment(Dispatch dispatch, const std::filesystem::path& data_root)
{
	const auto config = ReadConnectionConfiguration(data_root);
	if (!config.valid || !config.configured || !config.enabled) return nullptr;
	std::ifstream input(config.token_file);
	std::string token;
	std::getline(input, token);
	if (token.size() != 64 || token.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
	{
		std::cerr << "Companion disabled: invalid token file.\n";
		return nullptr;
	}
	const int port = config.port;
	const std::string& origin = config.origin;
	CefURLParts parts;
	if (!origin.empty() && (!CefParseURL(origin, parts) || CefString(&parts.scheme) != "https" ||
	    CefString(&parts.host).empty() || !CefString(&parts.username).empty() ||
	    !CefString(&parts.password).empty() || !CefString(&parts.query).empty() ||
	    !CefString(&parts.fragment).empty() ||
	    origin != "https://" + CefString(&parts.host).ToString() +
	        (CefString(&parts.port).empty() ? "" : ":" + CefString(&parts.port).ToString())))
	{
		std::cerr << "Companion disabled: origin must be HTTPS without a path.\n";
		return nullptr;
	}
	std::filesystem::path proxy_executable = config.proxy_executable;
	const std::filesystem::path proxy_config = config.proxy_config;
	if (!proxy_config.empty() && proxy_executable.empty())
	{
		const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
#if defined(__APPLE__)
		proxy_executable = executable.parent_path().parent_path() / "Resources/companion/caddy";
#else
		proxy_executable = executable.parent_path() / "companion/caddy";
#endif
	}
#if defined(__APPLE__)
	if (!origin.empty()) uam::platform::RequestMacLocalNetworkAccess();
#endif
	CefRefPtr<UamCompanionServer> handler = new UamCompanionServer(std::move(token), origin, std::move(dispatch), std::move(proxy_executable), std::move(proxy_config));
	CefServer::CreateServer("127.0.0.1", static_cast<uint16_t>(port), 8, handler);
	return handler;
}

void UamCompanionServer::Stop()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_stopped = true;
	if (m_server) m_server->Shutdown();
	if (m_proxy_drainer.joinable()) m_proxy_drainer.request_stop();
}

bool UamCompanionServer::StopAndWait(std::chrono::milliseconds timeout)
{
	// Only the service's serial worker calls this; UI callers request Stop without joining.
	Stop();
	if (m_proxy_drainer.joinable()) m_proxy_drainer.join();
	if (m_proxy_started)
	{
		PlatformServicesFactory::Instance().process_service.StopStdioProcess(m_proxy_process, true);
		m_proxy_started = false;
	}
	m_proxy_running = false;
	std::unique_lock<std::mutex> lock(m_mutex);
	return m_destroyed_condition.wait_for(lock, timeout, [&]() { return m_server_destroyed; });
}

uam::companion::ConnectionObservation UamCompanionServer::ObserveConnection()
{
	uam::companion::ConnectionObservation result;
	result.loopback_running = m_loopback_running.load();
	result.proxy_required = !m_proxy_config.empty();
	result.proxy_running = m_proxy_running.load();
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_server_created && !result.loopback_running) result.failure = "loopbackFailed";
	}
	if (m_proxy_start_failed) result.failure = "proxyStartFailed";
	else if (m_proxy_exited) result.failure = "proxyExited";
	if (!result.failure.empty() || !result.loopback_running) return result;
	CefURLParts parts;
	if (!m_origin.empty() && CefParseURL(m_origin, parts))
	{
		const std::string host = CefString(&parts.host).ToString();
#if defined(__APPLE__)
		if (host.find_first_not_of("0123456789.") == std::string::npos && host != "127.0.0.1")
		{
			const auto addresses = uam::platform::MacLocalIpv4Addresses();
			if (std::find(addresses.begin(), addresses.end(), host) == addresses.end())
			{ result.failure = "addressUnavailable"; return result; }
		}
#endif
		if (result.proxy_required && result.proxy_running)
		{
			const std::string port_text = CefString(&parts.port).ToString();
			int port = 443;
			if (!port_text.empty())
			{
				const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
				if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size())
				{ result.failure = "configurationInvalid"; return result; }
			}
			const std::string address = host.find_first_not_of("0123456789.") == std::string::npos ? host : "127.0.0.1";
			result.listener_ready = uam::platform::ProbeCompanionTcpListener(address, port, std::chrono::milliseconds(150));
		}
	}
	return result;
}

void UamCompanionServer::StartProxy()
{
	if (m_proxy_config.empty() || m_proxy_executable.empty()) return;
	std::string error;
	const std::vector<std::string> argv = {m_proxy_executable.string(), "run", "--config", m_proxy_config.string()};
	if (!PlatformServicesFactory::Instance().process_service.StartStdioProcess(m_proxy_process, m_proxy_config.parent_path(), argv, &error))
	{
		m_proxy_start_failed = true;
		std::cerr << "Companion HTTPS proxy failed to start: " << error << "\n";
		return;
	}
	m_proxy_started = true;
	m_proxy_running = true;
	std::cerr << "Companion HTTPS proxy started.\n";
	m_proxy_drainer = std::jthread([this](std::stop_token stop_token) { DrainProxy(stop_token); });
}

void UamCompanionServer::DrainProxy(std::stop_token stop_token)
{
	char buffer[4096];
	std::string stderr_tail;
	while (!stop_token.stop_requested())
	{
		// Observe exit before reading so the proxy's final diagnostic reaches the log.
		int exit_code = 0;
		const bool exited = PlatformServicesFactory::Instance().process_service.PollStdioProcessExited(m_proxy_process, &exit_code);
		bool read_any = false;
		for (const bool stderr_stream : {false, true})
		{
			const std::ptrdiff_t count = stderr_stream
				? PlatformServicesFactory::Instance().process_service.ReadStdioProcessStderr(m_proxy_process, buffer, sizeof(buffer))
				: PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(m_proxy_process, buffer, sizeof(buffer));
			if (count > 0)
			{
				read_any = true;
				if (stderr_stream)
				{
					stderr_tail.append(buffer, static_cast<std::size_t>(count));
					if (stderr_tail.size() > 2048) stderr_tail.erase(0, stderr_tail.size() - 2048);
				}
			}
		}
		if (exited && !read_any)
		{
			m_proxy_running = false;
			m_proxy_exited = true;
			std::cerr << "Companion HTTPS proxy exited with code " << exit_code;
			if (!stderr_tail.empty()) std::cerr << ": " << stderr_tail;
			std::cerr << '\n';
			return;
		}
		if (!read_any) std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

void UamCompanionServer::OnServerCreated(CefRefPtr<CefServer> server)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_server = server;
	m_server_created = true;
	m_loopback_running = server->IsRunning();
	if (m_stopped) server->Shutdown();
	else if (server->IsRunning())
	{
		std::cerr << "Companion listening on loopback.\n";
		StartProxy();
		ScheduleTransferExpiry(server);
	}
	else std::cerr << "Companion failed to bind its port.\n";
}

void UamCompanionServer::OnServerDestroyed(CefRefPtr<CefServer>)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_server = nullptr;
	m_loopback_running = false;
	m_server_destroyed = true;
	m_destroyed_condition.notify_all();
	m_request_transfers.Clear();
	m_response_transfers.Clear();
}

namespace
{
	std::mutex g_companion_services_mutex;
	std::vector<uam::companion::ConnectionLifecycle::ShutdownControl> g_companion_services;
	bool g_companion_shutdown = false;

	class CefConnectionRuntime final : public uam::companion::ConnectionRuntime
	{
	public:
		CefConnectionRuntime(const std::filesystem::path& data_root, const UamCompanionServer::Dispatch& dispatch, const uam::companion::ConnectionDesired& desired)
		{
			CefURLParts parts;
			if (desired.url.empty() || !CefParseURL(desired.url, parts)) { m_failure = "configurationInvalid"; return; }
#if defined(__APPLE__)
			const std::string host = CefString(&parts.host).ToString();
			if (host.find_first_not_of("0123456789.") == std::string::npos && host != "127.0.0.1")
			{
				const auto addresses = uam::platform::MacLocalIpv4Addresses();
				if (std::find(addresses.begin(), addresses.end(), host) == addresses.end())
				{ m_failure = "addressUnavailable"; return; }
			}
#endif
			m_server = UamCompanionServer::StartFromEnvironment(dispatch, data_root);
			if (!m_server) m_failure = "configurationInvalid";
		}
		uam::companion::ConnectionObservation Observe() override
		{
			if (!m_failure.empty()) return {false, false, false, false, m_failure};
			return m_server->ObserveConnection();
		}
		bool Stop() override { return !m_server || m_server->StopAndWait(std::chrono::seconds(5)); }
	private:
		CefRefPtr<UamCompanionServer> m_server;
		std::string m_failure;
	};
}

std::shared_ptr<UamCompanionService> UamCompanionService::Create(const std::filesystem::path& data_root, UamCompanionServer::Dispatch dispatch)
{
	auto service = std::shared_ptr<UamCompanionService>(new UamCompanionService(data_root,
	    [data_root, dispatch = std::move(dispatch)](const uam::companion::ConnectionDesired& desired)
	    { return std::make_unique<CefConnectionRuntime>(data_root, dispatch, desired); }));
	{
		std::lock_guard<std::mutex> lock(g_companion_services_mutex);
		std::erase_if(g_companion_services, [](const auto& control) { return control(false, std::chrono::milliseconds(0)); });
		g_companion_services.push_back(service->m_lifecycle.ShutdownCompletion());
		if (g_companion_shutdown) service->Stop();
	}
	service->Reload();
	return service;
}

void UamCompanionService::Reload(std::optional<bool> enabled_override)
{
	auto config = ReadConnectionConfiguration(m_data_root);
	if (enabled_override && config.valid) config.enabled = *enabled_override;
	m_lifecycle.Request(config.configured, config.enabled, config.Url(), config.valid ? "" : "configurationInvalid");
}

nlohmann::json UamCompanionService::ConfigurationStatus(const std::filesystem::path& data_root)
{
	const auto config = ReadConnectionConfiguration(data_root);
	const bool failed = !config.valid || config.enabled;
	auto status = uam::companion::ConnectionStatus(
	    {config.configured, config.enabled, config.Url(), 0}, failed ? "failed" : "disabled", failed ? "configurationInvalid" : "disabled");
#if defined(__APPLE__)
	status["localAddresses"] = uam::platform::MacLocalIpv4Addresses();
#endif
	return status;
}

void UamCompanionService::BeginShutdownAll()
{
	std::lock_guard<std::mutex> lock(g_companion_services_mutex);
	g_companion_shutdown = true;
	for (const auto& control : g_companion_services) (void)control(true, std::chrono::milliseconds(0));
}

bool UamCompanionService::AllStopped()
{
	std::lock_guard<std::mutex> lock(g_companion_services_mutex);
	std::erase_if(g_companion_services, [](const auto& control) { return control(false, std::chrono::milliseconds(0)); });
	return g_companion_services.empty();
}

nlohmann::json UamCompanionService::Status() const
{
	auto status = m_lifecycle.Snapshot();
#if defined(__APPLE__)
	status["localAddresses"] = uam::platform::MacLocalIpv4Addresses();
#endif
	return status;
}

std::string UamCompanionServer::NewTransferId()
{
	return PlatformServicesFactory::Instance().process_service.GenerateUuid();
}

void UamCompanionServer::ScheduleTransferExpiry(CefRefPtr<CefServer> server)
{
	CefRefPtr<UamCompanionServer> self = this;
	std::function<void()> expire = [self, server]
	{
		{
			std::lock_guard<std::mutex> lock(self->m_mutex);
			if (self->m_stopped || !server->IsRunning())
				return;
		}
		self->m_request_transfers.Expire();
		self->m_response_transfers.Expire();
		self->ScheduleTransferExpiry(server);
	};
	server->GetTaskRunner()->PostDelayedTask(new Task(std::move(expire)), 5000);
}

std::optional<std::string> UamCompanionServer::StoreTransfer(std::string body)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_stopped || !m_server) return std::nullopt;
	const auto descriptor = m_response_transfers.Store(std::move(body));
	if (!descriptor) return std::nullopt;
	return nlohmann::json{{"uamTransfer", {{"id", descriptor->id}, {"totalBytes", descriptor->total_bytes}, {"chunkBytes", kCompanionTransferChunkBytes}}}}.dump();
}

std::optional<nlohmann::json> UamCompanionServer::ReadTransferChunk(const std::string& id, std::size_t offset)
{
	const auto chunk = m_response_transfers.Read(id, offset);
	if (!chunk) return std::nullopt;
	return nlohmann::json{{"base64", CefBase64Encode(chunk->bytes.data(), chunk->bytes.size()).ToString()}, {"nextOffset", chunk->next_offset}, {"done", chunk->done}};
}

void UamCompanionServer::OnHttpRequest(CefRefPtr<CefServer> server, int connection_id, const CefString&, CefRefPtr<CefRequest> request)
{
	const bool accepts_gzip = AcceptsGzip(request->GetHeaderByName("Accept-Encoding"));
	CefRefPtr<UamCompanionServer> self = this;
	CefRefPtr<Reply> reply = new Reply(server, connection_id, accepts_gzip, [self](std::string body) { return self->StoreTransfer(std::move(body)); });
	CefURLParts url;
	if (!CefParseURL(request->GetURL(), url)) { reply->Failure(400, "Invalid URL."); return; }
	const std::string path = CefString(&url.path);
	if (path.empty() || path.front() != '/') { reply->Failure(400, "Invalid path."); return; }
	if (request->GetMethod() == "GET")
	{
		const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
#if defined(__APPLE__)
		const std::filesystem::path root = executable.parent_path().parent_path() / "Resources/UI-V2/dist";
#else
		const std::filesystem::path root = executable.parent_path() / "UI-V2/dist";
#endif
		const std::string relative = path == "/companion" || path == "/companion/" ? "index.html" :
		                             path == "/apple-touch-icon.png" ? "app_icon-180.png" :
		                             path.substr(path.starts_with("/companion/") ? 11 : 1);
		std::error_code error;
		const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, error);
		if (error) { reply->Failure(404, "Companion UI unavailable."); return; }
		const std::filesystem::path file = std::filesystem::weakly_canonical(root / relative, error);
		const std::filesystem::path within = file.lexically_relative(canonical_root);
		if (error || within.empty() || *within.begin() == ".." || !std::filesystem::is_regular_file(file, error))
		{ reply->Failure(404, "Not found."); return; }
		const std::map<std::string, std::string> types = {{".webmanifest", "application/manifest+json"}, {".html", "text/html"}, {".js", "text/javascript"}, {".css", "text/css"}, {".woff", "font/woff"}, {".woff2", "font/woff2"}, {".png", "image/png"}, {".svg", "image/svg+xml"}, {".ico", "image/x-icon"}};
		const std::map<std::string, std::string>::const_iterator type = types.find(file.extension().string());
		if (type == types.end() || std::filesystem::file_size(file, error) > 16 * 1024 * 1024 || error)
		{ reply->Failure(404, "Not found."); return; }
		std::ifstream input(file, std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if (!input || input.bad()) { reply->Failure(404, "Companion asset unavailable."); return; }
		// The client closes after Content-Length bytes; closing here truncates queued socket writes.
		std::string wire_bytes;
		const BodyEncodingResult encoding = EncodeBody(bytes, accepts_gzip, &wire_bytes);
		if (encoding == BodyEncodingResult::Failed) { reply->Failure(500, "Response compression failed."); return; }
		if (encoding == BodyEncodingResult::TooLarge) { reply->Failure(413, "Response exceeds 900 KiB."); return; }
		CefServer::HeaderMap headers = {{"Cache-Control", "no-store"}, {"Connection", "close"}, {"X-Content-Type-Options", "nosniff"}, {"Content-Security-Policy", "frame-ancestors 'none'"}};
		if (encoding == BodyEncodingResult::Gzip) { headers.emplace("Content-Encoding", "gzip"); headers.emplace("Vary", "Accept-Encoding"); }
		server->SendHttpResponse(connection_id, 200, type->second, static_cast<int64_t>(wire_bytes.size()), headers);
		server->SendRawData(connection_id, wire_bytes.data(), wire_bytes.size());
		return;
	}
	const std::string authorization = request->GetHeaderByName("Authorization");
	const std::string expected = "Bearer " + m_token;
	unsigned int difference = static_cast<unsigned int>(authorization.size() ^ expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i)
		difference |= static_cast<unsigned int>(expected[i] ^ (i < authorization.size() ? authorization[i] : 0));
	if (difference != 0 || (!request->GetHeaderByName("Origin").empty() &&
	    request->GetHeaderByName("Origin").ToString() != "http://" + server->GetAddress().ToString() &&
	    request->GetHeaderByName("Origin").ToString() != m_origin))
	{
		reply->Failure(401, "Companion authentication required.");
		return;
	}
	if (request->GetMethod() != "POST" || path != "/api")
	{
		reply->Failure(404, "Unknown companion endpoint.");
		return;
	}
	CefRefPtr<CefPostData> post = request->GetPostData();
	CefPostData::ElementVector elements;
	if (post) post->GetElements(elements);
	std::string body;
	for (CefRefPtr<CefPostDataElement> element : elements)
	{
		if (element->GetType() != PDE_TYPE_BYTES || element->GetBytesCount() > 65536 - body.size())
		{
			reply->Failure(413, "Request exceeds 64 KiB.");
			return;
		}
		const std::size_t offset = body.size();
		body.resize(offset + element->GetBytesCount());
		element->GetBytes(element->GetBytesCount(), body.data() + offset);
	}
	ProcessApiRequest(std::move(body), reply);
}

void UamCompanionServer::ProcessApiRequest(std::string body, CefRefPtr<Callback> reply, bool transferred)
{
	const std::size_t request_bytes = body.size();
	uam::cef::BridgeRequestParseResult parsed = uam::cef::ParseBridgeRequest(body);
	std::string{}.swap(body);
	if (!parsed.ok)
	{
		reply->Failure(parsed.status, parsed.error);
		return;
	}
	const std::string& action = parsed.request.action;
	nlohmann::json& payload = parsed.request.payload;
	if (uam::companion::IsRequestTransferAction(action))
	{
		const uam::companion::PolicyResult validation = uam::companion::ValidateTransferPayload(action, payload, transferred);
		if (!validation.Ok())
		{
			reply->Failure(validation.status, validation.error);
			return;
		}
		uam::companion::TransferResult result;
		if (action == "beginCompanionRequestTransfer")
		{
			const std::uint64_t total = payload["totalBytes"].get<std::uint64_t>();
			result = m_request_transfers.Begin(static_cast<std::size_t>(total));
			if (result.Ok())
				reply->Success(nlohmann::json{{"id", result.id}, {"totalBytes", total}, {"chunkBytes", uam::companion::kRequestChunkBytes}, {"nextOffset", 0}, {"expiresInSeconds", uam::companion::kRequestTransferTtl.count()}}.dump());
		}
		else
		{
			const std::string id = payload["id"].get<std::string>();
			if (action == "appendCompanionRequestChunk")
			{
				result = m_request_transfers.Append(id, payload["offset"].get<std::size_t>(), payload["base64"].get_ref<const std::string&>());
				if (result.Ok())
					reply->Success(nlohmann::json{{"id", id}, {"nextOffset", result.next_offset}}.dump());
			}
			else
			{
				result = m_request_transfers.Commit(id);
				if (result.Ok())
					ProcessApiRequest(std::move(result.body), reply, true);
			}
		}
		if (!result.Ok())
			reply->Failure(result.status, result.error);
		return;
	}
	const uam::companion::PolicyResult policy = uam::companion::PrepareRequest(action, payload);
	if (!policy.Ok())
	{
		reply->Failure(policy.status, policy.error);
		return;
	}
	if (parsed.request.action == "getCompanionResponseChunk")
	{
		if (!payload.contains("id") || !payload["id"].is_string() || !payload.contains("offset") || !payload["offset"].is_number_unsigned() || payload["offset"].get<std::uint64_t>() > std::numeric_limits<std::size_t>::max())
		{
			reply->Failure(400, "Invalid transfer chunk request.");
			return;
		}
		const std::optional<nlohmann::json> chunk = ReadTransferChunk(payload["id"].get<std::string>(), payload["offset"].get<std::size_t>());
		if (!chunk) { reply->Failure(404, "Transfer not found."); return; }
		reply->Success(chunk->dump());
		return;
	}
	const auto admission = m_request_admission.Acquire(request_bytes);
	if (!admission)
	{
		reply->Failure(503, "Companion request capacity exhausted. Wait for current actions to finish.");
		return;
	}
	reply = new AdmittedReply(reply, admission);
	nlohmann::json envelope = {{"action", action}, {"payload", std::move(payload)}};
	CefRefPtr<UamCompanionServer> self = this;
	std::function<void()> dispatch_request = [self, envelope = std::move(envelope), reply]() mutable
	{
		try
		{
			self->DispatchCompanion(std::move(envelope), reply);
		}
		catch (const std::exception&)
		{
			reply->Failure(400, "Invalid companion payload.");
		}
	};
	if (!CefPostTask(TID_UI, new Task(std::move(dispatch_request))))
		reply->Failure(503, "UAM is shutting down.");
}

void UamCompanionServer::DispatchCompanion(nlohmann::json envelope, CefRefPtr<Callback> reply)
{
	const std::string action = envelope.at("action").get<std::string>();
	if (action == "getInitialState" || action == "getCompanionCapabilities" || action == "getCompanionSettings")
	{
		const nlohmann::json payload = action == "getInitialState" ? envelope["payload"] : nlohmann::json{{"summaryOnly", true}};
		JsonReply::SuccessHandler project_state = [reply, action](nlohmann::json state)
		{
			if (action == "getCompanionCapabilities")
				reply->Success(uam::companion::Capabilities(state).dump());
			else if (action == "getCompanionSettings")
				reply->Success(nlohmann::json{{"readOnly", false}, {"settings", uam::companion::MobileSettings(state.value("settings", nlohmann::json::object()))}}.dump());
			else
				reply->Success(uam::companion::MobileState(std::move(state)).dump());
		};
		m_dispatch({{"action", "getInitialState"}, {"payload", payload}}, new JsonReply(reply, std::move(project_state)));
		return;
	}
	if (action == "getMemorySettings")
	{
		m_dispatch(envelope, new JsonReply(reply, [reply](nlohmann::json result)
		{
			reply->Success(uam::companion::MemorySettingsOutcome(result).dump());
		}));
		return;
	}
	if (uam::companion::IsSettingsAction(action))
	{
		CefRefPtr<UamCompanionServer> self = this;
		JsonReply::FailureHandler failure = [reply, action](int status)
		{
			reply->Failure(status, action == "getProviderMcpStatus"
			    ? "MCP status could not be read. Refresh or check the main device."
			    : "Settings could not be saved. Refresh current values and check the main device.");
		};
		JsonReply::SuccessHandler prepare = [self, reply, action, envelope = std::move(envelope), failure](nlohmann::json state) mutable
		{
			const auto validation = uam::companion::PrepareSettingsDispatch(action, envelope["payload"], state);
			if (!validation.Ok())
			{
				reply->Failure(validation.status, validation.error);
				return;
			}
			JsonReply::SuccessHandler project = [reply, action](nlohmann::json result)
			{
				if (action == "getProviderMcpStatus" || action == "setProviderMcpConfiguration")
					result = uam::companion::MobileMcpStatus(result);
				else if (action == "setMemorySettings")
					result = uam::companion::MemorySettingsOutcome(result);
				else if (action == "setUamAgentPreferences")
					result = uam::companion::Pick(result, {"favoriteUamAgentIds", "uamAgentCycleShortcut"});
				else result = nlohmann::json::object();
				reply->Success(result.dump());
			};
			self->m_dispatch(envelope, new JsonReply(reply, std::move(project), failure));
		};
		// Complete omitted handler-required fields from current state immediately before dispatch.
		m_dispatch({{"action", "getInitialState"}, {"payload", {{"summaryOnly", true}}}}, new JsonReply(reply, std::move(prepare), failure));
		return;
	}
	const uam::companion::Action* policy = uam::companion::FindAction(action);
	if (policy && policy->chat_scoped && !policy->read_only)
	{
		CefRefPtr<UamCompanionServer> self = this;
		// Current state and dispatch run together on the UI thread; no desktop selection is acquired.
		JsonReply::SuccessHandler authorize = [self, reply, action, envelope = std::move(envelope)](nlohmann::json state)
		{
			const uam::companion::PolicyResult authorization = uam::companion::AuthorizeChat(action, envelope.at("payload"), state);
			if (!authorization.Ok())
			{
				reply->Failure(authorization.status, authorization.error);
				return;
			}
			const nlohmann::json& payload = envelope.at("payload");
			const std::string chat_id = payload.at("chatId").get<std::string>();
			const nlohmann::json scope = uam::companion::ChatAttachmentScope(state, chat_id);
			if (action == "sendAcpPrompt" && payload.contains("attachments") && !self->m_staged_attachments.Allows(chat_id, payload["attachments"], scope))
			{
				reply->Failure(403, "Attachments must exactly match this chat's companion staging results and workspace. Stage client bytes first.");
				return;
			}
			CefRefPtr<Callback> target = reply;
			if (action == "stageChatAttachments")
			{
				JsonReply::SuccessHandler remember = [self, reply, chat_id, scope](nlohmann::json result)
				{
					if (!result.contains("attachments") || !self->m_staged_attachments.Remember(chat_id, result["attachments"], scope))
					{
						reply->Failure(503, "Companion attachment descriptor capacity exhausted. Restage after expiry.");
						return;
					}
					reply->Success(result.dump());
				};
				target = new JsonReply(reply, std::move(remember));
			}
			self->m_dispatch(envelope, target);
		};
		m_dispatch({{"action", "getInitialState"}, {"payload", {{"summaryOnly", true}}}}, new JsonReply(reply, std::move(authorize)));
		return;
	}
	m_dispatch(envelope, reply);
}
