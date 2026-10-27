#include "test_harness.h"
#include "app/opencode_native_activity_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include <algorithm>

using namespace uam_test;

namespace
{
	using Json = nlohmann::json;
	struct NativeCompanionFixture
	{
		TempDir temp{"uam-native-child-permission"};
		uam::AppState app;
		std::shared_ptr<std::atomic_int> replies = std::make_shared<std::atomic_int>(0);
		std::shared_ptr<std::atomic_bool> enabled = std::make_shared<std::atomic_bool>(false);
		std::shared_ptr<std::atomic_bool> rejected = std::make_shared<std::atomic_bool>(false);
		std::shared_ptr<std::atomic_int> queries = std::make_shared<std::atomic_int>(0);
		std::shared_ptr<std::atomic_int> policy_updates = std::make_shared<std::atomic_int>(0);
		std::shared_ptr<std::atomic_bool> has_permission = std::make_shared<std::atomic_bool>(true);
		std::shared_ptr<std::atomic_bool> foreign_permission = std::make_shared<std::atomic_bool>(false);
		std::shared_ptr<std::atomic_int> failures_left = std::make_shared<std::atomic_int>(0);
		std::shared_ptr<std::atomic_bool> fail_replies = std::make_shared<std::atomic_bool>(false);
		std::shared_ptr<std::atomic_bool> wrong_root = std::make_shared<std::atomic_bool>(false);
		std::shared_ptr<std::atomic_bool> many_permissions = std::make_shared<std::atomic_bool>(false);
		std::shared_ptr<uam::OpenCodeNativeActivityService> service;
		explicit NativeCompanionFixture(const std::string& permission_kind = "edit", bool mcp_doom_loop = false,
		    const std::string& expected_workspace = {}, const std::string& native_workspace = {})
		{
			app.data_root = temp.root;
			ChatSession chat;
			chat.id = "owned-chat"; chat.provider_id = "opencode-cli"; chat.native_session_id = "ses_root";
			chat.workspace_directory = expected_workspace.empty() ? temp.root.string() : expected_workspace; chat.command_safety_tier = "default";
			app.chats.push_back(chat);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = chat.id; session->provider_id = chat.provider_id; session->session_id = chat.native_session_id;
			session->running = session->session_ready = session->initialized = true;
			session->last_process_id = "owned-process"; session->process_execution_host_id = "local";
			session->opencode_native_companion = uam::OpenCodeNativeActivityService::CreateCompanion({49444, "fixture-private-password"});
			app.acp_sessions.push_back(std::move(session));
			const auto enabled_copy = enabled; const auto replies_copy = replies; const auto rejected_copy = rejected;
			const auto queries_copy = queries; const auto has_permission_copy = has_permission;
			const auto policy_updates_copy = policy_updates;
			const auto foreign_permission_copy = foreign_permission;
			const auto failures_copy = failures_left; const auto fail_replies_copy = fail_replies;
			const auto wrong_root_copy = wrong_root; const auto many_permissions_copy = many_permissions;
			const std::string workspace = native_workspace.empty() ? chat.workspace_directory : native_workspace;
			service = std::make_shared<uam::OpenCodeNativeActivityService>([enabled_copy, replies_copy, rejected_copy, queries_copy, policy_updates_copy, has_permission_copy, foreign_permission_copy, failures_copy, fail_replies_copy, wrong_root_copy, many_permissions_copy, workspace, permission_kind, mcp_doom_loop](const uam::OpenCodeNativeActivityService::Query& query, Json& result, std::string& error, std::stop_token)
			{
				++*queries_copy;
				const std::string operation = query.operation.value("operation", "");
				if (operation == (fail_replies_copy->load() ? "reply" : "permissions") && failures_copy->load() > 0)
				{ --*failures_copy; error = "Simulated transient native HTTP failure."; return false; }
				if (operation == "enableTask") { ++*policy_updates_copy; enabled_copy->store(true); result = Json::object(); return true; }
				if (operation == "disableTask") { ++*policy_updates_copy; enabled_copy->store(false); result = Json::object(); return true; }
				if (operation == "status") { result = Json::object(); return true; }
				if (operation == "session")
				{
					const auto id = query.operation.value("nativeSessionId", "");
					result = {{"id", id}, {"directory", workspace}};
					if (id == "ses_root" && wrong_root_copy->load()) result["id"] = "ses_wrong";
					if (id.starts_with("ses_child_")) result["parentID"] = "ses_parent_" + id.substr(10);
					else if (id == "ses_foreign") result["parentID"] = "ses_foreign_root";
					else if (id != "ses_root" && id != "ses_foreign_root") result["parentID"] = "ses_root";
					result["permission"] = Json::array({{{"permission", "task"}, {"pattern", "*"}, {"action", enabled_copy->load() ? "ask" : "deny"}}});
					return true;
				}
				if (operation == "permissions")
				{
					if (many_permissions_copy->load())
					{
						result = Json::array();
						for (int index = 0; index < 64; ++index)
							result.push_back({{"id", "per_" + std::to_string(index)}, {"sessionID", "ses_child_" + std::to_string(index)}, {"permission", "read"}, {"patterns", {"src/file.cpp"}}, {"metadata", Json::object()}});
						return true;
					}
					result = replies_copy->load() || !has_permission_copy->load() ? Json::array() : Json::array({{{"id", "per_child"}, {"sessionID", foreign_permission_copy->load() ? "ses_foreign" : "ses_child"}, {"permission", permission_kind}, {"patterns", {"src/file.cpp"}}, {"metadata", {{"file", "src/file.cpp"}}}}});
					if (permission_kind == "doom_loop" && !result.empty())
					{
						// Official v1.18.35 built-in processor vs MCP tool-name collision.
						auto& permission = result.front();
						permission["patterns"] = permission["always"] = Json::array({mcp_doom_loop ? "*" : "read"});
						permission["metadata"] = mcp_doom_loop ? Json::object() : Json{{"tool", "read"}, {"input", {{"filePath", "private-fixture.txt"}}}};
						if (mcp_doom_loop) permission["tool"] = {{"messageID", "msg_private"}, {"callID", "call_private"}};
					}
					return true;
				}
				if (operation == "reply")
				{
					rejected_copy->store(query.operation.value("reply", "") == "reject");
					++*replies_copy; result = true; return true;
				}
				return false;
			});
		}
		uam::AcpSessionState& Session() { return *app.acp_sessions.front(); }
		void Remote(const std::string& platform = {})
		{
			ExecutionHost host;
			host.id = "owned-ssh"; host.transport = "ssh"; host.ssh_alias = "fixture-native";
#if defined(_WIN32)
			host.platform = "windows";
#elif defined(__APPLE__)
			host.platform = "macos";
#else
			host.platform = "linux";
#endif
			if (!platform.empty()) host.platform = platform;
			host.runner_protocol_version = 3; host.runner_status = "ready";
			app.settings.execution_hosts.push_back(host);
			ChatSession& chat = app.chats.front();
			chat.execution_host_id = host.id; chat.remote_process_exists = true;
			chat.remote_process_control_token = "fixture-private-process-control";
			chat.remote_opencode_native_port = Session().opencode_native_companion->endpoint.port;
			chat.remote_opencode_native_token = Session().opencode_native_companion->endpoint.token;
			Session().process_execution_host_id = host.id;
			Session().remote_output_delivery_token = chat.remote_process_control_token;
		}
		static uam::AcpRemoteInteractionResponseState Decision(const std::string& id)
		{
			const Json request_id = id;
			return {request_id.dump(), Json({{"jsonrpc", "2.0"}, {"id", request_id}, {"result", {{"outcome", {{"outcome", "selected"}, {"optionId", "allow-once"}}}}}}).dump()};
		}
		void Until(const std::function<bool()>& condition)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!condition() && std::chrono::steady_clock::now() < deadline)
			{
				service->Poll(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			UAM_ASSERT(condition());
		}
	};
}

UAM_TEST(OpenCodeNativeCompanionRequiresExactWorkspaceAndAcyclicAncestry)
{
	Json sessions = {{"ses_root", {{"id", "ses_root"}, {"directory", "/owned"}}}, {"ses_child", {{"id", "ses_child"}, {"directory", "/owned"}, {"parentID", "ses_root"}}}};
	UAM_ASSERT(uam::OpenCodeNativeActivityService::OwnsSession("ses_root", "/owned", "ses_child", sessions));
	sessions["ses_child"]["directory"] = "/other";
	UAM_ASSERT(!uam::OpenCodeNativeActivityService::OwnsSession("ses_root", "/owned", "ses_child", sessions));
	sessions["ses_child"]["directory"] = "/owned"; sessions["ses_child"]["parentID"] = "ses_child";
	UAM_ASSERT(!uam::OpenCodeNativeActivityService::OwnsSession("ses_root", "/owned", "ses_child", sessions));
	sessions["ses_child"]["parentID"] = "ses_missing";
	UAM_ASSERT(!uam::OpenCodeNativeActivityService::OwnsSession("ses_root", "/owned", "ses_child", sessions));
}

UAM_TEST(OpenCodeNativeCompanionRequiresConfirmedTaskAskRule)
{
	UAM_ASSERT(!uam::OpenCodeNativeActivityService::TaskApprovalEnabled(Json::object()));
	Json session = {{"permission", Json::array({{{"permission", "task"}, {"pattern", "*"}, {"action", "ask"}}})}};
	UAM_ASSERT(uam::OpenCodeNativeActivityService::TaskApprovalEnabled(session));
	session["permission"].push_back({{"permission", "task"}, {"pattern", "*"}, {"action", "deny"}});
	UAM_ASSERT(!uam::OpenCodeNativeActivityService::TaskApprovalEnabled(session));
}

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
UAM_TEST(OpenCodeNativeCompanionRetriesTransientObservationAndResetsFailureCount)
{
	NativeCompanionFixture fixture;
	fixture.has_permission->store(false);
	std::string error;
	fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
	for (int recovery = 0; recovery < 2; ++recovery)
	{
		fixture.failures_left->store(recovery == 0 ? 2 : 1);
		fixture.Session().processing = true;
		fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Pending; });
		UAM_ASSERT(fixture.Session().running);
		UAM_ASSERT(fixture.Session().pending_permission.request_id_json.empty());
		fixture.Session().pending_permission.request_id_json = "77";
		fixture.Session().pending_permission.title = "task";
		fixture.Session().pending_permission.options = {{"allow-once", "Allow once", "allow_once"}, {"reject-once", "Reject", "reject_once"}};
		Json allow = {{"id", 77}, {"result", {{"outcome", {{"outcome", "selected"}, {"optionId", "allow-once"}}}}}};
		const auto held = uam::OpenCodeNativeActivityService::TryWritePermissionResponse(fixture.Session(), allow, &error);
		UAM_ASSERT(held.has_value() && !*held);
		allow["result"]["outcome"]["optionId"] = "reject-once";
		UAM_ASSERT(!uam::OpenCodeNativeActivityService::TryWritePermissionResponse(fixture.Session(), allow, &error).has_value());
		fixture.Session().pending_permission = {};
		fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
		UAM_ASSERT(fixture.Session().running);
		UAM_ASSERT(fixture.Session().last_error.empty());
	}
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionRetainsDurableDecisionAcrossTransientReplyFailure)
{
	NativeCompanionFixture fixture;
	fixture.Remote();
	fixture.Until([&] { return !fixture.Session().pending_permission.request_id_json.empty(); });
	fixture.fail_replies->store(true);
	fixture.failures_left->store(1);
	std::string error;
	UAM_ASSERT(uam::ResolveAcpPermission(fixture.app, fixture.Session().chat_id, fixture.Session().pending_permission.request_id_json, "allow-once", false, &error));
	uam::acp_detail::CompletePromptTurn(fixture.Session(), "ready");
	fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Pending; });
	UAM_ASSERT(fixture.Session().running);
	UAM_ASSERT_EQ(fixture.replies->load(), 0);
	UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.size(), std::size_t{1});
	fixture.Until([&] { return fixture.app.chats.front().remote_interaction_responses.empty(); });
	UAM_ASSERT(fixture.Session().running);
	UAM_ASSERT_EQ(fixture.replies->load(), 1);
	UAM_ASSERT(!fixture.rejected->load());
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionResumesOnlyExactAdmittedRootDecisionAfterRetry)
{
	for (const bool replacement : {false, true})
	{
		NativeCompanionFixture fixture;
		fixture.Remote();
		fixture.has_permission->store(false);
		auto& process = PlatformServicesFactory::Instance().process_service;
		std::string error;
#if defined(_WIN32)
		const std::vector<std::string> sink = {"cmd.exe", "/d", "/s", "/c", "more > root-task-wire.jsonl"};
#else
		const std::vector<std::string> sink = {"/bin/sh", "-c", "cat > root-task-wire.jsonl"};
#endif
		UAM_ASSERT(process.StartStdioProcess(fixture.Session(), fixture.temp.root, sink, &error));
		struct StopSink
		{
			IPlatformProcessService& process;
			uam::AcpSessionState& session;
			~StopSink() { process.StopStdioProcess(session, true); process.CloseStdioProcessHandles(session); }
		} stop_sink{process, fixture.Session()};
		fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
		fixture.Session().processing = true;
		fixture.failures_left->store(1);
		fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Pending; });
		uam::acp_detail::HandlePermissionRequest(fixture.app, fixture.Session(), fixture.app.chats.front(),
		    {{"jsonrpc", "2.0"}, {"id", 77}, {"method", "session/request_permission"}, {"params", {
		        {"toolCall", {{"toolCallId", "root-task"}, {"title", "task"}, {"kind", "other"}, {"status", "pending"}}},
		        {"options", Json::array({{{"optionId", "allow-once"}, {"name", "Allow once"}, {"kind", "allow_once"}}, {{"optionId", "reject-once"}, {"name", "Reject"}, {"kind", "reject_once"}}})}}}});
		UAM_ASSERT(!uam::ResolveAcpPermission(fixture.app, fixture.Session().chat_id, "77", "allow-once", false, &error));
		UAM_ASSERT(fixture.Session().waiting_for_permission);
		UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.size(), std::size_t{1});
		if (replacement) fixture.Session().pending_permission.request_id_json = "78";
		fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
		const fs::path wire = fixture.temp.root / "root-task-wire.jsonl";
		if (replacement)
		{
			UAM_ASSERT(fixture.Session().waiting_for_permission);
			UAM_ASSERT_EQ(fixture.Session().pending_permission.request_id_json, std::string("78"));
			UAM_ASSERT(uam::io::ReadTextFile(wire).empty());
		}
		else
		{
			fixture.Until([&] { return !uam::io::ReadTextFile(wire).empty(); });
			UAM_ASSERT(!fixture.Session().waiting_for_permission);
			const std::string written = uam::io::ReadTextFile(wire);
			UAM_ASSERT_EQ(std::count(written.begin(), written.end(), '\n'), 1);
			const Json response = Json::parse(written);
			UAM_ASSERT_EQ(response["id"], Json(77));
			UAM_ASSERT_EQ(response["result"]["outcome"]["optionId"], Json("allow-once"));
		}
		UAM_ASSERT_EQ(fixture.replies->load(), 0);
		fixture.service->Stop();
	}
}

UAM_TEST(OpenCodeNativeCompanionStopsAfterConsecutiveObservationFailures)
{
	NativeCompanionFixture fixture;
	fixture.has_permission->store(false);
	std::string error;
	fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
	fixture.failures_left->store(3);
	fixture.Session().processing = true;
	fixture.Until([&] { return !fixture.Session().running; });
	UAM_ASSERT_EQ(fixture.failures_left->load(), 0);
	UAM_ASSERT(fixture.Session().last_error.find("Simulated transient native HTTP failure") != std::string::npos);
	UAM_ASSERT(std::ranges::any_of(fixture.Session().diagnostics, [](const auto& entry) { return entry.event == "native_companion" && entry.reason == "observation_failures" && entry.detail == "failures=3"; }));
	UAM_ASSERT_EQ(fixture.replies->load(), 0);
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionRejectsConfirmedRootConflictImmediately)
{
	NativeCompanionFixture fixture;
	fixture.has_permission->store(false);
	std::string error;
	fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
	const int before = fixture.queries->load();
	fixture.wrong_root->store(true);
	fixture.Session().processing = true;
	fixture.Until([&] { return !fixture.Session().running; });
	UAM_ASSERT_EQ(fixture.queries->load(), before + 1);
	UAM_ASSERT(std::ranges::any_of(fixture.Session().diagnostics, [](const auto& entry) { return entry.event == "native_companion" && entry.reason == "ownership_conflict"; }));
	UAM_ASSERT_EQ(fixture.replies->load(), 0);
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionQueryBudgetCoversBoundedPermissionAncestry)
{
	NativeCompanionFixture fixture;
	fixture.many_permissions->store(true);
	fixture.Until([&] { return !fixture.Session().pending_permission.request_id_json.empty(); });
	UAM_ASSERT(fixture.Session().running);
	UAM_ASSERT_EQ(fixture.Session().queued_permissions.size(), std::size_t{63});
	UAM_ASSERT_EQ(fixture.queries->load(), 132);
	UAM_ASSERT_EQ(fixture.replies->load(), 0);
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionRetiresDeliveredDecisionAfterControllerRestart)
{
	NativeCompanionFixture fixture;
	fixture.Remote();
	fixture.has_permission->store(false);
	fixture.app.chats.front().remote_interaction_responses = {NativeCompanionFixture::Decision("opencode-native:per_child"), NativeCompanionFixture::Decision("different-native:request")};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front()));
	const auto restored = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(restored.has_value());
	fixture.app.chats.front() = *restored;
	fixture.Until([&] { return fixture.app.chats.front().remote_interaction_responses.size() == 1; });
	UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.front().request_id_json, Json("different-native:request").dump());
	UAM_ASSERT_EQ(fixture.replies->load(), 0);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->remote_interaction_responses.size(), std::size_t{1});
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionRetriesReceiptRetirementAfterSaveFailure)
{
	NativeCompanionFixture fixture;
	fixture.Remote();
	fixture.Until([&] { return !fixture.Session().pending_permission.request_id_json.empty(); });
	std::string error;
	UAM_ASSERT(uam::ResolveAcpPermission(fixture.app, fixture.Session().chat_id, fixture.Session().pending_permission.request_id_json, "allow-once", false, &error));
	UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.size(), std::size_t{1});
	UAM_ASSERT(!fixture.Session().waiting_for_permission);
	// Permission waits mark the parent processing; its native completion is a
	// separate receipt. Settle that turn before exercising idle HTTP cleanup.
	uam::acp_detail::CompletePromptTurn(fixture.Session(), "ready");
	UAM_ASSERT(!fixture.Session().processing && !fixture.Session().waiting_for_permission);
	const fs::path blocked = fixture.temp.root / "blocked-save";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "not a data directory"));
	fixture.app.data_root = blocked;
	fixture.app.pending_chat_save_at_by_chat_id.clear();
	fixture.Until([&] { return fixture.replies->load() == 1 && fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.Session().chat_id); });
	UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.size(), std::size_t{1});
	UAM_ASSERT(!fixture.Session().processing && !fixture.Session().waiting_for_permission);
	fixture.app.data_root = fixture.temp.root;
	fixture.Until([&] { return fixture.app.chats.front().remote_interaction_responses.empty(); });
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT(saved->remote_interaction_responses.empty());
	UAM_ASSERT_EQ(fixture.replies->load(), 1);
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionPreservesForeignDecisionsWithoutIdlePolling)
{
	NativeCompanionFixture fixture;
	fixture.Remote();
	fixture.foreign_permission->store(true);
	fixture.app.chats.front().remote_interaction_responses = {NativeCompanionFixture::Decision("opencode-native:per_child"), NativeCompanionFixture::Decision("different-native:request")};
	std::string error;
	fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
	UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.size(), std::size_t{2});
	UAM_ASSERT(fixture.Session().pending_permission.request_id_json.empty());
	const int queries = fixture.queries->load();
	for (int index = 0; index < 100; ++index)
	{
		fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(6));
	}
	UAM_ASSERT_EQ(fixture.queries->load(), queries);
	UAM_ASSERT_EQ(fixture.replies->load(), 0);
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionPreservesDecisionsWhenSavedProcessAuthorityDiffers)
{
	for (const std::string field : {"session", "control", "endpoint"})
	{
		NativeCompanionFixture fixture;
		fixture.Remote();
		fixture.has_permission->store(false);
		fixture.app.chats.front().remote_interaction_responses = {NativeCompanionFixture::Decision("opencode-native:per_child")};
		if (field == "session") fixture.app.chats.front().native_session_id = "ses_other";
		if (field == "control") fixture.app.chats.front().remote_process_control_token = "different-private-process-control";
		if (field == "endpoint") ++fixture.app.chats.front().remote_opencode_native_port;
		std::string error;
		fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
		UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.size(), std::size_t{1});
		fixture.service->Stop();
	}
}

UAM_TEST(OpenCodeNativeCompanionSleepsWhenIdleAndDeliversRepliesAfterParentSettles)
{
	NativeCompanionFixture fixture;
	fixture.has_permission->store(false);
	std::string error;
	fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
	const int bootstrap_queries = fixture.queries->load();
	UAM_ASSERT_EQ(bootstrap_queries, 4);
	// Cross the observation interval while exercising many ordinary application polls.
	for (int index = 0; index < 100; ++index)
	{
		fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(6));
	}
	UAM_ASSERT_EQ(fixture.queries->load(), bootstrap_queries);
	fixture.has_permission->store(true);
	fixture.Session().processing = true;
	fixture.Until([&] { return !fixture.Session().pending_permission.request_id_json.empty(); });
	UAM_ASSERT(fixture.queries->load() > bootstrap_queries);
	fixture.Session().processing = false;
	UAM_ASSERT(uam::ResolveAcpPermission(fixture.app, fixture.Session().chat_id, fixture.Session().pending_permission.request_id_json, "allow-once", false, &error));
	UAM_ASSERT(!fixture.Session().waiting_for_permission);
	fixture.Until([&] { return fixture.replies->load() == 1; });
	UAM_ASSERT(!fixture.rejected->load());
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionGatesPromptAndRoutesChildApprovalOnce)
{
	NativeCompanionFixture fixture;
	std::string error;
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Pending);
	UAM_ASSERT(!fixture.enabled->load());
	fixture.Until([&] { return !fixture.Session().pending_permission.request_id_json.empty(); });
	UAM_ASSERT(fixture.enabled->load());
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready);
	UAM_ASSERT_EQ(fixture.Session().pending_permission.provider_request_method, std::string("opencode/nativePermission"));
	UAM_ASSERT_EQ(fixture.Session().queued_permissions.size(), std::size_t{0});
	Json response = {{"id", "opencode-native:per_child"}, {"result", {{"outcome", {{"outcome", "selected"}, {"optionId", "allow-once"}}}}}};
	const auto accepted = uam::OpenCodeNativeActivityService::TryWritePermissionResponse(fixture.Session(), response, &error);
	UAM_ASSERT(accepted.has_value() && *accepted);
	fixture.Until([&] { return fixture.replies->load() == 1; });
	UAM_ASSERT(!fixture.rejected->load());
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionObservesPendingChildWithoutNativeIdentity)
{
	NativeCompanionFixture fixture;
	fixture.has_permission->store(false);
	std::string error;
	fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Ready; });
	const int bootstrap_queries = fixture.queries->load();
	uam::AcpToolCallState tool;
	tool.id = "unresolved-child-tool";
	tool.is_sub_agent = true;
	tool.status = "pending";
	fixture.Session().tool_calls.push_back(tool);
	UAM_ASSERT(!fixture.Session().processing);
	UAM_ASSERT(fixture.Session().opencode_child_activity_by_tool.empty());
	fixture.Until([&] { return fixture.queries->load() >= bootstrap_queries + 2; });
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionPlanTransitionDistinguishesStagedAndSentTurns)
{
	NativeCompanionFixture fixture;
	fixture.Until([&] { return !fixture.Session().pending_permission.request_id_json.empty(); });
	fixture.app.chats.front().approval_mode = "plan";
	fixture.Session().processing = true;
	fixture.Session().prompt_request_id = 0;
	std::string error;
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::Pending);
	fixture.service->Stop();

	NativeCompanionFixture active;
	active.Until([&] { return !active.Session().pending_permission.request_id_json.empty(); });
	active.app.chats.front().approval_mode = "plan";
	active.Session().prompt_request_id = 42;
	UAM_ASSERT(active.service->BeforeDispatch(active.app, active.Session(), active.app.chats.front(), error) == uam::OpenCodeNativeGateState::Failed);
	UAM_ASSERT(!error.empty());
	active.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionReadOnlyRejectsChildWrites)
{
	NativeCompanionFixture fixture;
	fixture.app.chats.front().approval_mode = "plan";
	fixture.Until([&] { return fixture.replies->load() == 1; });
	UAM_ASSERT(fixture.rejected->load());
	UAM_ASSERT(fixture.Session().pending_permission.request_id_json.empty());
	fixture.service->Stop();
}

UAM_TEST(OpenCodeNativeCompanionCapturedReadKeepsDoomLoopQuestionManualAndDeliverable)
{
	std::vector<bool> accepted;
	for (bool goal_review : {false, true})
	{
		NativeCompanionFixture fixture("doom_loop");
		fixture.app.chats.front().command_safety_tier = "off";
		fixture.Session().goal_review_turn = goal_review;
		fixture.Session().active_uam_agent_workspace_access = goal_review ? "" : "read";
		fixture.Until([&] { return fixture.Session().waiting_for_permission || fixture.replies->load() != 0; });
		UAM_ASSERT(fixture.Session().waiting_for_permission);
		UAM_ASSERT_EQ(fixture.replies->load(), 0);
		UAM_ASSERT_EQ(fixture.Session().pending_permission.provider_request_method, std::string("opencode/nativePermission"));
		UAM_ASSERT_EQ(fixture.Session().pending_permission.title, std::string("doom_loop"));
		UAM_ASSERT_EQ(fixture.Session().pending_permission.kind, std::string("other"));
		const std::string request_id = fixture.Session().pending_permission.request_id_json;
		std::string error;
		const bool delivered = uam::ResolveAcpPermission(fixture.app, fixture.Session().chat_id, request_id, "allow-once", false, &error);
		accepted.push_back(delivered);
		if (!delivered)
		{
			UAM_ASSERT_EQ(fixture.Session().pending_permission.request_id_json, request_id);
			UAM_ASSERT_EQ(fixture.replies->load(), 0);
			UAM_ASSERT(uam::ResolveAcpPermission(fixture.app, fixture.Session().chat_id, request_id, "reject-once", false, &error));
		}
		fixture.Until([&] { return fixture.replies->load() == 1; });
		UAM_ASSERT_EQ(fixture.rejected->load(), !delivered);
		UAM_ASSERT(!fixture.Session().waiting_for_permission);
		fixture.service->Stop();
	}
	UAM_ASSERT_EQ(accepted.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(accepted, [](bool value) { return value; }));
}


UAM_TEST(OpenCodeNativeCompanionCapturedReadRejectsMcpDoomLoopAction)
{
	// Native MCP server doom + tool loop has permission doom_loop, not the
	// built-in repeat question. Exercise the actual HTTP snapshot -> ACP relay
	// -> public decision -> final posted HTTP reply, under both captured ceilings.
	std::vector<bool> refused;
	for (bool goal_review : {false, true})
	{
		NativeCompanionFixture fixture("doom_loop", true);
		fixture.app.chats.front().command_safety_tier = "off";
		fixture.Session().goal_review_turn = goal_review;
		fixture.Session().active_uam_agent_workspace_access = goal_review ? "" : "read";
		fixture.Until([&] { return fixture.Session().waiting_for_permission || fixture.replies->load() != 0; });
		UAM_ASSERT(fixture.Session().waiting_for_permission);
		UAM_ASSERT_EQ(fixture.replies->load(), 0);
		const std::string request_id = fixture.Session().pending_permission.request_id_json;
		const std::string original_params = fixture.Session().pending_permission.provider_request_params_json;
		UAM_ASSERT_EQ(fixture.Session().pending_permission.provider_request_method, std::string("opencode/nativePermission"));
		std::string error;
		const bool accepted = uam::ResolveAcpPermission(fixture.app, fixture.Session().chat_id, request_id, "allow-once", false, &error);
		refused.push_back(!accepted);
		if (!accepted)
		{
			UAM_ASSERT(!error.empty());
			UAM_ASSERT_EQ(fixture.Session().pending_permission.request_id_json, request_id);
			UAM_ASSERT_EQ(fixture.Session().pending_permission.provider_request_params_json, original_params);
			UAM_ASSERT_EQ(fixture.replies->load(), 0);
			UAM_ASSERT(uam::ResolveAcpPermission(fixture.app, fixture.Session().chat_id, request_id, "reject-once", false, &error));
		}
		// Await the actual posted reply as the delivery fence, not a quiet poll.
		fixture.Until([&] { return fixture.replies->load() == 1; });
		UAM_ASSERT_EQ(fixture.rejected->load(), !accepted);
		UAM_ASSERT(!fixture.Session().waiting_for_permission);
		fixture.service->Stop();
	}
	UAM_ASSERT_EQ(refused.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(refused, [](bool value) { return value; }));
}

UAM_TEST(OpenCodeNativeCompanionRejectsForeignRepliesAndExcludedAgents)
{
	NativeCompanionFixture fixture;
	std::string error;
	Json response = {{"id", "opencode-native:per_foreign"}, {"result", {{"outcome", {{"outcome", "selected"}, {"optionId", "allow-once"}}}}}};
	const auto denied = uam::OpenCodeNativeActivityService::TryWritePermissionResponse(fixture.Session(), response, &error);
	UAM_ASSERT(denied.has_value() && !*denied);
	UAM_ASSERT(!error.empty());
	fixture.Session().active_uam_agent_execution_capability = "opencode-native-agent-config";
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == uam::OpenCodeNativeGateState::NotApplicable);
	UAM_ASSERT(!uam::OpenCodeNativeActivityService::TryWritePermissionResponse(fixture.Session(), {{"id", 42}}, &error).has_value());
}

UAM_TEST(OpenCodeNativeCompanionCancellationDoesNotJoinBlockedWorker)
{
	NativeCompanionFixture fixture;
	auto started = std::make_shared<std::atomic_bool>(false);
	auto cancelled = std::make_shared<std::atomic_bool>(false);
	fixture.service = std::make_shared<uam::OpenCodeNativeActivityService>([started, cancelled](const uam::OpenCodeNativeActivityService::Query&, Json&, std::string&, std::stop_token stop)
	{
		started->store(true);
		while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		cancelled->store(true); return false;
	});
	fixture.Until([&] { return started->load(); });
	const auto before = std::chrono::steady_clock::now();
	fixture.service->Stop();
	UAM_ASSERT(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!cancelled->load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	UAM_ASSERT(cancelled->load());
}
#endif


UAM_TEST(OpenCodeNativeCompanionWindowsWorkspaceGrammarKeepsExactIdentity)
{
	struct Case { std::string expected, actual; bool accepted; };
	const std::vector<Case> cases{
	    {"C:\\Owned\\Workspace", "C:/Owned/Workspace", true},
	    {"C:/Owned\\Workspace\\", "C:\\Owned/Workspace/", true},
	    {"C:\\", "C:/", true},
	    {"C:\\Owned Workspace\\data", "C:/Owned Workspace/data", true},
	    {"C:\\Owned\\Workspace", "c:/Owned/Workspace", false},
	    {"C:\\Owned\\Workspace", "C:/owned/Workspace", false},
	    {"C:\\Owned\\Workspace", "C:/Owned/OtherRoot", false},
	    {"C:\\Owned\\Workspace", "C:/Owned/Workspace/", false},
	    {"C:\\Owned\\.\\Workspace", "C:/Owned/./Workspace", false},
	    {"C:\\Owned\\..\\Workspace", "C:/Owned/../Workspace", false},
	    {"C:\\Owned\\\\Workspace", "C:/Owned//Workspace", false},
	    {"C:\\Owned\\Workspace.", "C:/Owned/Workspace.", false},
	    {"C:\\Owned\\Workspace ", "C:/Owned/Workspace ", false},
	    {"C:\\Owned\\data:stream", "C:/Owned/data:stream", false},
	    {"C:\\Owned\\CON.txt", "C:/Owned/CON.txt", false},
	    {"C:\\Owned\\com1", "C:/Owned/com1", false},
	    {"C:\\Owned\\LPT\xc2\xb9.log", "C:/Owned/LPT\xc2\xb9.log", false},
	    {"C:\\Owned\\NUL ", "C:/Owned/NUL ", false},
	    {"\\\\server\\share\\Owned", "//server/share/Owned", false},
	    {"\\\\?\\C:\\Owned", "//?/C:/Owned", false},
	    {"\\\\.\\C:\\Owned", "//./C:/Owned", false},
	    {"C:Owned\\Workspace", "C:Owned/Workspace", false},
	    {"Owned\\Workspace", "Owned/Workspace", false},
	    {std::string("C:\\Owned\\nul\0tail", 17), std::string("C:/Owned/nul\0tail", 17), false},
	    {"C:\\Owned\\line\nname", "C:/Owned/line\nname", false},
	    {"C:\\Owned\\" + std::string(4096, 'x'), "C:/Owned/" + std::string(4096, 'x'), false},
	};
	for (const auto& item : cases)
	{
		const Json sessions{{"ses_root", {{"id", "ses_root"}, {"directory", item.actual}}}};
		UAM_ASSERT_EQ(uam::OpenCodeNativeActivityService::OwnsSession("ses_root", item.expected, "ses_root", sessions, "windows"), item.accepted);
		UAM_ASSERT(!uam::OpenCodeNativeActivityService::OwnsSession("ses_root", item.expected, "ses_root", sessions));
		UAM_ASSERT(!uam::OpenCodeNativeActivityService::OwnsSession("ses_root", item.expected, "ses_root", sessions, "linux"));
	}
	// Exact byte matches retain their prior admission, even outside the additional DOS grammar.
	for (const std::string exact : {std::string("\\\\server\\share\\Owned"), std::string("C:\\Owned\\.\\Workspace")})
	{
		Json sessions{{"ses_root", {{"id", "ses_root"}, {"directory", exact}}}};
		UAM_ASSERT(uam::OpenCodeNativeActivityService::OwnsSession("ses_root", exact, "ses_root", sessions, "windows"));
		sessions["ses_root"]["id"] = "ses_foreign";
		UAM_ASSERT(!uam::OpenCodeNativeActivityService::OwnsSession("ses_root", exact, "ses_root", sessions, "windows"));
	}
	Json ancestry{{"ses_root", {{"id", "ses_root"}, {"directory", "C:/Owned/Workspace"}}},
	    {"ses_child", {{"id", "ses_child"}, {"directory", "C:\\Owned/Workspace"}, {"parentID", "ses_root"}}}};
	UAM_ASSERT(uam::OpenCodeNativeActivityService::OwnsSession("ses_root", "C:\\Owned\\Workspace", "ses_child", ancestry, "Windows"));
	ancestry["ses_child"]["parentID"] = "ses_foreign";
	UAM_ASSERT(!uam::OpenCodeNativeActivityService::OwnsSession("ses_root", "C:\\Owned\\Workspace", "ses_child", ancestry, "windows"));
}

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
UAM_TEST(OpenCodeNativeCompanionWindowsSeparatorSnapshotPreservesReadCeiling)
{
	for (const bool foreign : {false, true})
	{
		NativeCompanionFixture fixture("read", false, "C:\\Owned\\Workspace", foreign ? "C:/Owned/OtherRoot" : "C:/Owned/Workspace");
		fixture.Remote("windows");
		fixture.has_permission->store(false);
		fixture.Session().active_uam_agent_workspace_access = "read";
		fixture.app.chats.front().approval_mode = "plan";
		std::string error;
		const auto expected = foreign ? uam::OpenCodeNativeGateState::Failed : uam::OpenCodeNativeGateState::Ready;
		fixture.Until([&] { return fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.app.chats.front(), error) == expected; });
		UAM_ASSERT_EQ(fixture.replies->load(), 0);
		UAM_ASSERT(!fixture.enabled->load());
		UAM_ASSERT_EQ(fixture.Session().active_uam_agent_workspace_access, std::string("read"));
		UAM_ASSERT_EQ(fixture.Session().session_id, std::string("ses_root"));
		UAM_ASSERT_EQ(fixture.Session().process_execution_host_id, std::string("owned-ssh"));
		if (foreign)
		{
			UAM_ASSERT_EQ(fixture.policy_updates->load(), 0);
			UAM_ASSERT(fixture.Session().last_error.find("owned workspace") != std::string::npos);
		}
		else
		{
			UAM_ASSERT_EQ(fixture.policy_updates->load(), 1);
			UAM_ASSERT(fixture.Session().running);
			UAM_ASSERT(fixture.Session().pending_permission.request_id_json.empty());
		}
		fixture.service->Stop();
	}
}
#endif
