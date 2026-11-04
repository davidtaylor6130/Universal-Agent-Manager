#include "test_harness.h"
#include "cef/state_serializer.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
namespace
{

struct PermissionProjectionFixture
{
	TempDir temp{"uam-permission-projection"};
	uam::AppState app;
	uam::AcpSessionState* session = nullptr;

	explicit PermissionProjectionFixture(bool goal_review, bool unrestricted = false)
	{
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession chat;
		chat.id = "private-permission-projection";
		chat.provider_id = "opencode-cli";
		chat.execution_host_id = "local";
		chat.workspace_directory = temp.root.string();
		chat.approval_mode = "default";
		chat.command_safety_tier = "yolo";
		app.chats.push_back(std::move(chat));
		app.selected_chat_index = 0;
		auto owned = std::make_unique<uam::AcpSessionState>();
		session = owned.get();
		session->chat_id = app.chats.front().id;
		session->provider_id = app.chats.front().provider_id;
		session->protocol_kind = "opencode-acp";
		session->session_id = "private-projection-native-session";
		session->running = session->initialized = session->session_ready = true;
		session->waiting_for_permission = true;
		session->lifecycle_state = "waitingPermission";
		session->goal_review_turn = !unrestricted && goal_review;
		session->active_uam_agent_workspace_access = !unrestricted && !goal_review ? "read" : "";
		session->active_uam_agent_id = session->active_uam_agent_workspace_access.empty() ? "" : "private-read-agent";
		app.acp_sessions.push_back(std::move(owned));
	}

	void SetPending(const std::string& kind)
	{
		auto& pending = session->pending_permission;
		pending.request_id_json = "\"native-projection-request\"";
		pending.provider_request_method = "session/request_permission";
		pending.provider_request_kind = kind;
		pending.provider_request_params_json = R"({ "private": "C:\\fixture\\notes", "literal": "line\nnext" })";
		pending.provider_input_json = R"({"path":"private-fixture.txt"})";
		pending.tool_call_id = "private-projection-tool";
		pending.title = kind == "read" ? "Read private fixture" : "Change private fixture";
		pending.kind = kind;
		pending.status = "pending";
		pending.content = "private-fixture.txt";
		pending.options = {{"once", "Allow once", "allow_once"},
		    {"persistent", "Allow always", "allow_always"},
		    {"allow-looking-rejection", "Deny", "reject_once"},
		    {"cancelled", "Cancel", "reject_once"}};
	}
};

// Snapshot native fields independently of the frontend projection. Strings remain byte-exact.
nlohmann::json NativePermissionSnapshot(const uam::AcpPendingPermissionState& pending)
{
	nlohmann::json options = nlohmann::json::array();
	for (const auto& option : pending.options)
		options.push_back({{"id", option.id}, {"name", option.name}, {"kind", option.kind}});
	return {{"request", pending.request_id_json}, {"method", pending.provider_request_method},
	    {"providerKind", pending.provider_request_kind}, {"codexPayload", pending.codex_approval_payload_json},
	    {"input", pending.provider_input_json}, {"params", pending.provider_request_params_json},
	    {"tool", pending.tool_call_id}, {"title", pending.title}, {"kind", pending.kind},
	    {"status", pending.status}, {"content", pending.content}, {"risk", pending.safety_risk},
	    {"tier", pending.safety_tier}, {"requiresApproval", pending.safety_requires_approval},
	    {"versionControlled", pending.version_controlled_workspace}, {"options", std::move(options)}};
}

std::vector<nlohmann::json> ProjectPendingPermission(const PermissionProjectionFixture& fixture)
{
	const auto before = NativePermissionSnapshot(fixture.session->pending_permission);
	const std::vector<Message> messages = fixture.app.chats.front().messages;
	const std::vector<nlohmann::json> snapshots = {
	    uam::StateSerializer::Serialize(fixture.app, false),
	    uam::StateSerializer::Serialize(fixture.app, true),
	    uam::StateSerializer::SerializeFingerprint(fixture.app)};
	UAM_ASSERT_EQ(NativePermissionSnapshot(fixture.session->pending_permission), before);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
	UAM_ASSERT(fixture.session->waiting_for_permission);
	std::vector<nlohmann::json> permissions;
	for (const auto& snapshot : snapshots)
	{
		UAM_ASSERT_EQ(snapshot.at("chats").size(), std::size_t(1));
		const auto& permission = snapshot.at("chats").at(0).at("acpSession").at("pendingPermission");
		UAM_ASSERT(permission.is_object());
		UAM_ASSERT_EQ(permission.at("requestId"), before.at("request"));
		UAM_ASSERT_EQ(permission.at("toolCallId"), before.at("tool"));
		UAM_ASSERT_EQ(permission.at("title"), before.at("title"));
		UAM_ASSERT_EQ(permission.at("kind"), before.at("kind"));
		UAM_ASSERT_EQ(permission.at("content"), before.at("content"));
		permissions.push_back(permission);
	}
	return permissions;
}

nlohmann::json DenialAndCancelOptions()
{
	return nlohmann::json::array({
	    {{"id", "allow-looking-rejection"}, {"name", "Deny"}, {"kind", "reject_once"}},
	    {{"id", "cancelled"}, {"name", "Cancel"}, {"kind", "reject_once"}}});
}

}

UAM_TEST(AcpPermissionProjectionHidesCapturedMutationChoices)
{
	for (bool goal_review : {false, true})
	{
		PermissionProjectionFixture fixture(goal_review);
		fixture.SetPending("edit");
		for (const auto& permission : ProjectPendingPermission(fixture))
			UAM_ASSERT_EQ(permission.at("options"), DenialAndCancelOptions());
	}
}

UAM_TEST(AcpPermissionProjectionKeepsOnlyOneTimeCapturedReadGrants)
{
	for (bool goal_review : {false, true})
	{
		PermissionProjectionFixture fixture(goal_review);
		fixture.SetPending("read");
		nlohmann::json expected = nlohmann::json::array({{{"id", "once"}, {"name", "Allow once"}, {"kind", "allow_once"}}});
		for (const auto& option : DenialAndCancelOptions()) expected.push_back(option);
		for (const auto& permission : ProjectPendingPermission(fixture))
			UAM_ASSERT_EQ(permission.at("options"), expected);
	}
}

UAM_TEST(AcpPermissionProjectionPreservesOpenCodeContinuationQuestion)
{
	for (bool goal_review : {false, true})
	{
		PermissionProjectionFixture fixture(goal_review);
		fixture.session->waiting_for_permission = false;
		fixture.session->lifecycle_state = "processing";
		fixture.session->processing = true;
		// The real ACP decoder retains OpenCode's explicit non-action continuation question.
		const nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", 42}, {"method", "session/request_permission"},
		    {"params", {{"toolCall", {{"toolCallId", "private-doom-loop"}, {"title", "doom_loop"}, {"kind", "other"},
		        {"status", "pending"}, {"rawInput", {{"tool", "read"}, {"input", {{"filePath", "private-fixture.txt"}}}}}}},
		        {"options", {{{"optionId", "once"}, {"name", "Continue once"}, {"kind", "allow_once"}},
		            {{"optionId", "reject"}, {"name", "Stop repeating"}, {"kind", "reject_once"}}}}}}};
		UAM_ASSERT(uam::ProcessAcpLineForTests(fixture.app, *fixture.session, fixture.app.chats.front(), request.dump()));
		UAM_ASSERT_EQ(fixture.session->pending_permission.request_id_json, std::string("42"));
		UAM_ASSERT_EQ(fixture.session->pending_permission.kind, std::string("other"));
		UAM_ASSERT_EQ(fixture.session->pending_permission.title, std::string("doom_loop"));
		const nlohmann::json expected = nlohmann::json::array({
		    {{"id", "once"}, {"name", "Continue once"}, {"kind", "allow_once"}},
		    {{"id", "reject"}, {"name", "Stop repeating"}, {"kind", "reject_once"}}});
		for (const auto& permission : ProjectPendingPermission(fixture))
			UAM_ASSERT_EQ(permission.at("options"), expected);
	}
}

UAM_TEST(AcpPermissionProjectionKeepsUnrestrictedNativeOffers)
{
	PermissionProjectionFixture fixture(false, true);
	fixture.SetPending("edit");
	const auto expected = NativePermissionSnapshot(fixture.session->pending_permission).at("options");
	for (const auto& permission : ProjectPendingPermission(fixture))
		UAM_ASSERT_EQ(permission.at("options"), expected);
}
#endif
