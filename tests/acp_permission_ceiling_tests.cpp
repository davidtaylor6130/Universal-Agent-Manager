#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/structured_framing.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
namespace
{

struct PrivatePermissionSink
{
	uam::AcpSessionState& session;
	fs::path captured;

	PrivatePermissionSink(uam::AcpSessionState& target, const fs::path& root)
	    : session(target), captured(root / "permission-responses.bin")
	{
#if defined(_WIN32)
		// Copy bytes without interpreting either the framing or the permission's command.
		const fs::path script = root / "capture-permission-bytes.ps1";
		UAM_ASSERT(uam::io::WriteTextFile(script,
		    "param([string]$CapturePath)\r\n"
		    "$source = [Console]::OpenStandardInput()\r\n"
		    "$target = [IO.File]::Open($CapturePath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)\r\n"
		    "$buffer = New-Object byte[] 4096\r\n"
		    "try { while (($count = $source.Read($buffer, 0, $buffer.Length)) -gt 0) { $target.Write($buffer, 0, $count); $target.Flush() } } finally { $target.Dispose() }\r\n"));
		const std::vector<std::string> command{"powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script.string(), captured.string()};
#else
		const std::vector<std::string> command{"/bin/sh", "-c", "cat > \"$1\"", "private-permission-sink", captured.string()};
#endif
		std::string error;
		UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, root, command, &error));
	}

	~PrivatePermissionSink()
	{
		auto& process = PlatformServicesFactory::Instance().process_service;
		process.StopStdioProcess(session, true);
		process.CloseStdioProcessHandles(session);
	}

	std::vector<nlohmann::json> ReadThroughResponse(const nlohmann::json& request_id, const std::string& decision = {}) const
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline)
		{
			std::string bytes;
			(void)uam::io::TryReadTextFile(captured, bytes);
			std::vector<nlohmann::json> responses;
			std::size_t consumed = 0;
			bool found = false;
			while (consumed < bytes.size())
			{
				const std::string_view remaining = std::string_view(bytes).substr(consumed);
				const auto frame = uam::structured_framing::Read(remaining);
				UAM_ASSERT(frame.status != uam::structured_framing::Status::Invalid);
				if (frame.status != uam::structured_framing::Status::Complete) break;
				responses.push_back(nlohmann::json::parse(remaining.substr(frame.body_offset, frame.body_bytes)));
				consumed += frame.consumed_bytes;
				const auto& response = responses.back();
				UAM_ASSERT_EQ(response.at("method"), nlohmann::json("session.permissions.handlePendingPermissionRequest"));
				if (response.at("params").at("requestId") == request_id &&
				    (decision.empty() || response.at("params").at("result").at("kind") == decision)) found = true;
			}
			if (found)
			{
				UAM_ASSERT_EQ(consumed, bytes.size());
				return responses;
			}
			std::this_thread::yield();
		}
		UAM_ASSERT(false && "Private permission sink did not receive the ordered response fence.");
		return {};
	}
};

struct PermissionCeilingFixture
{
	TempDir temp{"uam-acp-permission-ceiling"};
	uam::AppState app;
	uam::AcpSessionState* session = nullptr;
	std::unique_ptr<PrivatePermissionSink> sink;

	explicit PermissionCeilingFixture(bool goal_review, bool unrestricted = false, bool ordinary_plan = false)
	{
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession chat;
		chat.id = "private-permission-ceiling";
		chat.provider_id = "copilot-cli";
		chat.execution_host_id = "local";
		chat.approval_mode = ordinary_plan ? "plan" : "default";
		chat.command_safety_tier = "yolo";
		chat.native_session_id = "11111111-1111-4111-8111-111111111111";
		chat.interaction_at = "2026-01-01T00:00:00.000Z";
		chat.messages.push_back({MessageRole::User, "Inspect the private fixture"});
		chat.messages.push_back({MessageRole::Assistant, ""});
		app.chats.push_back(std::move(chat));
		auto owned = std::make_unique<uam::AcpSessionState>();
		session = owned.get();
		session->chat_id = app.chats.front().id;
		session->provider_id = app.chats.front().provider_id;
		session->protocol_kind = "copilot-sdk";
		session->session_id = app.chats.front().native_session_id;
		session->current_mode_id = "plan";
		session->running = session->processing = session->initialized = session->session_ready = true;
		session->turn_serial = 1;
		session->turn_user_message_index = session->turn_first_user_message_index = 0;
		session->interaction_at = app.chats.front().interaction_at;
		session->active_uam_agent_workspace_access = !unrestricted && !goal_review ? "read" : "";
		session->active_uam_agent_id = session->active_uam_agent_workspace_access.empty() ? "" : "private-read-reviewer";
		session->goal_review_turn = !unrestricted && goal_review;
		session->copilot_native_state["protocolVersion"] = 3;
		app.acp_sessions.push_back(std::move(owned));
		sink = std::make_unique<PrivatePermissionSink>(*session, temp.root);
	}

	void Request(int request_id, const std::string& kind, const std::string& command = {}, const std::string& human_required_flag = "mustPrompt")
	{
		nlohmann::json permission = {{"kind", kind}, {"path", (temp.root / "private-output.txt").string()}};
		if (!command.empty()) permission["fullCommandText"] = command;
		if (!human_required_flag.empty()) permission[human_required_flag] = true;
		const nlohmann::json frame = {{"jsonrpc", "2.0"}, {"method", "session.event"}, {"params", {
		    {"sessionId", session->session_id}, {"event", {{"id", "private-permission-" + std::to_string(request_id)}, {"type", "permission.requested"},
		    {"data", {{"requestId", request_id}, {"permissionRequest", permission}, {"resolvedByHook", false}}}}}}}};
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(), frame.dump()));
	}

	bool Resolve(int request_id, const std::string& option, bool cancelled, std::string& error)
	{
		return uam::ResolveAcpPermission(app, session->chat_id, std::to_string(request_id), option, cancelled, &error);
	}
};

void AssertDecision(const nlohmann::json& response, int request_id, const std::string& decision)
{
	UAM_ASSERT_EQ(response.at("params").at("requestId"), nlohmann::json(request_id));
	UAM_ASSERT_EQ(response.at("params").at("result").at("kind"), nlohmann::json(decision));
	UAM_ASSERT_EQ(response.at("params").at("sessionId"), nlohmann::json("11111111-1111-4111-8111-111111111111"));
}

struct PermissionSnapshot
{
	uam::AcpPendingPermissionState pending;
	std::size_t queued;
	std::vector<Message> messages;
	std::string chat_interaction;
	std::string session_interaction;
	nlohmann::json native;

	explicit PermissionSnapshot(const PermissionCeilingFixture& fixture)
	    : pending(fixture.session->pending_permission), queued(fixture.session->queued_permissions.size()),
	      messages(fixture.app.chats.front().messages), chat_interaction(fixture.app.chats.front().interaction_at),
	      session_interaction(fixture.session->interaction_at), native(fixture.session->copilot_native_state)
	{
	}

	void AssertUnchanged(const PermissionCeilingFixture& fixture) const
	{
		const auto& actual = fixture.session->pending_permission;
		UAM_ASSERT(fixture.session->running && fixture.session->waiting_for_permission);
		UAM_ASSERT_EQ(actual.request_id_json, pending.request_id_json);
		UAM_ASSERT_EQ(actual.provider_request_method, pending.provider_request_method);
		UAM_ASSERT_EQ(actual.provider_request_params_json, pending.provider_request_params_json);
		UAM_ASSERT_EQ(actual.kind, pending.kind);
		UAM_ASSERT_EQ(actual.content, pending.content);
		UAM_ASSERT_EQ(actual.options.size(), pending.options.size());
		for (std::size_t index = 0; index < pending.options.size(); ++index)
		{
			UAM_ASSERT_EQ(actual.options[index].id, pending.options[index].id);
			UAM_ASSERT_EQ(actual.options[index].kind, pending.options[index].kind);
		}
		UAM_ASSERT_EQ(fixture.session->queued_permissions.size(), queued);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
		UAM_ASSERT_EQ(fixture.app.chats.front().interaction_at, chat_interaction);
		UAM_ASSERT_EQ(fixture.session->interaction_at, session_interaction);
		UAM_ASSERT_EQ(fixture.session->copilot_native_state, native);
	}
};

bool ManualGrantIsRefused(bool goal_review, const std::string& kind, const std::string& command, const std::string& flag)
{
	PermissionCeilingFixture fixture(goal_review);
	fixture.Request(7, kind, command, flag);
	UAM_ASSERT_EQ(fixture.session->pending_permission.request_id_json, std::string("7"));
	UAM_ASSERT(ProviderRuntimeRegistry::ResolveById(fixture.session->provider_id).AcpPermissionRequiresUserDecision(fixture.session->pending_permission));
	const PermissionSnapshot before(fixture);
	std::string error;
	const bool resolved = fixture.Resolve(7, "allow_once", false, error);
	if (!resolved)
	{
		before.AssertUnchanged(fixture);
		UAM_ASSERT(error.find("read-only") != std::string::npos);
		UAM_ASSERT(fixture.Resolve(7, "reject_once", false, error));
		// This delivered denial fences all earlier writes in the actual FIFO sink.
		const auto responses = fixture.sink->ReadThroughResponse(7, "reject");
		UAM_ASSERT_EQ(responses.size(), std::size_t(1));
		AssertDecision(responses.front(), 7, "reject");
	}
	else
	{
		const auto responses = fixture.sink->ReadThroughResponse(7);
		std::cout << "captured ceiling unexpected manual grant: goal=" << goal_review << " kind=" << kind << " command=" << command << " flag=" << flag << " response=" << responses.front().dump() << '\n';
	}
	UAM_ASSERT(!fs::exists(fixture.temp.root / "private-output.txt"));
	return !resolved;
}

void AssertManualMutationArms(bool goal_review)
{
	std::vector<bool> refused;
	for (const std::string flag : {"mustPrompt", "managedApprovalRequired", "requestSandboxBypass", "requiresUserInteraction"})
	{
		refused.push_back(ManualGrantIsRefused(goal_review, "write", "", flag));
		refused.push_back(ManualGrantIsRefused(goal_review, "shell", "echo hello > private-output.txt", flag));
		refused.push_back(ManualGrantIsRefused(goal_review, "shell", "private-unknown-command", flag));
	}
	UAM_ASSERT_EQ(refused.size(), std::size_t(12));
	UAM_ASSERT(std::ranges::all_of(refused, [](bool value) { return value; }));
}

}

UAM_TEST(AcpManualPermissionCannotOverrideCapturedAgentRead)
{
	AssertManualMutationArms(false);
}

UAM_TEST(AcpManualPermissionCannotOverrideGoalReview)
{
	AssertManualMutationArms(true);
}

UAM_TEST(AcpCapturedReadManualDecisionsPreserveAllowedOperations)
{
	for (bool goal_review : {false, true})
	{
		for (const std::string command : {"", "pwd", "git status"})
		{
			PermissionCeilingFixture fixture(goal_review);
			fixture.Request(7, command.empty() ? "read" : "shell", command);
			std::string error;
			if (command.empty())
			{
				// Copilot does not offer a persistent grant. Never manufacture a native offer.
				const PermissionSnapshot before(fixture);
				UAM_ASSERT(!fixture.Resolve(7, "allow_always", false, error));
				UAM_ASSERT(error.find("not offered") != std::string::npos);
				before.AssertUnchanged(fixture);
			}
			UAM_ASSERT(fixture.Resolve(7, "allow_once", false, error));
			const auto responses = fixture.sink->ReadThroughResponse(7, "approve-once");
			UAM_ASSERT_EQ(responses.size(), std::size_t(1));
			AssertDecision(responses.front(), 7, "approve-once");
		}
		for (bool cancelled : {false, true})
		{
			PermissionCeilingFixture fixture(goal_review);
			fixture.Request(7, "write");
			std::string error;
			UAM_ASSERT(fixture.Resolve(7, cancelled ? "" : "reject_once", cancelled, error));
			const std::string decision = cancelled ? "user-not-available" : "reject";
			const auto responses = fixture.sink->ReadThroughResponse(7, decision);
			UAM_ASSERT_EQ(responses.size(), std::size_t(1));
			AssertDecision(responses.front(), 7, decision);
		}
	}
	for (const std::string command : {"", "echo hello > private-output.txt"})
	{
		PermissionCeilingFixture fixture(false, true);
		fixture.Request(7, command.empty() ? "write" : "shell", command);
		std::string error;
		UAM_ASSERT(fixture.Resolve(7, "allow_once", false, error));
		const auto responses = fixture.sink->ReadThroughResponse(7, "approve-once");
		UAM_ASSERT_EQ(responses.size(), std::size_t(1));
		AssertDecision(responses.front(), 7, "approve-once");
		UAM_ASSERT(!fs::exists(fixture.temp.root / "private-output.txt"));
	}
	PermissionCeilingFixture ordinary_plan(false, true, true);
	ordinary_plan.Request(7, "read");
	std::string error;
	UAM_ASSERT(ordinary_plan.Resolve(7, "allow_once", false, error));
	const auto responses = ordinary_plan.sink->ReadThroughResponse(7, "approve-once");
	UAM_ASSERT_EQ(responses.size(), std::size_t(1));
	AssertDecision(responses.front(), 7, "approve-once");
	UAM_ASSERT_EQ(ordinary_plan.app.chats.front().approval_mode, std::string("plan"));
}

UAM_TEST(AcpRejectedManualGrantPreservesPermissionQueueUntilDenial)
{
	std::vector<bool> refused;
	for (bool goal_review : {false, true})
	{
		PermissionCeilingFixture fixture(goal_review);
		fixture.Request(7, "write");
		fixture.Request(8, "read");
		UAM_ASSERT_EQ(fixture.session->pending_permission.request_id_json, std::string("7"));
		UAM_ASSERT_EQ(fixture.session->queued_permissions.size(), std::size_t(1));
		const PermissionSnapshot before(fixture);
		std::string error;
		const bool resolved = fixture.Resolve(7, "allow_once", false, error);
		refused.push_back(!resolved);
		if (!resolved)
		{
			before.AssertUnchanged(fixture);
			UAM_ASSERT(error.find("read-only") != std::string::npos);
			UAM_ASSERT_EQ(fixture.session->queued_permissions.front().request_id_json, std::string("8"));
			UAM_ASSERT(fixture.Resolve(7, "reject_once", false, error));
		}
		UAM_ASSERT_EQ(fixture.session->pending_permission.request_id_json, std::string("8"));
		UAM_ASSERT(fixture.session->waiting_for_permission);
		UAM_ASSERT(fixture.session->queued_permissions.empty());
		UAM_ASSERT(fixture.Resolve(8, "allow_once", false, error));
		const auto responses = fixture.sink->ReadThroughResponse(8, "approve-once");
		UAM_ASSERT_EQ(responses.size(), std::size_t(2));
		AssertDecision(responses.front(), 7, resolved ? "approve-once" : "reject");
		AssertDecision(responses.back(), 8, "approve-once");
		UAM_ASSERT(!fixture.session->waiting_for_permission && fixture.session->pending_permission.request_id_json.empty());
	}
	UAM_ASSERT_EQ(refused.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(refused, [](bool value) { return value; }));
}

UAM_TEST(AcpCapturedReadAutoPolicyRejectsMutatingReadCommandForms)
{
	std::vector<bool> rejected;
	for (bool goal_review : {false, true})
	{
		for (const std::string command : {"git diff --output=private.txt", "file -C -m private.magic", "Get-Content (New-Item private.txt -ItemType File)"})
		{
			PermissionCeilingFixture fixture(goal_review);
			fixture.Request(7, "shell", command, "");
			const auto responses = fixture.sink->ReadThroughResponse(7);
			UAM_ASSERT_EQ(responses.size(), std::size_t(1));
			const std::string decision = responses.front().at("params").at("result").at("kind");
			const bool denied = decision == "reject" || decision == "user-not-available";
			rejected.push_back(denied);
			std::cout << "captured ceiling automatic decision: goal=" << goal_review << " command=" << command << " decision=" << decision << '\n';
			UAM_ASSERT(!fs::exists(fixture.temp.root / "private.txt"));
			UAM_ASSERT(!fs::exists(fixture.temp.root / "private.magic.mgc"));
		}
		for (const std::string command : {"", "pwd"})
		{
			PermissionCeilingFixture fixture(goal_review);
			fixture.Request(7, command.empty() ? "read" : "shell", command, "");
			const auto responses = fixture.sink->ReadThroughResponse(7, "approve-once");
			UAM_ASSERT_EQ(responses.size(), std::size_t(1));
			AssertDecision(responses.front(), 7, "approve-once");
		}
	}
	UAM_ASSERT_EQ(rejected.size(), std::size_t(6));
	UAM_ASSERT(std::ranges::all_of(rejected, [](bool value) { return value; }));
}
#endif


#if UAM_ENABLE_RUNTIME_COPILOT_CLI
UAM_TEST(AcpPersistedPermissionReplayCannotOverrideCurrentCapturedReadCeiling)
{
	std::vector<bool> safe_replay;
	for (bool goal_review : {false, true})
	{
		PermissionCeilingFixture fixture(false, true);
		// Exercise the SSH response-persistence route with a private local byte sink.
		// This metadata does not start SSH or claim a live remote-provider transition.
		fixture.app.chats.front().execution_host_id = "private-ssh-permission-host";
		std::string error;
		fixture.Request(7, "write");
		UAM_ASSERT(fixture.Resolve(7, "allow_once", false, error));
		const auto original_write = fixture.sink->ReadThroughResponse(7, "approve-once");
		UAM_ASSERT_EQ(original_write.size(), std::size_t(1));
		AssertDecision(original_write.front(), 7, "approve-once");
		fixture.Request(9, "read");
		UAM_ASSERT(fixture.Resolve(9, "allow_once", false, error));
		const auto original_read = fixture.sink->ReadThroughResponse(9, "approve-once");
		UAM_ASSERT_EQ(original_read.size(), std::size_t(2));
		AssertDecision(original_read.back(), 9, "approve-once");
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(saved);
		UAM_ASSERT_EQ(saved->execution_host_id, std::string("private-ssh-permission-host"));
		UAM_ASSERT_EQ(saved->remote_interaction_responses.size(), std::size_t(2));
		for (const auto& [request_id, original] : std::vector<std::pair<std::string, nlohmann::json>>{
		    {"7", original_write.front()}, {"9", original_read.back()}})
		{
			const auto durable = std::ranges::find(saved->remote_interaction_responses, request_id,
			    &uam::AcpRemoteInteractionResponseState::request_id_json);
			UAM_ASSERT(durable != saved->remote_interaction_responses.end());
			UAM_ASSERT_EQ(nlohmann::json::parse(durable->response_json), original);
		}
		// Restore only genuine persisted responses produced by the public resolver.
		// The current captured session restriction is a separate fixture input, not
		// evidence that a particular production agent-change path performs reattachment.
		fixture.app.chats.front() = *saved;
		fixture.sink.reset();
		std::error_code remove_error;
		fs::remove(fixture.temp.root / "permission-responses.bin", remove_error);
		UAM_ASSERT(!remove_error);
		fixture.sink = std::make_unique<PrivatePermissionSink>(*fixture.session, fixture.temp.root);
		fixture.session->running = fixture.session->processing = fixture.session->initialized = fixture.session->session_ready = true;
		fixture.session->goal_review_turn = goal_review;
		fixture.session->active_uam_agent_workspace_access = goal_review ? "" : "read";
		fixture.session->active_uam_agent_id = goal_review ? "" : "private-read-reviewer";
		uam::acp_detail::ResetAcpPendingInteractionState(*fixture.session);
		// Replay the exact original request bodies/IDs. This does not manufacture a
		// request-ID collision or edit either saved native response's decision.
		fixture.Request(7, "write");
		fixture.Request(9, "read");
		// The supported saved read response is the actual FIFO fence for every
		// preceding replay write on this freshly attached private transport.
		const auto replayed = fixture.sink->ReadThroughResponse(9, "approve-once");
		AssertDecision(replayed.back(), 9, "approve-once");
		const bool write_approved = std::ranges::any_of(replayed, [](const nlohmann::json& response)
		{
			return response.at("params").at("requestId") == 7 &&
			    response.at("params").at("result").at("kind") == "approve-once";
		});
		safe_replay.push_back(!write_approved);
		std::cout << "captured ceiling persisted replay: goal=" << goal_review
		          << " writeApproved=" << write_approved << " wire=" << nlohmann::json(replayed).dump() << '\n';
		UAM_ASSERT(!fs::exists(fixture.temp.root / "private-output.txt"));
	}
	UAM_ASSERT_EQ(safe_replay.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(safe_replay, [](bool value) { return value; }));
}
#endif
