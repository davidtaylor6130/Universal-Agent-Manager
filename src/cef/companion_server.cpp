#include "cef/companion_server.h"
#include "cef/uam_bridge_request.h"
#include "common/utils/env_utils.h"
#include "common/platform/platform_services.h"
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

namespace
{
	constexpr std::size_t kGzipThreshold = 64 * 1024;
	constexpr std::size_t kMaxWireBody = 900 * 1024;

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

	// Replies run on the server thread so disconnected connection IDs cannot be reused between check and send.
	class Reply final : public CefMessageRouterBrowserSide::Callback
	{
	public:
		Reply(CefRefPtr<CefServer> server, int id, bool accepts_gzip, std::function<std::optional<std::string>(std::string)> store_transfer) : m_server(server), m_id(id), m_accepts_gzip(accepts_gzip), m_store_transfer(std::move(store_transfer)) {}
		void Success(const CefString& response) override { Send(200, response.ToString()); }
		void Success(const void*, size_t) override { Failure(500, "Binary replies are unavailable to the companion."); }
		void Failure(int status, const CefString& error) override
		{
			Send(status >= 400 && status <= 599 ? status : 500, nlohmann::json{{"error", error.ToString()}}.dump());
		}
	private:
		void Send(int status, std::string body)
		{
			std::string wire_body;
			const BodyEncodingResult encoding = EncodeBody(body, m_accepts_gzip, &wire_body);
			const bool too_large = encoding == BodyEncodingResult::TooLarge;
			if (too_large) wire_body.clear();
			else if (encoding == BodyEncodingResult::Failed)
			{
				status = 500;
				body = R"({"error":"Response compression failed."})";
				wire_body = body;
			}
			const bool gzip = encoding == BodyEncodingResult::Gzip;
			CefRefPtr<CefServer> server = m_server;
			const int id = m_id;
			const std::function<std::optional<std::string>(std::string)> store_transfer = m_store_transfer;
			server->GetTaskRunner()->PostTask(new Task([server, id, status, gzip, too_large, store_transfer, raw_body = std::move(body), body = std::move(wire_body)]() mutable {
				if (!server->IsValidConnection(id)) return;
				int response_status = status;
				bool response_gzip = gzip;
				if (too_large)
				{
					const std::optional<std::string> descriptor = store_transfer ? store_transfer(std::move(raw_body)) : std::nullopt;
					if (!descriptor) { response_status = 503; body = R"({"error":"Response transfer capacity exhausted."})"; }
					else { body = *descriptor; response_gzip = false; }
				}
				CefServer::HeaderMap headers = {{"Cache-Control", "no-store"}, {"Connection", "close"}};
				if (response_gzip) { headers.emplace("Content-Encoding", "gzip"); headers.emplace("Vary", "Accept-Encoding"); }
				server->SendHttpResponse(id, response_status, "application/json", static_cast<int64_t>(body.size()), headers);
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
struct CompanionConfig
{
	std::filesystem::path token_file;
	std::string port = "58948";
	std::string origin;
	std::filesystem::path proxy_executable;
	std::filesystem::path proxy_config;
};

std::optional<CompanionConfig> LoadConfig(const std::filesystem::path& data_root)
{
	if (data_root.empty()) return std::nullopt;
	std::ifstream input(data_root / "companion.json");
	if (!input) return std::nullopt;
	try
	{
		nlohmann::json config;
		input >> config;
		if (!config.is_object())
		{
			std::cerr << "Companion disabled: invalid persisted configuration.\n";
			return std::nullopt;
		}
		if (!config.contains("enabled") || !config["enabled"].is_boolean())
			return std::nullopt;
		if (!config["enabled"].get<bool>()) return std::nullopt;
		if (!config.contains("token_file") || !config["token_file"].is_string())
		{
			std::cerr << "Companion disabled: invalid persisted configuration.\n";
			return std::nullopt;
		}
		CompanionConfig result;
		result.token_file = config["token_file"].get<std::string>();
		if (config.contains("port"))
			result.port = config["port"].is_number_integer() ? std::to_string(config["port"].get<int>()) : config["port"].get<std::string>();
		if (config.contains("origin")) result.origin = config["origin"].get<std::string>();
		if (config.contains("proxy_executable")) result.proxy_executable = config["proxy_executable"].get<std::string>();
		if (config.contains("proxy_config")) result.proxy_config = config["proxy_config"].get<std::string>();
		return result;
	}
	catch (const std::exception&)
	{
		std::cerr << "Companion disabled: invalid persisted configuration.\n";
		return std::nullopt;
	}
}
}

CefRefPtr<UamCompanionServer> UamCompanionServer::StartFromEnvironment(Dispatch dispatch, const std::filesystem::path& data_root)
{
	const std::optional<std::string> environment_token_file = uam::env::GetNonEmptyString("UAM_COMPANION_TOKEN_FILE");
	const bool environment_configured = environment_token_file.has_value();
	const std::optional<CompanionConfig> persisted = environment_configured ? std::nullopt : LoadConfig(data_root);
	if (!environment_configured && !persisted) return nullptr;
	const std::filesystem::path token_file = environment_configured ? std::filesystem::path(*environment_token_file) : persisted->token_file;
	std::ifstream input(token_file);
	std::string token;
	std::getline(input, token);
	const std::string port_text = uam::env::GetNonEmptyString("UAM_COMPANION_PORT").value_or(persisted ? persisted->port : "58948");
	int port = 0;
	const std::from_chars_result parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
	if (token.size() != 64 || token.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
	    parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || port < 1025 || port > 65535)
	{
		std::cerr << "Companion disabled: invalid token file or port.\n";
		return nullptr;
	}
	const std::string origin = uam::env::GetNonEmptyString("UAM_COMPANION_ORIGIN").value_or(persisted ? persisted->origin : "");
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
	std::filesystem::path proxy_executable;
	std::filesystem::path proxy_config;
	if (persisted)
	{
		proxy_executable = persisted->proxy_executable;
		proxy_config = persisted->proxy_config;
		if (!proxy_config.empty() && proxy_executable.empty())
		{
			const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
#if defined(__APPLE__)
			proxy_executable = executable.parent_path().parent_path() / "Resources/companion/caddy";
#else
			proxy_executable = executable.parent_path() / "companion/caddy";
#endif
		}
	}
	CefRefPtr<UamCompanionServer> handler = new UamCompanionServer(std::move(token), origin, std::move(dispatch), std::move(proxy_executable), std::move(proxy_config));
	CefServer::CreateServer("127.0.0.1", static_cast<uint16_t>(port), 8, handler);
	return handler;
}

void UamCompanionServer::Stop()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stopped = true;
		if (m_server) m_server->Shutdown();
	}
	if (m_proxy_drainer.joinable())
	{
		m_proxy_drainer.request_stop();
		m_proxy_drainer.join();
	}
	if (m_proxy_started)
	{
		PlatformServicesFactory::Instance().process_service.StopStdioProcess(m_proxy_process, true);
		m_proxy_started = false;
	}
}

void UamCompanionServer::StartProxy()
{
	if (m_proxy_config.empty() || m_proxy_executable.empty()) return;
	std::string error;
	const std::vector<std::string> argv = {m_proxy_executable.string(), "run", "--config", m_proxy_config.string()};
	if (!PlatformServicesFactory::Instance().process_service.StartStdioProcess(m_proxy_process, m_proxy_config.parent_path(), argv, &error))
	{
		std::cerr << "Companion HTTPS proxy failed to start: " << error << "\n";
		return;
	}
	m_proxy_started = true;
	std::cerr << "Companion HTTPS proxy started.\n";
	m_proxy_drainer = std::jthread([this](std::stop_token stop_token) { DrainProxy(stop_token); });
}

void UamCompanionServer::DrainProxy(std::stop_token stop_token)
{
	char buffer[4096];
	std::string stderr_tail;
	while (!stop_token.stop_requested())
	{
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
		int exit_code = 0;
		if (PlatformServicesFactory::Instance().process_service.PollStdioProcessExited(m_proxy_process, &exit_code))
		{
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
	if (m_stopped) server->Shutdown();
	else if (server->IsRunning())
	{
		std::cerr << "Companion listening on loopback.\n";
		StartProxy();
	}
	else std::cerr << "Companion failed to bind its port.\n";
}

void UamCompanionServer::OnServerDestroyed(CefRefPtr<CefServer>)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_server = nullptr;
}

std::optional<std::string> UamCompanionServer::StoreTransfer(std::string body)
{
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	for (std::unordered_map<std::string, Transfer>::iterator it = m_transfers.begin(); it != m_transfers.end();)
		it = now - it->second.touched > std::chrono::seconds(120) ? m_transfers.erase(it) : std::next(it);
	if (m_transfers.size() >= 16) return std::nullopt;
	const std::string id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	m_transfers.emplace(id, Transfer{std::move(body), now});
	return nlohmann::json{{"uamTransfer", {{"id", id}, {"totalBytes", m_transfers.at(id).body.size()}}}}.dump();
}

std::optional<nlohmann::json> UamCompanionServer::ReadTransferChunk(const std::string& id, std::size_t offset)
{
	std::unordered_map<std::string, Transfer>::iterator it = m_transfers.find(id);
	if (it == m_transfers.end()) return std::nullopt;
	if (std::chrono::steady_clock::now() - it->second.touched > std::chrono::seconds(120))
	{
		m_transfers.erase(it);
		return std::nullopt;
	}
	if (offset >= it->second.body.size()) return std::nullopt;
	it->second.touched = std::chrono::steady_clock::now();
	const std::size_t count = std::min<std::size_t>(128 * 1024, it->second.body.size() - offset);
	const std::string encoded = CefBase64Encode(it->second.body.data() + offset, count).ToString();
	const std::size_t next = offset + count;
	const bool done = next == it->second.body.size();
	nlohmann::json result = {{"base64", encoded}, {"nextOffset", next}, {"done", done}};
	if (done) m_transfers.erase(it);
	return result;
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
	const uam::cef::BridgeRequestParseResult parsed = uam::cef::ParseBridgeRequest(body);
	if (!parsed.ok)
	{
		reply->Failure(parsed.status, parsed.error);
		return;
	}
	if (parsed.request.action == "getCompanionResponseChunk")
	{
		const nlohmann::json& payload = parsed.request.payload;
		if (!payload.contains("id") || !payload["id"].is_string() || !payload.contains("offset") ||
		    !payload["offset"].is_number_unsigned())
		{
			reply->Failure(400, "Invalid transfer chunk request.");
			return;
		}
		const std::optional<nlohmann::json> chunk = ReadTransferChunk(payload["id"].get<std::string>(), payload["offset"].get<std::size_t>());
		if (!chunk) { reply->Failure(404, "Transfer not found."); return; }
		reply->Success(chunk->dump());
		return;
	}
	nlohmann::json envelope = {{"action", parsed.request.action}, {"payload", parsed.request.payload}};
	const std::string& action = parsed.request.action;
	if (action == "getInitialState")
		envelope["payload"]["summaryOnly"] = true;
	if (action == "openNativeSessionChat" || action == "createSession")
		envelope["payload"]["selectChat"] = false;
	if (action != "getInitialState" && action != "getChatMessages" && action != "getToolCallContent" &&
	    action != "openNativeSessionChat" && action != "createSession" && action != "listRemoteDirectories" &&
	    action != "setChatPinned" && action != "setChatModel" && action != "setChatProvider" && action != "discoverProviderModels" &&
	    action != "setChatCommandSafetyTier" && action != "setChatUamControlEnabled" && action != "listUamAgents" && action != "setChatUamAgent" && action != "setChatMemoryEnabled" &&
	    action != "setChatCodexOptions" && action != "setChatApprovalMode" && action != "setAcpConfigOption" &&
	    action != "manageQueuedAcpPrompt" && action != "sendAcpPrompt" && action != "resolveAcpPermission" && action != "resolveAcpUserInput" && action != "cancelAcpTurn")
	{
		reply->Failure(403, "Action unavailable to the companion.");
		return;
	}
	const Dispatch dispatch = m_dispatch;
	if (!CefPostTask(TID_UI, new Task([dispatch, envelope = std::move(envelope), reply] {
		try { dispatch(envelope, reply); }
		catch (const std::exception&) { reply->Failure(400, "Invalid companion payload."); }
	}))) reply->Failure(503, "UAM is shutting down.");
}
