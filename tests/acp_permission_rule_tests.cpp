#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/security/command_rules.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{
	namespace rules = uam::command_rules;

	rules::Rule CommandRule(std::string id, rules::Action action, std::string pattern)
	{
		rules::Rule rule;
		rule.id = std::move(id);
		rule.action = action;
		rule.tool = rules::Tool::Command;
		rule.pattern = std::move(pattern);
		return rule;
	}

	/// <summary>A running Gemini ACP session whose permission responses go to a file sink, so the
	/// outcome is observed through UAM's own pending state and diagnostics.</summary>
	struct PermissionRuleFixture
	{
		TempDir temp{"uam-permission-rules"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;

		PermissionRuleFixture(std::string tier, std::vector<rules::Rule> list, std::string approval_mode = "default")
		{
			app.data_root = temp.root / "data";
			app.settings.permission_command_rules = std::move(list);
			ChatSession chat;
			chat.id = "permission-rules";
			chat.provider_id = "gemini-cli";
			chat.execution_host_id = "local";
			chat.workspace_directory = temp.root.string();
			chat.approval_mode = std::move(approval_mode);
			chat.command_safety_tier = std::move(tier);
			chat.messages.push_back({MessageRole::User, "Run the fixture"});
			chat.messages.push_back({MessageRole::Assistant, ""});
			app.chats.push_back(std::move(chat));
			auto owned = std::make_unique<uam::AcpSessionState>();
			session = owned.get();
			session->chat_id = app.chats.front().id;
			session->provider_id = app.chats.front().provider_id;
			session->protocol_kind = "gemini-acp";
			session->session_id = "permission-rules-native";
			session->running = session->processing = true;
			app.acp_sessions.push_back(std::move(owned));
			const std::vector<std::string> sink = StdioCatArgv(temp.root / "permission-responses.bin");
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*session, temp.root, sink, &error));
		}

		~PermissionRuleFixture()
		{
			auto& process = PlatformServicesFactory::Instance().process_service;
			process.StopStdioProcess(*session, true);
			process.CloseStdioProcessHandles(*session);
		}

		void Request(int id, const std::string& kind, const std::string& command)
		{
			const nlohmann::json frame = {
			    {"jsonrpc", "2.0"}, {"id", id}, {"method", "session/request_permission"},
			    {"params", {{"sessionId", session->session_id},
			        {"toolCall", {{"toolCallId", "tool-" + std::to_string(id)}, {"title", "Run a command"}, {"kind", kind}, {"rawInput", {{"command", command}}}}},
			        {"options", nlohmann::json::array({{{"optionId", "allow-once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
			                                           {{"optionId", "reject-once"}, {"name", "Reject"}, {"kind", "reject_once"}}})}}}};
			uam::acp_detail::HandlePermissionRequest(app, *session, app.chats.front(), frame);
		}

		bool Waiting(int id) const
		{
			return session->waiting_for_permission && session->pending_permission.request_id_json == std::to_string(id);
		}

		bool Diagnostic(std::string_view reason) const
		{
			return std::ranges::any_of(session->diagnostics, [&](const uam::AcpDiagnosticEntryState& entry) { return entry.reason == reason; });
		}
	};
}

UAM_TEST(PermissionRulesReadOnlyDefaultReplacesTheReviewerUnderAiReview)
{
	PermissionRuleFixture fixture("aiReview", rules::DefaultRules());
	fixture.Request(1, "execute", "pwd");
	UAM_ASSERT(!fixture.Waiting(1));
	UAM_ASSERT(fixture.Diagnostic("approved_by_rule"));
	UAM_ASSERT(fixture.app.permission_review_tasks.empty());
}

UAM_TEST(PermissionRulesAllowNeverApprovesOutsideAiReview)
{
	for (const std::string tier : {"off", "acceptEdits"})
	{
		PermissionRuleFixture fixture(tier, {CommandRule("allow-npm", rules::Action::Allow, "npm test"), rules::DefaultRules().front()});
		fixture.Request(1, "execute", "npm test");
		UAM_ASSERT(fixture.Waiting(1));
		UAM_ASSERT(!fixture.Diagnostic("approved_by_rule"));
		PermissionRuleFixture read_only(tier, rules::DefaultRules());
		read_only.Request(2, "execute", "pwd");
		UAM_ASSERT(read_only.Waiting(2));
	}
}

UAM_TEST(PermissionRulesAllowCannotApproveHighRiskRequests)
{
	for (const std::string_view command : {"git push origin main", "rm -rf build", "npm test && rm -rf build", "curl https://example.invalid"})
	{
		PermissionRuleFixture fixture("aiReview", {CommandRule("allow-git", rules::Action::Allow, "git push"), CommandRule("allow-rm", rules::Action::Allow, "rm"),
		                                           CommandRule("allow-npm", rules::Action::Allow, "npm test"), CommandRule("allow-curl", rules::Action::Allow, "curl")});
		fixture.Request(1, "execute", std::string(command));
		UAM_ASSERT(fixture.Waiting(1));
		UAM_ASSERT(!fixture.Diagnostic("approved_by_rule"));
	}
}

UAM_TEST(PermissionRulesRunAfterThePlanCeiling)
{
	PermissionRuleFixture fixture("aiReview", {CommandRule("allow-npm", rules::Action::Allow, "npm test")}, "plan");
	fixture.Request(1, "execute", "npm test");
	UAM_ASSERT(!fixture.Waiting(1));
	UAM_ASSERT(fixture.Diagnostic("rejected_by_access_ceiling"));
	UAM_ASSERT(!fixture.Diagnostic("approved_by_rule"));
}

UAM_TEST(PermissionRulesAskHoldsTheRequestEvenUnderYolo)
{
	PermissionRuleFixture held("yolo", {CommandRule("ask-rm", rules::Action::Ask, "rm")});
	held.Request(1, "execute", "rm -rf build");
	UAM_ASSERT(held.Waiting(1));
	UAM_ASSERT(held.session->pending_permission.command_rule_hold);
	UAM_ASSERT(held.Diagnostic("held_by_rule"));

	PermissionRuleFixture dynamic("yolo", {CommandRule("ask-rm", rules::Action::Ask, "rm")});
	dynamic.Request(1, "execute", "$TOOL -rf build");
	UAM_ASSERT(dynamic.Waiting(1));

	PermissionRuleFixture unmatched("yolo", {CommandRule("ask-rm", rules::Action::Ask, "rm")});
	unmatched.Request(1, "execute", "ls");
	UAM_ASSERT(!unmatched.Waiting(1));
}

UAM_TEST(PermissionRulesDenyRejectsChainedCommandsUnderEveryTier)
{
	for (const std::string tier : {"off", "acceptEdits", "aiReview", "yolo"})
	{
		PermissionRuleFixture fixture(tier, rules::DefaultRules());
		fixture.Request(1, "execute", "npm test && git push --force");
		UAM_ASSERT(!fixture.Waiting(1));
		UAM_ASSERT(fixture.Diagnostic("denied_by_rule"));
		const auto tool = std::ranges::find(fixture.session->tool_calls, std::string("tool-1"), &uam::AcpToolCallState::id);
		UAM_ASSERT(tool != fixture.session->tool_calls.end() && tool->status == uam::acp_statuses::kFailed);
		UAM_ASSERT_EQ(tool->permission_review_decision, std::string("rule deny"));
	}
}

UAM_TEST(PermissionRulesAiReviewOffLeavesUnmatchedRequestsForTheUser)
{
	PermissionRuleFixture fixture("aiReview", rules::DefaultRules());
	fixture.app.settings.permission_ai_review_enabled = false;
	fixture.Request(1, "execute", "npm test");
	UAM_ASSERT(fixture.Waiting(1));
	UAM_ASSERT(fixture.Diagnostic("ai_review_off"));
	UAM_ASSERT(fixture.app.permission_review_tasks.empty());
}

UAM_TEST(PermissionReviewCacheReplaysAnIdenticalVerdictWithoutAWorker)
{
	for (const std::string verdict : {"approve", "deny"})
	{
		PermissionRuleFixture fixture("aiReview", {});
		fixture.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		fixture.app.settings.permission_reviewer_provider_id = "gemini-cli";
		// Capture the request with AI review off so no worker starts, then prime the cache with its exact input.
		fixture.app.settings.permission_ai_review_enabled = false;
		fixture.Request(1, "execute", "npm test");
		UAM_ASSERT(fixture.Waiting(1));
		const ChatSession& chat = fixture.app.chats.front();
		const std::string key = uam::acp_detail::PermissionReviewCacheKey(fixture.app, *fixture.session, chat, "gemini-cli");
		UAM_ASSERT(!key.empty());
		fixture.app.permission_review_cache[key] = verdict;
		fixture.app.permission_review_cache_order.push_back(key);

		fixture.app.settings.permission_ai_review_enabled = true;
		(void)uam::acp_detail::TryAutoApprovePendingPermission(fixture.app, *fixture.session, chat);
		UAM_ASSERT(fixture.Diagnostic("cached"));
		UAM_ASSERT_EQ(fixture.app.permission_review_tasks.size(), std::size_t{1});
		UAM_ASSERT(fixture.app.permission_review_tasks.front().worker == nullptr);
		(void)uam::acp_detail::PollPermissionReviewTasks(fixture.app);
		UAM_ASSERT(!fixture.Waiting(1));
		UAM_ASSERT(fixture.Diagnostic(verdict == "approve" ? "approved_once" : "denied"));
		UAM_ASSERT(fixture.app.permission_review_tasks.empty());

		// A later call of the same command carries a new tool-call id and request id, and still reuses it.
		fixture.Request(2, "execute", "npm test");
		UAM_ASSERT_EQ(std::ranges::count_if(fixture.session->diagnostics, [](const uam::AcpDiagnosticEntryState& entry) { return entry.reason == "cached"; }), std::ptrdiff_t{2});
		(void)uam::acp_detail::PollPermissionReviewTasks(fixture.app);
		UAM_ASSERT(!fixture.Waiting(2));
	}
}

UAM_TEST(PermissionReviewCacheKeyCoversOnlyPlainTypedCommands)
{
	PermissionRuleFixture fixture("aiReview", {});
	fixture.app.settings.permission_ai_review_enabled = false;
	const ChatSession& chat = fixture.app.chats.front();
	fixture.Request(1, "execute", "npm test");
	const std::string first = uam::acp_detail::PermissionReviewCacheKey(fixture.app, *fixture.session, chat, "gemini-cli");
	fixture.session->pending_permission.execution_command = "npm test -- --watch";
	UAM_ASSERT(uam::acp_detail::PermissionReviewCacheKey(fixture.app, *fixture.session, chat, "gemini-cli") != first);
	fixture.session->pending_permission.codex_approval_payload_json = R"({"grantRoot":"/"})";
	UAM_ASSERT(uam::acp_detail::PermissionReviewCacheKey(fixture.app, *fixture.session, chat, "gemini-cli").empty());
	fixture.session->pending_permission.codex_approval_payload_json.clear();
	fixture.session->pending_permission.kind = "edit";
	UAM_ASSERT(uam::acp_detail::PermissionReviewCacheKey(fixture.app, *fixture.session, chat, "gemini-cli").empty());
}

UAM_TEST(PermissionRulesDoNotHoldRequestsWithoutATypedCommand)
{
	PermissionRuleFixture fixture("yolo", rules::DefaultRules());
	fixture.Request(1, "execute", "");
	UAM_ASSERT(!fixture.Waiting(1));
	UAM_ASSERT(!fixture.Diagnostic("held_by_rule"));
}

UAM_TEST(PermissionReviewCacheIsNotConsultedForHeldOrHighRiskRequests)
{
	PermissionRuleFixture fixture("aiReview", {CommandRule("ask-npm", rules::Action::Ask, "npm")});
	fixture.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	fixture.app.settings.permission_reviewer_provider_id = "gemini-cli";
	fixture.Request(1, "execute", "npm test");
	UAM_ASSERT(fixture.Waiting(1));
	UAM_ASSERT(fixture.app.permission_review_tasks.empty());
	UAM_ASSERT(!fixture.Diagnostic("cached") && !fixture.Diagnostic("started"));

	PermissionRuleFixture high_risk("aiReview", {});
	high_risk.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	high_risk.app.settings.permission_reviewer_provider_id = "gemini-cli";
	high_risk.Request(1, "execute", "git push origin main");
	const std::string key = uam::acp_detail::PermissionReviewCacheKey(high_risk.app, *high_risk.session, high_risk.app.chats.front(), "gemini-cli");
	high_risk.app.permission_review_cache[key] = "approve";
	(void)uam::acp_detail::TryAutoApprovePendingPermission(high_risk.app, *high_risk.session, high_risk.app.chats.front());
	UAM_ASSERT(high_risk.Waiting(1));
	UAM_ASSERT(high_risk.app.permission_review_tasks.empty());
	UAM_ASSERT(!high_risk.Diagnostic("cached"));
}

UAM_TEST(PermissionReviewCacheStoresOnlyDeliveredVerdicts)
{
	PermissionRuleFixture fixture("aiReview", {});
	const auto completed = [&](const std::string& request_id, const std::string& output, const std::string& key)
	{
		fixture.session->pending_permission = {};
		fixture.session->pending_permission.request_id_json = request_id;
		fixture.session->pending_permission.tool_call_id = "tool-" + request_id;
		fixture.session->pending_permission.kind = "execute";
		fixture.session->pending_permission.execution_command = "npm test";
		fixture.session->pending_permission.options = {{"allow-once", "Allow once", "allow_once"}, {"reject-once", "Reject", "reject_once"}};
		fixture.session->waiting_for_permission = true;
		uam::AsyncPermissionReviewTask task;
		task.permission_review_incarnation = fixture.session->permission_review_incarnation;
		task.chat_id = fixture.app.chats.front().id;
		task.request_id_json = request_id;
		task.cache_key = key;
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.state->result.ok = true;
		task.state->result.output = output;
		task.state->completed = true;
		fixture.app.permission_review_tasks.push_back(std::move(task));
		(void)uam::acp_detail::PollPermissionReviewTasks(fixture.app);
	};
	completed("7", R"({"decision":"uncertain","reason":"Needs context"})", "uncertain-key");
	UAM_ASSERT(!fixture.app.permission_review_cache.contains("uncertain-key"));
	completed("8", R"({"decision":"approve","reason":"Routine test run"})", "approve-key");
	UAM_ASSERT(fixture.app.permission_review_cache.contains("approve-key"));
	UAM_ASSERT_EQ(fixture.app.permission_review_cache.at("approve-key"), std::string("approve"));

	// A deny rule added while the reviewer ran wins: the approval is not delivered or cached, and the deny applies.
	fixture.app.settings.permission_command_rules = {CommandRule("deny-npm", rules::Action::Deny, "npm")};
	completed("9", R"({"decision":"approve","reason":"Routine test run"})", "late-key");
	UAM_ASSERT(!fixture.Waiting(9));
	UAM_ASSERT(fixture.Diagnostic("superseded_by_rule"));
	UAM_ASSERT(fixture.Diagnostic("denied_by_rule"));
	UAM_ASSERT(!fixture.app.permission_review_cache.contains("late-key"));
	UAM_ASSERT(fixture.app.permission_review_tasks.empty());
}
#endif

UAM_TEST(PermissionRulesPersistAndAreValidatedAtLoad)
{
	namespace rules = uam::command_rules;
	TempDir temp("uam-permission-rules-settings");
	const fs::path file = temp.root / "settings.txt";

	// A missing file starts with the safe defaults.
	AppSettings fresh;
	(void)SettingsStore::Load(file, fresh);
	UAM_ASSERT_EQ(fresh.permission_command_rules.size(), rules::DefaultRules().size());
	UAM_ASSERT(fresh.permission_ai_review_enabled);

	// Saved rules and the AI review switch round-trip; a saved empty list stays empty.
	AppSettings saved;
	rules::Rule rule;
	rule.id = "deny-make";
	rule.action = rules::Action::Deny;
	rule.tool = rules::Tool::Command;
	rule.pattern = "make deploy";
	rule.provider_id = "codex-cli";
	rule.workspace = "/work/project";
	rule.enabled = false;
	saved.permission_command_rules = {rule};
	saved.permission_ai_review_enabled = false;
	UAM_ASSERT(SettingsStore::Save(file, saved));
	AppSettings loaded;
	(void)SettingsStore::Load(file, loaded);
	UAM_ASSERT_EQ(loaded.permission_command_rules.size(), std::size_t{1});
	UAM_ASSERT_EQ(loaded.permission_command_rules.front().pattern, std::string("make deploy"));
	UAM_ASSERT_EQ(loaded.permission_command_rules.front().provider_id, std::string("codex-cli"));
	UAM_ASSERT(!loaded.permission_command_rules.front().enabled);
	UAM_ASSERT(!loaded.permission_ai_review_enabled);
	saved.permission_command_rules.clear();
	UAM_ASSERT(SettingsStore::Save(file, saved));
	AppSettings emptied;
	(void)SettingsStore::Load(file, emptied);
	UAM_ASSERT(emptied.permission_command_rules.empty());

	// Hand-edited invalid entries are dropped at load, never widened.
	std::string text;
	UAM_ASSERT(uam::io::TryReadTextFile(file, text));
	const std::size_t key = text.find("permission_command_rules=");
	UAM_ASSERT(key != std::string::npos);
	const std::size_t end = text.find('\n', key);
	const nlohmann::json edited = nlohmann::json::array({
	    {{"id", "ok"}, {"action", "ask"}, {"tool", "command"}, {"pattern", "rm"}},
	    {{"id", "wide"}, {"action", "allow"}, {"tool", "any"}},
	    {{"id", "glob"}, {"action", "allow"}, {"tool", "command"}, {"pattern", "*"}},
	    {{"id", "chain"}, {"action", "deny"}, {"tool", "command"}, {"pattern", "a; b"}},
	    {{"id", "bad"}, {"action", "approve"}, {"tool", "command"}, {"pattern", "rm"}},
	});
	text.replace(key, end - key, "permission_command_rules=" + uam::EncodeLineValue(edited.dump()));
	UAM_ASSERT(uam::io::WriteTextFile(file, text));
	AppSettings validated;
	const SettingsLoadResult partial = SettingsStore::Load(file, validated);
	UAM_ASSERT_EQ(validated.permission_command_rules.size(), std::size_t{1});
	UAM_ASSERT_EQ(validated.permission_command_rules.front().id, std::string("ok"));
	UAM_ASSERT(partial.warning.find("permission rules") != std::string::npos);

	// An unreadable list falls back to the defaults, never to no rules, and says so.
	const std::size_t again = text.find("permission_command_rules=");
	text.replace(again, text.find('\n', again) - again, "permission_command_rules=" + uam::EncodeLineValue("{not json"));
	UAM_ASSERT(uam::io::WriteTextFile(file, text));
	AppSettings corrupt;
	const SettingsLoadResult unreadable = SettingsStore::Load(file, corrupt);
	UAM_ASSERT_EQ(corrupt.permission_command_rules.size(), rules::DefaultRules().size());
	UAM_ASSERT(unreadable.warning.find("default rules") != std::string::npos);
}
