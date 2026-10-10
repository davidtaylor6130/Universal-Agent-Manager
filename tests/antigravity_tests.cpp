#include "test_harness.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_internal.h"
#include <limits>

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
UAM_TEST(AntigravityStructuredLaunchUsesNativeStreamAndSavedConversation)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	ChatSession chat;
	chat.native_session_id = "11111111-2222-4333-8444-555555555555";
	chat.model_id = "user-model";
	const std::vector<std::string> args = runtime.BuildStructuredLaunchArgv(ProviderProfileStore::DefaultAntigravityProfile(), chat);
	UAM_ASSERT_EQ(args, (std::vector<std::string>{"agy", "--input-format", "stream-json", "--output-format", "stream-json", "--conversation", chat.native_session_id, "--model", chat.model_id}));
	uam::AcpSessionState session;
	UAM_ASSERT(runtime.OnAcpBuildInitialize(session, 1).is_null());
	UAM_ASSERT(session.initialized);
	std::string method;
	UAM_ASSERT(runtime.OnAcpBuildSetupRequest(2, chat, "/tmp", true, method).is_null());
	UAM_ASSERT(runtime.OnAcpBuildCancel(session, 3, method).is_null());
	UAM_ASSERT_EQ(runtime.OnAcpBuildPrompt(session, 4, "hello", chat, method), (nlohmann::json{{"event", "user"}, {"message", {{"content", "hello"}}}}));
}

UAM_TEST(AntigravityCapturedNativeStreamPersistsIdentityAndAvoidsDuplicateAnswer)
{
	TempDir temp("uam-agy-stream");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-stream";
	chat.provider_id = "antigravity-cli";
	app.chats.push_back(chat);
	ChatSession& active = app.chats.front();
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.chat_id = chat.id;
	session.running = true;
	session.processing = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	active.messages.push_back({MessageRole::Assistant, "AGY_PROTOCOL_20261007\n", "earlier"});
	active.messages.push_back({MessageRole::User, "Repeat the marker", "now"});
	session.turn_user_message_index = 1;
	uam::acp_detail::RememberAssistantReplayPrefixes(session, active, 1);
	uam::acp_detail::RememberLoadHistoryReplayUpdates(session, active, 1);
	const auto consume = [&](const char* line) { UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, active, nlohmann::json::parse(line), nullptr)); };
	consume(R"({"event":"init","conversation_id":"11111111-2222-4333-8444-555555555555","init":{"permission_mode":"request-review"}})");
	consume(R"({"event":"step_update","step_update":{"conversation_id":"11111111-2222-4333-8444-555555555555","step_index":1,"state":"ACTIVE","step_type":"agent_response","text_delta":"AGY_PROTOCOL_20261007"}})");
	consume(R"({"event":"step_update","step_update":{"conversation_id":"11111111-2222-4333-8444-555555555555","step_index":1,"state":"DONE","step_type":"agent_response","text_delta":"\n","usage":{"input_tokens":11765,"output_tokens":171}}})");
	consume(R"({"event":"result","result":{"conversation_id":"11111111-2222-4333-8444-555555555555","status":"SUCCESS","response":"AGY_PROTOCOL_20261007\n","num_turns":1,"usage":{"input_tokens":11765,"output_tokens":171}}})");
	UAM_ASSERT_EQ(active.native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	UAM_ASSERT(!session.processing);
	UAM_ASSERT_EQ(active.messages.size(), std::size_t{3});
	UAM_ASSERT_EQ(active.messages.back().content, std::string("AGY_PROTOCOL_20261007\n"));
	UAM_ASSERT_EQ(active.messages.back().tokens_input, 11765);
	UAM_ASSERT_EQ(active.messages.back().tokens_output, 171);
	const auto saved = ChatRepository::LoadLocalChat(temp.root, active.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->native_session_id, active.native_session_id);
}

UAM_TEST(AntigravityCumulativeUsageDuplicateResultsAndNewProcessReset)
{
	TempDir temp("uam-agy-counters");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-counters";
	chat.provider_id = "antigravity-cli";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.chat_id = chat.id;
	session.running = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	runtime.OnAcpBuildInitialize(session, 1);
	const auto result = [&](int turns, int input, int output, const char* text)
	{
		return nlohmann::json{{"event", "result"}, {"result", {{"conversation_id", "11111111-2222-4333-8444-555555555555"}, {"status", "SUCCESS"}, {"response", text}, {"num_turns", turns}, {"usage", {{"input_tokens", input}, {"output_tokens", output}}}}}};
	};
	const auto step = [&](int index, int input, int output)
	{
		runtime.OnAcpHandleMessage(app, session, chat, {{"event", "step_update"}, {"step_update", {{"conversation_id", "11111111-2222-4333-8444-555555555555"}, {"step_type", "agent_response"}, {"step_index", index}, {"state", "DONE"}, {"usage", {{"input_tokens", input}, {"output_tokens", output}}}}}}, nullptr);
	};
	std::string method;
	runtime.OnAcpBuildPrompt(session, 1, "first", chat, method);
	session.processing = true;
	step(1, 100, 10);
	runtime.OnAcpHandleMessage(app, session, chat, result(1, 100, 10, "one"), nullptr);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	uam::acp_detail::ResetAcpTurnStreamState(session);
	session.processing = true;
	session.current_assistant_message_index = -1;
	runtime.OnAcpBuildPrompt(session, 2, "second", chat, method);
	runtime.OnAcpHandleMessage(app, session, chat, result(1, 100, 10, "duplicate"), nullptr);
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	step(2, 25, 4);
	step(2, 25, 4); // Duplicate completed step must not add usage twice.
	runtime.OnAcpHandleMessage(app, session, chat, result(2, 125, 14, "two"), nullptr);
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("two"));
	UAM_ASSERT_EQ(chat.messages.back().tokens_input, 25);
	UAM_ASSERT_EQ(chat.messages.back().tokens_output, 4);
	runtime.OnAcpBuildInitialize(session, 2);
	UAM_ASSERT_EQ(session.antigravity_completed_turns, int64_t{0});
	uam::acp_detail::ResetAcpTurnStreamState(session);
	session.current_assistant_message_index = -1;
	session.processing = true;
	runtime.OnAcpBuildPrompt(session, 3, "resumed", chat, method);
	step(9, 30, 3);
	uam::acp_detail::RememberAssistantReplayPrefixes(session, chat, static_cast<int>(chat.messages.size()));
	runtime.OnAcpHandleMessage(app, session, chat, result(3, 1000000, 100000, "two"), nullptr);
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("two"));
	UAM_ASSERT_EQ(chat.messages.back().tokens_input, 30);
	UAM_ASSERT_EQ(chat.messages.back().tokens_output, 3);
}

UAM_TEST(AntigravityRejectsChangedIdentityAndKeepsNativePermissionPolicy)
{
	TempDir temp("uam-agy-identity");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-identity";
	chat.provider_id = "antigravity-cli";
	chat.native_session_id = "11111111-2222-4333-8444-555555555555";
	uam::AcpSessionState session;
	session.session_id = chat.native_session_id;
	session.processing = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	runtime.OnAcpHandleMessage(app, session, chat, {{"event", "init"}, {"conversation_id", "99999999-2222-4333-8444-555555555555"}}, nullptr);
	UAM_ASSERT(!session.processing);
	UAM_ASSERT(session.last_error.find("identity") != std::string::npos);
	UAM_ASSERT_EQ(chat.native_session_id, std::string("11111111-2222-4333-8444-555555555555"));
	for (const nlohmann::json& counter : std::vector<nlohmann::json>{nullptr, 0, -1, "1", 1.5, false, std::numeric_limits<uint64_t>::max()})
	{
		uam::AcpSessionState malformed;
		malformed.session_id = chat.native_session_id;
		malformed.processing = true;
		nlohmann::json result{{"conversation_id", chat.native_session_id}, {"status", "SUCCESS"}, {"response", "must not complete"}};
		if (!counter.is_null()) result["num_turns"] = counter;
		runtime.OnAcpHandleMessage(app, malformed, chat, {{"event", "result"}, {"result", result}}, nullptr);
		UAM_ASSERT(!malformed.processing);
		UAM_ASSERT(malformed.last_error.find("turn counter") != std::string::npos);
		UAM_ASSERT_EQ(malformed.antigravity_completed_turns, int64_t{0});
	}
	const nlohmann::json serialized = uam::StateSerializer::SerializeProvider(ProviderProfileStore::DefaultAntigravityProfile());
	UAM_ASSERT_EQ(serialized["structuredPermissionControl"], nlohmann::json("provider"));
}

UAM_TEST(AntigravityDocumentedToolStepRendersOutputAndError)
{
	TempDir temp("uam-agy-tool-step");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "agy-tool-step";
	chat.provider_id = "antigravity-cli";
	uam::AcpSessionState session;
	session.processing = true;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("antigravity-cli");
	runtime.OnAcpHandleMessage(app, session, chat, nlohmann::json::parse(R"({"event":"step_update","step_update":{"conversation_id":"11111111-2222-4333-8444-555555555555","step_index":4,"state":"DONE","step_type":"tool","tool_name":"run_command","tool_info":{"parameters":{"CommandLine":"fixture only"},"output":"fixture output","error":{"message":"fixture failure"}}}})"), nullptr);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.messages.front().tool_calls.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.messages.front().tool_calls.front().status, std::string("failed"));
	UAM_ASSERT(chat.messages.front().tool_calls.front().result_text.find("fixture failure") != std::string::npos);
}

#endif
