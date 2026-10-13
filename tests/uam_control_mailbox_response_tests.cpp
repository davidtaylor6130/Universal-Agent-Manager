#include "test_harness.h"
#include "app/uam_control_service.h"

using namespace uam_test;

namespace
{
	/// <summary>Drives the real MCP subprocess with private mailbox replies and checks that its next call survives rejection.</summary>
	void VerifyMalformedControlReplyRecovery(const nlohmann::json& malformed)
	{
		TempDir temp("uam-control-malformed-reply");
		const fs::path capability = temp.root / "capability";
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
