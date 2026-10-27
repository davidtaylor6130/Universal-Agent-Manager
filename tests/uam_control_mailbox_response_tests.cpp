#include "test_harness.h"
#include "app/uam_control_service.h"

using namespace uam_test;

namespace
{
	/// <summary>Drives the real MCP subprocess with private mailbox replies and checks that its next call survives rejection.</summary>
	void VerifyMalformedControlReplyRecovery(const nlohmann::json& malformed)
	{
		TempDir temp("uam-control-malformed-reply");
		const fs::path capability = temp.root / "uam-control" / "manager" / "capability"; // Canonical data-root layout.
		fs::create_directories(capability / "requests");
		fs::create_directories(capability / "responses");
		uam::platform::StdioProcessPlatformFields process;
		IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
		std::string error;
		UAM_ASSERT(service.StartStdioProcess(process, temp.root,
		    {service.ResolveCurrentExecutablePath().string(), "--uam-test-control-mcp"}, &error,
		    {{"UAM_CONTROL_CAPABILITY_DIR", capability.string()}}));
		struct StopProcess
		{
			uam::platform::StdioProcessPlatformFields& process;
			~StopProcess()
			{
				PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true);
				PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(process);
			}
		} stop{process};
		std::string output;
		const auto read_response = [&](int request_id) -> nlohmann::json
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			std::array<char, 4096> buffer{};
			while (std::chrono::steady_clock::now() < deadline)
			{
				const std::ptrdiff_t size = service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &error);
				if (size > 0) output.append(buffer.data(), static_cast<std::size_t>(size));
				const std::size_t newline = output.find('\n');
				if (newline != std::string::npos)
				{
					const nlohmann::json response = nlohmann::json::parse(output.substr(0, newline));
					output.erase(0, newline + 1);
					UAM_ASSERT_EQ(response["id"], nlohmann::json(request_id));
					return response;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			throw std::runtime_error("The actual MCP process did not reply after a private mailbox response.");
		};
		const auto send = [&](const nlohmann::json& request)
		{
			const std::string line = request.dump() + '\n';
			UAM_ASSERT(service.WriteToStdioProcess(process, line.data(), line.size(), &error));
		};
		const auto reply_to_mailbox = [&](const nlohmann::json& response)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				for (const fs::directory_entry& entry : fs::directory_iterator(capability / "requests"))
				{
					if (entry.path().extension() != ".json") continue;
					const nlohmann::json request = nlohmann::json::parse(uam::io::ReadTextFile(entry.path()));
					UAM_ASSERT_EQ(request["method"], nlohmann::json("goal_get"));
					UAM_ASSERT_EQ(request["arguments"], nlohmann::json::object());
					UAM_ASSERT_EQ(request["requestId"], nlohmann::json(entry.path().stem().string()));
					UAM_ASSERT(uam::io::WriteTextFile(capability / "responses" / entry.path().filename(), response.dump()));
					fs::remove(entry.path());
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			throw std::runtime_error("The actual MCP process did not publish its private tool request.");
		};
		send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", nlohmann::json::object()}});
		UAM_ASSERT_EQ(read_response(1)["result"]["serverInfo"]["name"], nlohmann::json("uam-control"));
		const auto call = [&](int id)
		{
			send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
			    {"params", {{"name", "goal_get"}, {"arguments", nlohmann::json::object()}}}});
		};
		call(2);
		reply_to_mailbox(malformed);
		const nlohmann::json rejected = read_response(2);
		UAM_ASSERT_EQ(rejected["result"]["isError"], nlohmann::json(true));
		const std::string rejected_text = rejected["result"]["content"][0]["text"].get<std::string>();
		UAM_ASSERT(rejected_text.size() < 256);
		UAM_ASSERT_EQ(nlohmann::json::parse(rejected_text)["error"], nlohmann::json("Control manager is unavailable."));
		call(3);
		reply_to_mailbox({{"ok", true}, {"result", {{"sentinel", "private valid recovery"}}}});
		const nlohmann::json recovered = read_response(3);
		UAM_ASSERT_EQ(recovered["result"]["isError"], nlohmann::json(false));
		UAM_ASSERT_EQ(nlohmann::json::parse(recovered["result"]["content"][0]["text"].get<std::string>()),
		    (nlohmann::json{{"sentinel", "private valid recovery"}}));
	}
}

UAM_TEST(UamControlStdioRejectsMalformedReplyOkAndContinues)
{
	VerifyMalformedControlReplyRecovery({{"ok", 1}});
}

UAM_TEST(UamControlStdioRejectsMissingReplyResultAndContinues)
{
	VerifyMalformedControlReplyRecovery({{"ok", true}});
}

UAM_TEST(UamControlStdioRejectsMalformedReplyErrorAndContinues)
{
	VerifyMalformedControlReplyRecovery({{"ok", false}, {"error", 1}});
}


UAM_TEST(UamControlStdioSingleWaitReturnsOwnedChildResultAfterEightSeconds)
{
	TempDir temp("uam-control-eight-second-wait");
	ScopedEnvVar codex("CODEX_HOME", (temp.root / "codex").string());
	ScopedEnvVar claude("CLAUDE_CONFIG_DIR", (temp.root / "claude").string());
	ScopedEnvVar config("XDG_CONFIG_HOME", (temp.root / "config").string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
	app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	ChatSession chat;
	chat.id = "wait-parent";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "local";
	chat.workspace_directory = workspace.string();
	chat.uam_control_enabled = true;
	chat.approval_mode = "default";
	chat.command_safety_tier = "yolo";
	chat.uam_agent_id = "build";
	chat.messages.push_back(Message{MessageRole::User, "Delegate one read-only child and wait for its result."});
	app.chats.push_back(chat);
	UAM_ASSERT(uam::UamControlService::Initialize(app));
	struct ShutdownManager
	{
		uam::AppState& app;
		~ShutdownManager() { uam::UamControlService::Shutdown(app); }
	} shutdown{app};
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.process_execution_host_id = chat.execution_host_id;
	session.running = true;
	session.processing = true;
	session.initialized = true;
	session.session_ready = true;
	session.session_id = "fixture-parent-session";
	session.codex_thread_id = session.session_id;
	session.codex_turn_id = "fixture-parent-turn";
	session.turn_serial = 1;
	session.last_process_id = "fixture-parent-process";
	session.active_uam_agent_id = "build";
	session.active_uam_agent_workspace_access = "write";
	session.turn_first_user_message_index = 0;
	session.turn_user_message_index = 0;
	nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, app.chats.front(), "session/new", setup));
	const fs::path capability_directory = app.uam_control_capabilities.front().directory;
	uam::platform::StdioProcessPlatformFields process;
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	std::string error;
	UAM_ASSERT(service.StartStdioProcess(process, temp.root,
	    {service.ResolveCurrentExecutablePath().string(), "--uam-test-control-mcp"}, &error,
	    {{"UAM_CONTROL_CAPABILITY_DIR", capability_directory.string()}}));
	struct StopProcess
	{
		uam::platform::StdioProcessPlatformFields& process;
		~StopProcess()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(process);
		}
	} stop{process};
	const auto send = [&](const nlohmann::json& request)
	{
		const std::string line = request.dump() + '\n';
		UAM_ASSERT(service.WriteToStdioProcess(process, line.data(), line.size(), &error));
	};
	std::string output;
	const auto read_available = [&]() -> std::optional<nlohmann::json>
	{
		std::array<char, 4096> buffer{};
		const std::ptrdiff_t size = service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &error);
		if (size > 0) output.append(buffer.data(), static_cast<std::size_t>(size));
		const std::size_t newline = output.find('\n');
		if (newline == std::string::npos) return std::nullopt;
		const nlohmann::json response = nlohmann::json::parse(output.substr(0, newline));
		output.erase(0, newline + 1);
		return response;
	};
	const auto read_setup_response = [&](int id) -> nlohmann::json
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::UamControlService::ProcessPendingRequests(app);
			if (const auto response = read_available())
			{
				UAM_ASSERT_EQ(response->at("id"), nlohmann::json(id));
				return *response;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		throw std::runtime_error("Real MCP initialization/delegation did not complete.");
	};
	send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", nlohmann::json::object()}});
	UAM_ASSERT_EQ(read_setup_response(1)["result"]["serverInfo"]["name"], nlohmann::json("uam-control"));
	send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
	// Public delegation establishes ownership through the real manager, not an injected grant.
	send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
	    {"params", {{"name", "agent_delegate"}, {"arguments", {{"agentId", "plan"}, {"task", "Return the fixture terminal result."}}}}}});
	const nlohmann::json delegated = read_setup_response(2);
	UAM_ASSERT_EQ(delegated["result"]["isError"], nlohmann::json(false));
	const std::string run_id = nlohmann::json::parse(delegated["result"]["content"][0]["text"].get<std::string>()).at("runId");
	UAM_ASSERT(app.uam_control_capabilities.front().mcp_initialized);
	UAM_ASSERT(!app.uam_control_capabilities.front().mcp_connection_id.empty());
	UAM_ASSERT(app.uam_control_capabilities.front().owned_agent_run_ids.contains(run_id));
	UAM_ASSERT_EQ(app.agent_runs.size(), std::size_t{1});
	AgentRun& child = app.agent_runs.front();
	UAM_ASSERT_EQ(child.id, run_id);
	UAM_ASSERT_EQ(child.root_chat_id, chat.id);
	UAM_ASSERT_EQ(child.effective_workspace_access, std::string("read"));
	child.status = "running";
	child.started_at_epoch_ms = uam::time::SystemEpochMillisecondsNow();
	child.deadline_at_epoch_ms = child.started_at_epoch_ms + 30'000;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, child));
	const nlohmann::json wait_id = "single-eight-second-wait";
	send({{"jsonrpc", "2.0"}, {"id", wait_id}, {"method", "tools/call"},
	    {"params", {{"name", "agent_wait"}, {"arguments", {{"runId", run_id}}}}}});
	std::optional<std::chrono::steady_clock::time_point> wait_observed_at;
	std::vector<std::string> wait_request_ids;
	bool child_completed = false;
	const std::string terminal_result = "Owned child completed after eight real seconds.";
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (std::chrono::steady_clock::now() < deadline)
	{
		// Observe genuine proxy requests before the manager consumes them; never create replies here.
		for (const fs::directory_entry& entry : fs::directory_iterator(capability_directory / "requests"))
		{
			if (entry.path().extension() != ".json") continue;
			const nlohmann::json request = nlohmann::json::parse(uam::io::ReadTextFile(entry.path()));
			const std::string method = request.at("method");
			UAM_ASSERT(method == "agent_wait" || method == "_mcp_heartbeat");
			if (method != "agent_wait") continue;
			UAM_ASSERT_EQ(request.at("arguments"), (nlohmann::json{{"runId", run_id}}));
			UAM_ASSERT_EQ(request.at("requestId"), nlohmann::json(entry.path().stem().string()));
			const std::string request_id = request.at("requestId");
			if (std::ranges::find(wait_request_ids, request_id) == wait_request_ids.end()) wait_request_ids.push_back(request_id);
			UAM_ASSERT_EQ(wait_request_ids.size(), std::size_t{1});
			if (!wait_observed_at) wait_observed_at = std::chrono::steady_clock::now();
		}
		if (!child_completed && wait_observed_at &&
		    std::chrono::steady_clock::now() - *wait_observed_at >= std::chrono::seconds(8))
		{
			// Simulated child lifecycle only: the MCP response still comes exclusively from the manager.
			child.status = "completed";
			child.result_excerpt = terminal_result;
			child.finished_at_epoch_ms = uam::time::SystemEpochMillisecondsNow();
			UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, child));
			child_completed = true;
		}
		// Do not let the manager consume a newly published wait before this fixture observes it.
		if (wait_observed_at) (void)uam::UamControlService::ProcessPendingRequests(app);
		if (const auto response = read_available())
		{
			if (!child_completed)
				throw std::runtime_error("agent_wait replied before the owned child's eight-second completion: " + response->dump());
			UAM_ASSERT_EQ(response->at("id"), wait_id);
			UAM_ASSERT_EQ(response->at("result").at("isError"), nlohmann::json(false));
			const nlohmann::json result = nlohmann::json::parse(response->at("result").at("content")[0].at("text").get<std::string>());
			UAM_ASSERT_EQ(result.at("runId"), nlohmann::json(run_id));
			UAM_ASSERT_EQ(result.at("status"), nlohmann::json("completed"));
			UAM_ASSERT_EQ(result.at("result"), nlohmann::json(terminal_result));
			UAM_ASSERT_EQ(wait_request_ids.size(), std::size_t{1});
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("The original agent_wait tools/call did not return its terminal child result within 30 seconds.");
}


namespace
{
	/// Real stdio proxy, synthetic manager receipts only: no child/provider is launched.
	struct WaitProxyFixture
	{
		TempDir temp{"uam-wait-proxy-input"};
		fs::path capability = temp.root / "uam-control" / "manager" / "capability"; // Canonical data-root layout.
		uam::platform::StdioProcessPlatformFields process;
		IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
		std::string error;
		std::string output;

		WaitProxyFixture()
		{
			fs::create_directories(capability / "requests");
			fs::create_directories(capability / "responses");
			UAM_ASSERT(service.StartStdioProcess(process, temp.root,
			    {service.ResolveCurrentExecutablePath().string(), "--uam-test-control-mcp"}, &error,
			    {{"UAM_CONTROL_CAPABILITY_DIR", capability.string()}}));
		}
		~WaitProxyFixture()
		{
			service.StopStdioProcess(process, true);
			service.CloseStdioProcessHandles(process);
		}
		void Send(const nlohmann::json& message)
		{
			const std::string bytes = message.dump() + '\n';
			UAM_ASSERT(service.WriteToStdioProcess(process, bytes.data(), bytes.size(), &error));
		}
		std::optional<nlohmann::json> Read()
		{
			std::array<char, 4096> bytes{};
			const auto size = service.ReadStdioProcessStdout(process, bytes.data(), bytes.size(), &error);
			if (size > 0) output.append(bytes.data(), static_cast<std::size_t>(size));
			const auto newline = output.find('\n');
			if (newline == std::string::npos) return std::nullopt;
			const auto message = nlohmann::json::parse(output.substr(0, newline));
			output.erase(0, newline + 1);
			return message;
		}
		nlohmann::json Native(const nlohmann::json& id)
		{
			const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (std::chrono::steady_clock::now() < end)
			{
				if (const auto message = Read()) { UAM_ASSERT_EQ(message->at("id"), id); return *message; }
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			throw std::runtime_error("Proxy fixture did not return its exact native request ID.");
		}
		void Reply(const nlohmann::json& request, const nlohmann::json& result = nlohmann::json::object())
		{
			const std::string id = request.at("requestId");
			UAM_ASSERT(uam::io::WriteTextFile(capability / "responses" / (id + ".json"),
			    nlohmann::json{{"ok", true}, {"result", result}}.dump()));
		}
		nlohmann::json Take(std::string_view method)
		{
			const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (std::chrono::steady_clock::now() < end)
			{
				for (const auto& file : fs::directory_iterator(capability / "requests"))
				{
					if (file.path().extension() != ".json") continue;
					const auto request = nlohmann::json::parse(uam::io::ReadTextFile(file.path()));
					const std::string name = request.at("method");
					if (name == method)
					{
						UAM_ASSERT_EQ(request.at("requestId"), nlohmann::json(file.path().stem().string()));
						fs::remove(file.path()); // Synthetic manager adopts, but never invents a tool result.
						return request;
					}
					UAM_ASSERT_EQ(name, std::string("_mcp_heartbeat"));
					fs::remove(file.path());
					Reply(request);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			throw std::runtime_error("Expected proxy mailbox request was not observed.");
		}
		std::string Initialize()
		{
			Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", nlohmann::json::object()}});
			UAM_ASSERT_EQ(Native(1)["result"]["serverInfo"]["name"], nlohmann::json("uam-control"));
			Send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
			const auto initialized = Take("_mcp_initialized");
			Reply(initialized);
			return initialized.at("arguments").at("connectionId");
		}
		void Wait(const nlohmann::json& id)
		{
			Send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
			    {"params", {{"name", "agent_wait"}, {"arguments", {{"runId", "synthetic-owned-run"}}}}}});
		}
		void Exit(int expected)
		{
			const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			int code = -1;
			while (std::chrono::steady_clock::now() < end)
			{
				if (service.PollStdioProcessExited(process, &code)) { UAM_ASSERT_EQ(code, expected); return; }
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			throw std::runtime_error("Proxy did not exit while its input writer remained open.");
		}
	};
}

UAM_TEST(UamControlStdioWaitCancellationPreservesNextCallAndEofExits)
{
	WaitProxyFixture fixture;
	const std::string connection = fixture.Initialize();
	fixture.Wait("cancelled-wait");
	const auto wait = fixture.Take("agent_wait");
	fixture.Send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
	    {"params", {{"requestId", "unrelated-call"}}}});
	fixture.Send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
	    {"params", {{"requestId", "cancelled-wait"}}}});
	const auto cancelled = fixture.Take("_mcp_cancel_request");
	UAM_ASSERT_EQ(cancelled.at("arguments"), (nlohmann::json{{"connectionId", connection}, {"requestId", wait.at("requestId")}}));
	fixture.Reply(wait, {{"forbiddenOldPayload", true}}); // Must never be adopted by the next call.
	// The proxy leaves the raced reply for the manager, whose wait retirement removes it while handling the cancel.
	const fs::path raced_reply = fixture.capability / "responses" / (wait.at("requestId").get<std::string>() + ".json");
	UAM_ASSERT(fs::exists(raced_reply));
	fs::remove(raced_reply);
	fixture.Reply(cancelled);
	fixture.Send({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
	    {"params", {{"name", "goal_get"}, {"arguments", nlohmann::json::object()}}}});
	const auto next = fixture.Take("goal_get");
	fixture.Reply(next, {{"sentinel", "next-call-only"}});
	const auto response = fixture.Native(3); // Any late cancelled-wait output fails this exact ID check.
	UAM_ASSERT_EQ(nlohmann::json::parse(response.at("result").at("content")[0].at("text").get<std::string>()),
	    (nlohmann::json{{"sentinel", "next-call-only"}}));
	// JSON-RPC string "23" must not cancel numeric23. No fake result can mask a bad match.
	fixture.Wait(23);
	const auto numeric_wait = fixture.Take("agent_wait");
	fixture.Send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
	    {"params", {{"requestId", "23"}}}});
	const auto unrelated_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
	while (std::chrono::steady_clock::now() < unrelated_deadline)
	{
		for (const auto& file : fs::directory_iterator(fixture.capability / "requests"))
		{
			if (file.path().extension() != ".json") continue;
			const auto request = nlohmann::json::parse(uam::io::ReadTextFile(file.path()));
			UAM_ASSERT_EQ(request.at("method"), nlohmann::json("_mcp_heartbeat"));
			fs::remove(file.path());
			fixture.Reply(request);
		}
		UAM_ASSERT(!fixture.Read().has_value());
		int unexpected_exit = -1;
		UAM_ASSERT(!fixture.service.PollStdioProcessExited(fixture.process, &unexpected_exit));
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const nlohmann::json exact_numeric_cancel{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
	    {"params", {{"requestId", 23}}}};
	const nlohmann::json coalesced_next{{"jsonrpc", "2.0"}, {"id", 24}, {"method", "tools/call"},
	    {"params", {{"name", "goal_get"}, {"arguments", nlohmann::json::object()}}}};
	const std::string coalesced_bytes = exact_numeric_cancel.dump() + '\n' + coalesced_next.dump() + '\n';
	// One write carrying both complete lines; transport may split reads, neither line may be lost.
	UAM_ASSERT(fixture.service.WriteToStdioProcess(fixture.process, coalesced_bytes.data(), coalesced_bytes.size(), &fixture.error));
	const auto numeric_cancel = fixture.Take("_mcp_cancel_request");
	UAM_ASSERT_EQ(numeric_cancel.at("arguments"), (nlohmann::json{{"connectionId", connection}, {"requestId", numeric_wait.at("requestId")}}));
	fixture.Reply(numeric_cancel);
	const auto numeric_next = fixture.Take("goal_get");
	fixture.Reply(numeric_next, {{"sentinel", "numeric-next-call-only"}});
	const auto numeric_response = fixture.Native(24);
	UAM_ASSERT_EQ(numeric_response.at("result").at("isError"), nlohmann::json(false));
	UAM_ASSERT_EQ(nlohmann::json::parse(numeric_response.at("result").at("content")[0].at("text").get<std::string>()),
	    (nlohmann::json{{"sentinel", "numeric-next-call-only"}}));
	fixture.Wait(4);
	const auto second = fixture.Take("agent_wait");
	fixture.service.CloseStdioProcessInput(fixture.process);
	const auto eof = fixture.Take("_mcp_cancel_request");
	UAM_ASSERT_EQ(eof.at("arguments"), (nlohmann::json{{"connectionId", connection}, {"requestId", second.at("requestId")}}));
	fixture.Reply(eof);
	fixture.Exit(0);
	UAM_ASSERT(!fixture.Read().has_value()); // No abandoned wait4 response on EOF.
}

UAM_TEST(UamControlStdioOversizePartialLineExitsWithInputStillOpen)
{
	WaitProxyFixture fixture;
	const std::string bytes(64U * 1024U + 1U, 'x'); // Deliberately no newline or EOF.
	UAM_ASSERT(fixture.service.WriteToStdioProcess(fixture.process, bytes.data(), bytes.size(), &fixture.error));
	fixture.Exit(2);
}

UAM_TEST(UamControlStdioWaitQueueOverflowAbortsWithoutReaderJoin)
{
	WaitProxyFixture fixture;
	const std::string connection = fixture.Initialize();
	fixture.Wait(2);
	const auto wait = fixture.Take("agent_wait");
	for (int id = 10; id < 43; ++id)
		fixture.Send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/list"}});
	const auto cancel = fixture.Take("_mcp_cancel_request");
	UAM_ASSERT_EQ(cancel.at("arguments"), (nlohmann::json{{"connectionId", connection}, {"requestId", wait.at("requestId")}}));
	fixture.Reply(cancel);
	fixture.Exit(2); // Writer remains open; a blocked getline/thread join cannot pass.
	UAM_ASSERT(!fixture.Read().has_value());
}
