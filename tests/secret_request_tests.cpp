#include "test_harness.h"
#include "app/chat_domain_service.h"
#include "app/local_chat_bundle_service.h"
#include "app/memory_service.h"
#include "app/secret_request_service.h"
#include "app/uam_control_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/companion/protocol.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/secrets/secret_drop.h"
#include "common/secrets/secret_keychain.h"
#include "common/secrets/secret_redactor.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"
#include "common/utils/time_utils.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <thread>

using namespace uam_test;

namespace
{
	constexpr const char* kCanary = "ghp_CanarySecretValue_8c41f2";

	/// One caller chat with a live structured session and an issued UAM control capability.
	struct SecretFixture
	{
		TempDir temp{"uam-secret-request"};
		uam::AppState app;
		SecretFixture()
		{
			app.data_root = temp.root / "data";
			fs::create_directories(app.data_root);
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			AddChat("caller");
			Issue("caller-capability", "caller", "caller");
		}
		ChatSession& AddChat(const std::string& id)
		{
			ChatSession chat;
			chat.id = id;
			chat.title = "Release chat";
			chat.created_at = "2026-11-12 10:00:00";
			chat.provider_id = "codex-cli";
			chat.approval_mode = "acceptEdits";
			chat.execution_host_id = "local";
			chat.workspace_directory = (temp.root / "workspace").string();
			chat.uam_control_enabled = true;
			chat.uam_agent_id = "build";
			chat.messages.push_back(Message{MessageRole::User, "Push the release tag."});
			app.chats.push_back(chat);
			return app.chats.back();
		}
		uam::AcpSessionState& Issue(const std::string& id, const std::string& root_chat_id, const std::string& session_chat_id, const std::string& agent = "build")
		{
			uam::UamControlCapability capability;
			capability.id = id;
			capability.directory = temp.root / id;
			fs::create_directories(capability.directory / "responses");
			fs::create_directories(capability.directory / "requests");
			capability.chat_id = root_chat_id;
			capability.session_chat_id = session_chat_id;
			capability.provider_id = "codex-cli";
			capability.agent_id = agent;
			capability.expires_at_epoch_ms = uam::time::SystemEpochMillisecondsNow() + 3600000;
			app.uam_control_capabilities.push_back(capability);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = session_chat_id;
			session->provider_id = "codex-cli";
			session->running = true;
			session->uam_control_capability_id = id;
			app.acp_sessions.push_back(std::move(session));
			return *app.acp_sessions.back();
		}
		ChatSession& Chat(const std::string& id) { return *ChatDomainService().FindChatById(app, id); }
		nlohmann::json Ask(const std::string& request_id, nlohmann::json arguments, const std::string& capability = "caller-capability")
		{
			return uam::UamControlService::HandleRequestForTests(app, capability,
			    {{"requestId", request_id}, {"method", "secret_request"}, {"arguments", std::move(arguments)}}, uam::time::SystemEpochMillisecondsNow());
		}
		nlohmann::json Response(const std::string& request_id, const std::string& capability = "caller-capability")
		{
			std::string text;
			UAM_ASSERT(uam::io::TryReadTextFile(temp.root / capability / "responses" / (request_id + ".json"), text));
			return nlohmann::json::parse(text);
		}
		static nlohmann::json Token() { return {{"label", "GitHub token"}, {"reason", "To push the v5.1.0 tag."}}; }
	};

	std::string ReadAll(const fs::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		std::ostringstream text;
		text << input.rdbuf();
		return text.str();
	}

	/// Every byte UAM wrote under root, so a canary anywhere on disk is found.
	std::string Everything(const fs::path& root)
	{
		std::string all;
		std::error_code error;
		for (const auto& entry : fs::recursive_directory_iterator(root, error))
			if (entry.is_regular_file(error)) all += ReadAll(entry.path());
		return all;
	}

	std::string ReadOnce(const std::string& path)
	{
		return ReadAll(uam::paths::PathFromUtf8(path));
	}
} // namespace

UAM_TEST(SecretRequestToolIsListedWithoutReadOnlyHint)
{
	const nlohmann::json tools = uam::UamControlService::ToolDefinitionsForTests();
	const auto found = std::ranges::find_if(tools, [](const nlohmann::json& tool) { return tool.value("name", "") == "secret_request"; });
	UAM_ASSERT(found != tools.end());
	UAM_ASSERT(!found->contains("annotations"));
	UAM_ASSERT(found->at("description").get<std::string>().find("never the value") != std::string::npos);
}

UAM_TEST(SecretRequestRejectsInvalidArguments)
{
	SecretFixture fixture;
	UAM_ASSERT(!fixture.Ask("r1", {{"label", "Token"}}).value("ok", true));
	UAM_ASSERT(!fixture.Ask("r2", {{"label", "Token\nline"}, {"reason", "x"}}).value("ok", true));
	UAM_ASSERT(!fixture.Ask("r3", {{"label", "Token"}, {"reason", "x"}, {"url", "http://example.com"}}).value("ok", true));
	UAM_ASSERT(!fixture.Ask("r4", {{"label", "Token"}, {"reason", "x"}, {"delivery", "ref"}}).value("ok", true));
	UAM_ASSERT(!fixture.Ask("r5", {{"label", "Token"}, {"reason", "x"}, {"value", "sneaky"}}).value("ok", true));
	UAM_ASSERT(!fixture.Ask("r6", {{"label", std::string(81, 'a')}, {"reason", "x"}}).value("ok", true));
	UAM_ASSERT(fixture.Ask("r7", {{"label", "Token"}, {"reason", "x"}, {"url", "https://github.com/settings/tokens"}, {"delivery", "file"}}).value("pendingApproval", false));
}

UAM_TEST(SecretRequestRefusesReadOnlyGoalReviewSideAndAutomationCallers)
{
	{
		SecretFixture fixture;
		fixture.Issue("plan-agent", "caller", "caller", "plan");
		const nlohmann::json reply = fixture.Ask("r1", SecretFixture::Token(), "plan-agent");
		UAM_ASSERT(!reply.value("ok", true) && reply.value("error", "").find("read-only") != std::string::npos);
	}
	{
		SecretFixture fixture;
		fixture.app.acp_sessions.front()->goal_review_turn = true;
		// A goal reviewer is read-only, so the shared ceiling refuses it before the secret-specific check.
		const nlohmann::json reply = fixture.Ask("r1", SecretFixture::Token());
		const std::string error = reply.value("error", "");
		UAM_ASSERT(!reply.value("ok", true) && (error.find("read-only") != std::string::npos || error.find("Goal review") != std::string::npos));
		UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(fixture.app, "caller").has_value());
	}
	{
		SecretFixture fixture;
		fixture.Chat("caller").temporary_parent_chat_id = "parent";
		UAM_ASSERT(fixture.Ask("r1", SecretFixture::Token()).value("error", "").find("Side chats") != std::string::npos);
	}
	{
		SecretFixture fixture;
		fixture.Chat("caller").automation_token_id = "token";
		UAM_ASSERT(fixture.Ask("r1", SecretFixture::Token()).value("error", "").find("Automation") != std::string::npos);
	}
	SecretFixture fixture;
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(fixture.app, "caller").has_value());
}

UAM_TEST(SecretRequestPlanModeNeedsAllowOnce)
{
	SecretFixture fixture;
	fixture.Chat("caller").approval_mode = "plan";
	UAM_ASSERT(fixture.Ask("r1", SecretFixture::Token()).value("pendingApproval", false));
	auto pending = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
	UAM_ASSERT(pending.has_value() && pending->secret_request.value("planOptIn", false));
	uam::SecretDeliveryTarget target;
	std::string error;
	UAM_ASSERT(!uam::UamControlService::BeginSecretDelivery(fixture.app, "caller", pending->request_id_json, target, &error));
	// A typed value never travels the generic answer path, which the phone can reach.
	UAM_ASSERT(!uam::UamControlService::ResolveApproval(fixture.app, "caller", pending->request_id_json, {{"secretRequest", {kCanary}}}, &error));
	UAM_ASSERT(uam::UamControlService::ResolveApproval(fixture.app, "caller", pending->request_id_json, {{"secretRequest", {"Allow"}}}, &error));
	pending = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
	UAM_ASSERT(pending.has_value() && !pending->secret_request.value("planOptIn", true));
	UAM_ASSERT(uam::UamControlService::BeginSecretDelivery(fixture.app, "caller", pending->request_id_json, target, &error));
}

UAM_TEST(SecretRequestShowsInRootChatForSubagentAndTellsThePhoneToUseTheComputer)
{
	SecretFixture fixture;
	fixture.AddChat("child");
	fixture.Issue("child-capability", "caller", "child");
	UAM_ASSERT(fixture.Ask("r1", SecretFixture::Token(), "child-capability").value("pendingApproval", false));
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(fixture.app, "child").has_value());
	const auto pending = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
	UAM_ASSERT(pending.has_value());
	UAM_ASSERT_EQ(pending->questions.size(), std::size_t{1});
	UAM_ASSERT_EQ(pending->questions.front().id, std::string("secretRequest"));
	UAM_ASSERT(pending->questions.front().options.empty() && !pending->questions.front().is_other);
	UAM_ASSERT(pending->questions.front().question.find("Answer it on the computer") != std::string::npos);
	UAM_ASSERT_EQ(pending->secret_request.value("label", ""), std::string("GitHub token"));
	UAM_ASSERT(pending->secret_request.at("host").is_null());
	const nlohmann::json state = nlohmann::json::parse(uam::StateUpdateForTests(fixture.app));
	UAM_ASSERT(state.dump().find("\"secretRequest\"") != std::string::npos);
}

UAM_TEST(SecretRequestDenyAndTurnStopAnswerTheAgent)
{
	SecretFixture fixture;
	UAM_ASSERT(fixture.Ask("r1", SecretFixture::Token()).value("pendingApproval", false));
	auto pending = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
	std::string error;
	UAM_ASSERT(uam::UamControlService::ResolveApproval(fixture.app, "caller", pending->request_id_json, {{"secretRequest", {"Deny"}}}, &error));
	UAM_ASSERT(fixture.Response("r1").value("error", "").find("cancelled") != std::string::npos);
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(fixture.app, "caller").has_value());

	UAM_ASSERT(fixture.Ask("r2", SecretFixture::Token()).value("pendingApproval", false));
	UAM_ASSERT(uam::UamControlService::DeclinePendingApprovalForSession(fixture.app, *fixture.app.acp_sessions.front(), "The user stopped the turn."));
	UAM_ASSERT(!fixture.Response("r2").value("ok", true));
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(fixture.app, "caller").has_value());
}

UAM_TEST(SecretRequestDeliversOnlyAReadOncePath)
{
	uam::secrets::ResetForTests();
	SecretFixture fixture;
	UAM_ASSERT(fixture.Ask("r1", SecretFixture::Token()).value("pendingApproval", false));
	const auto pending = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
	uam::SecretDeliveryTarget target;
	std::string error;
	UAM_ASSERT(uam::UamControlService::BeginSecretDelivery(fixture.app, "caller", pending->request_id_json, target, &error));
	UAM_ASSERT(!target.remote_host.has_value());
	// A second submit while the first is in flight is refused.
	uam::SecretDeliveryTarget again;
	UAM_ASSERT(!uam::UamControlService::BeginSecretDelivery(fixture.app, "caller", pending->request_id_json, again, &error));
	const uam::secrets::DropResult drop = uam::SecretRequestService::Deliver(target, kCanary);
	UAM_ASSERT(drop.ok);
	UAM_ASSERT(uam::UamControlService::FinishSecretDelivery(fixture.app, "caller", pending->request_id_json, drop, &error));
	const nlohmann::json reply = fixture.Response("r1");
	UAM_ASSERT(reply.value("ok", false));
	UAM_ASSERT_EQ(reply["result"].value("path", ""), drop.path);
	UAM_ASSERT(reply.dump().find(kCanary) == std::string::npos);
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(fixture.app, "caller").has_value());
	UAM_ASSERT_EQ(ReadOnce(drop.path), std::string(kCanary));
#ifndef _WIN32
	// The pipe serves one reader, then it is gone; nothing was ever stored on disk.
	for (int attempt = 0; attempt < 100 && fs::exists(uam::paths::PathFromUtf8(drop.path)); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	UAM_ASSERT(!fs::exists(uam::paths::PathFromUtf8(drop.path)));
#endif
	// Windows keeps an owner-only file until its TTL; stopping removes it before the scan.
	uam::secrets::StopAllDrops();
	UAM_ASSERT(Everything(fixture.temp.root).find(kCanary) == std::string::npos);
}

UAM_TEST(SecretRequestTargetsTheAskingAgentsHost)
{
	SecretFixture fixture;
	ExecutionHost host;
	host.id = "build-box";
	host.label = "build-box";
	host.transport = "ssh";
	host.ssh_alias = "build-box";
	host.platform = "linux";
	host.runner_status = "unavailable";
	fixture.app.settings.execution_hosts.push_back(host);
	fixture.AddChat("child").execution_host_id = "build-box";
	fixture.Issue("child-capability", "caller", "child");
	UAM_ASSERT(fixture.Ask("r1", SecretFixture::Token(), "child-capability").value("pendingApproval", false));
	const auto pending = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
	UAM_ASSERT_EQ(pending->secret_request.at("host").value("name", ""), std::string("build-box"));
	UAM_ASSERT(!pending->secret_request.at("keychain").value("available", true));
	uam::SecretDeliveryTarget target;
	std::string error;
	UAM_ASSERT(uam::UamControlService::BeginSecretDelivery(fixture.app, "caller", pending->request_id_json, target, &error));
	UAM_ASSERT(target.remote_host.has_value() && target.remote_host->id == "build-box");
	// An unready helper fails without contacting the host and without echoing the value.
	const uam::secrets::DropResult drop = uam::SecretRequestService::Deliver(target, kCanary);
	UAM_ASSERT(!drop.ok && drop.error.find(kCanary) == std::string::npos);
	UAM_ASSERT(!uam::UamControlService::FinishSecretDelivery(fixture.app, "caller", pending->request_id_json, drop, &error));
	// The card stays open for Retry.
	UAM_ASSERT(uam::UamControlService::PendingApprovalForChat(fixture.app, "caller").has_value());
	UAM_ASSERT(uam::UamControlService::BeginSecretDelivery(fixture.app, "caller", pending->request_id_json, target, &error));
}

UAM_TEST(SecretRequestRefusesSavingOnWindowsAndForSshHosts)
{
	uam::SecretDeliveryTarget local;
	uam::SecretDeliveryTarget remote;
	remote.remote_host = ExecutionHost{};
	UAM_ASSERT(uam::SecretRequestService::SaveRefusal(local, false, false).empty());
	UAM_ASSERT(!uam::SecretRequestService::SaveRefusal(remote, true, false).empty());
	UAM_ASSERT(!uam::SecretRequestService::SaveRefusal(remote, false, true).empty());
#ifdef _WIN32
	// Windows secrets are one-time: Credential Manager would let any program of the user read them.
	UAM_ASSERT(!uam::secrets::keychain::Available());
	UAM_ASSERT(!uam::SecretRequestService::SaveRefusal(local, true, false).empty());
	UAM_ASSERT(!uam::SecretRequestService::SaveRefusal(local, false, true).empty());
	std::string error;
	UAM_ASSERT(!uam::secrets::keychain::Save("account", "value-123456", &error) && !error.empty());
#else
	UAM_ASSERT(uam::SecretRequestService::SaveRefusal(local, true, false).empty());
#endif
	SecretFixture fixture;
	UAM_ASSERT(fixture.Ask("r1", SecretFixture::Token()).value("pendingApproval", false));
	const auto pending = uam::UamControlService::PendingApprovalForChat(fixture.app, "caller");
	UAM_ASSERT_EQ(pending->secret_request.at("keychain").value("available", true), uam::secrets::keychain::Available());
}

UAM_TEST(SecretRedactionKeepsEchoesOutOfChatLogsMemoryCompanionAndExport)
{
	uam::secrets::ResetForTests();
	UAM_ASSERT(uam::secrets::Register(kCanary));
	TempDir temp{"uam-secret-leaks"};
	uam::AppState app;
	app.data_root = temp.root / "data";
	fs::create_directories(app.data_root);
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-secret-echo";
	chat.title = "Echo chat";
	chat.created_at = "2026-11-12 10:00:00";
	chat.provider_id = "gemini-cli";
	chat.workspace_directory = temp.root.string();
	chat.messages.push_back(Message{MessageRole::User, "Use the token."});
	app.chats.push_back(chat);
	auto owned = std::make_unique<uam::AcpSessionState>();
	owned->chat_id = chat.id;
	owned->provider_id = "gemini-cli";
	owned->running = true;
	owned->initialized = true;
	owned->session_ready = true;
	owned->session_id = "secret-echo-session";
	owned->lifecycle_state = "processing";
	owned->processing = true;
	owned->prompt_request_id = 5;
	owned->turn_user_message_index = 0;
	uam::AcpSessionState& session = *owned;
	app.acp_sessions.push_back(std::move(owned));
	const auto line = [&](const std::string& update)
	{
		return uam::ProcessAcpLineForTests(app, session, app.chats.front(),
		    R"({"jsonrpc":"2.0","method":"session/update","params":{"update":)" + update + "}}");
	};
	const std::string first(kCanary, 10);
	const std::string rest(kCanary + 10);
	// Whole in a tool result, and split across two streamed text deltas.
	UAM_ASSERT(line(R"({"sessionUpdate":"tool_call","toolCallId":"t1","status":"completed","title":"Run gh","kind":"execute","content":[{"type":"content","content":{"type":"text","text":"token=)" + std::string(kCanary) + R"("}}]})"));
	UAM_ASSERT(line(R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"visible-marker )" + first + R"("}})"));
	UAM_ASSERT(line(R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":")" + rest + R"( done"}})"));
	ChatSession& live = app.chats.front();
	const std::string transcript = nlohmann::json(live.messages.back().content).dump() + live.messages.back().thoughts;
	UAM_ASSERT(transcript.find("visible-marker") != std::string::npos);
	for (const Message& message : live.messages)
	{
		UAM_ASSERT(message.content.find(kCanary) == std::string::npos);
		for (const ToolCall& tool : message.tool_calls) UAM_ASSERT(tool.result_text.find(kCanary) == std::string::npos);
	}
	for (const uam::AcpToolCallState& tool : session.tool_calls) UAM_ASSERT(tool.content.find(kCanary) == std::string::npos);

	// Chat file, memory worker input, the full state the companion projects from, and an export bundle.
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, live));
	const std::string memory_input = MemoryService::BuildWorkerPromptForTests(live);
	UAM_ASSERT(memory_input.find(kCanary) == std::string::npos);
	const nlohmann::json state = nlohmann::json::parse(uam::StateUpdateForTests(app));
	UAM_ASSERT(state.dump().find(kCanary) == std::string::npos);
	UAM_ASSERT(uam::companion::MobileState(state).dump().find(kCanary) == std::string::npos);
	const fs::path bundle = temp.root / "export.uambundle";
	UAM_ASSERT(uam::LocalChatBundleService::Export(app.data_root, bundle).ok);
	UAM_ASSERT(Everything(temp.root).find(kCanary) == std::string::npos);
	UAM_ASSERT(Everything(app.data_root).find("visible-marker") != std::string::npos);
	UAM_ASSERT(Everything(bundle).find("visible-marker") != std::string::npos || ReadAll(bundle).find("visible-marker") != std::string::npos);

	// The diagnostic log scrubs a line that carries the value.
	const fs::path log = temp.root / "logs" / "uam.log";
	fs::create_directories(log.parent_path());
	std::string log_error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, log_error, 0));
	uam::diagnostics::Write(std::string("[test] provider said ") + kCanary);
	uam::diagnostics::StopFileLog();
	const std::string written = ReadAll(log);
	UAM_ASSERT(written.find("[redacted secret]") != std::string::npos);
	UAM_ASSERT(written.find(kCanary) == std::string::npos);
	uam::secrets::ResetForTests();
}

UAM_TEST(SecretRedactionKeepsProviderJsonValidForNumericAndEscapedValues)
{
	uam::secrets::ResetForTests();
	// A 6+ digit PIN can also appear inside an unrelated number; a value can end in a backslash.
	UAM_ASSERT(uam::secrets::Register("424242"));
	UAM_ASSERT(uam::secrets::Register("pa55word\\"));
	nlohmann::json message = nlohmann::json::parse(R"({"id":14242424200,"text":"pin 424242 and pa55word\\ end","n":[424242]})");
	UAM_ASSERT(uam::secrets::ScrubJsonStrings(message));
	UAM_ASSERT_EQ(message["id"].get<std::int64_t>(), std::int64_t{14242424200});
	UAM_ASSERT_EQ(message["n"][0].get<int>(), 424242);
	UAM_ASSERT_EQ(message["text"].get<std::string>(), std::string("pin [redacted secret] and [redacted secret] end"));
	// Only the tail is rescanned for a value completed by the newest bytes.
	std::string grown = std::string(5000, 'x') + "4242" + "42";
	UAM_ASSERT(uam::secrets::ScrubTail(grown, 2 + uam::secrets::MaxPatternBytes()));
	UAM_ASSERT(grown.ends_with("[redacted secret]"));

	TempDir temp{"uam-secret-numeric"};
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-numeric";
	chat.provider_id = "gemini-cli";
	chat.workspace_directory = temp.root.string();
	chat.messages.push_back(Message{MessageRole::User, "Pair it."});
	app.chats.push_back(chat);
	auto owned = std::make_unique<uam::AcpSessionState>();
	owned->chat_id = chat.id;
	owned->provider_id = "gemini-cli";
	owned->running = true;
	owned->initialized = true;
	owned->session_ready = true;
	owned->session_id = "numeric-session";
	owned->processing = true;
	owned->prompt_request_id = 5;
	owned->turn_user_message_index = 0;
	uam::AcpSessionState& session = *owned;
	app.acp_sessions.push_back(std::move(owned));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"code 424242 ok"}},"seq":1424242}})"));
	UAM_ASSERT(session.running);
	UAM_ASSERT(session.last_error.find("Invalid JSON") == std::string::npos);
	UAM_ASSERT(app.chats.front().messages.back().content.find("424242") == std::string::npos);
	UAM_ASSERT(app.chats.front().messages.back().content.find("[redacted secret]") != std::string::npos);
	uam::secrets::ResetForTests();
}
