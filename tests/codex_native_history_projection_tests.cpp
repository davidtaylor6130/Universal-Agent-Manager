#include "test_harness.h"
#include "app/runtime_orchestration_services.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
const std::string kCodexHistoryThread = "550e8400-e29b-41d4-a716-446655440000";
const std::string kCodexHistoryChild = "660e8400-e29b-41d4-a716-446655440001";
const std::string kCodexHistoryOtherChild = "770e8400-e29b-41d4-a716-446655440002";

nlohmann::json CodexRichHistory()
{
	return {{"thread", {{"id", kCodexHistoryThread}, {"cwd", "/workspace"}, {"turns", nlohmann::json::array({
	    {{"id", "actual-turn"}, {"items", nlohmann::json::array({
	        {{"id", "reasoning"}, {"type", "reasoning"}, {"content", nlohmann::json::array({"Actual raw reasoning"})},
	            {"summary", nlohmann::json::array({{{"type", "summary_text"}, {"text", "Actual reasoning summary"}}})}, {"encrypted_content", "opaque-not-visible"}},
	        {{"id", "plan"}, {"type", "plan"}, {"text", "Actual native plan"}},
	        {{"id", "reply-before"}, {"type", "agentMessage"}, {"text", "Before compaction"}},
	        {{"id", "compact"}, {"type", "contextCompaction"}, {"summary", "Retained native context"}},
	        {{"id", "children"}, {"type", "collabAgentToolCall"}, {"tool", "spawnAgent"}, {"status", "completed"},
	            {"senderThreadId", kCodexHistoryThread}, {"receiverThreadIds", nlohmann::json::array({kCodexHistoryChild, kCodexHistoryOtherChild})},
	            {"agentsStates", {{kCodexHistoryChild, {{"status", "completed"}}}, {kCodexHistoryOtherChild, {{"status", "running"}}}}}},
	        {{"id", "reply-after"}, {"type", "agentMessage"}, {"text", "After compaction"}}
	    })}}
	})}}}};
}
}

UAM_TEST(CodexRemoteHistoryPreservesRichTimelineAndNativeChildLinks)
{
	TempDir root("uam-codex-rich-history");
	const auto native = CodexRichHistory();
	const auto parsed = ChatHistorySyncService::ParseRemoteCodexTranscript(native);
	UAM_ASSERT(parsed.success && parsed.error.empty());
	UAM_ASSERT_EQ(parsed.messages.size(), std::size_t(1));
	const Message& message = parsed.messages.front();
	UAM_ASSERT_EQ(message.content, std::string("Before compaction\nAfter compaction"));
	UAM_ASSERT(message.thoughts.find("Actual raw reasoning") != std::string::npos);
	UAM_ASSERT(message.thoughts.find("Actual reasoning summary") != std::string::npos);
	UAM_ASSERT(message.thoughts.find("opaque-not-visible") == std::string::npos);
	UAM_ASSERT_EQ(message.plan_summary, std::string("Actual native plan"));
	const std::vector<std::string> expected = {"thought", "plan", "assistant_text", "context_compaction", "tool_call", "assistant_text"};
	UAM_ASSERT_EQ(message.blocks.size(), expected.size());
	for (std::size_t index = 0; index < expected.size(); ++index) UAM_ASSERT_EQ(message.blocks[index].type, expected[index]);
	UAM_ASSERT_EQ(message.blocks[3].text, std::string("Retained native context"));
	UAM_ASSERT_EQ(message.blocks[3].request_id_json, std::string("codex-compaction:actual-turn:compact"));
	UAM_ASSERT_EQ(message.tool_calls.size(), std::size_t(1));
	UAM_ASSERT(message.tool_calls.front().is_sub_agent);
	UAM_ASSERT_EQ(message.tool_calls.front().sub_agent_id, kCodexHistoryChild);
	const std::vector<SubAgentChild> expected_children = {{kCodexHistoryChild, "", "completed"}, {kCodexHistoryOtherChild, "", "running"}};
	UAM_ASSERT_EQ(message.tool_calls.front().sub_agent_children, expected_children);
	UAM_ASSERT_EQ(nlohmann::json::parse(message.tool_calls.front().result_text)["receiverThreadIds"], native["thread"]["turns"][0]["items"][4]["receiverThreadIds"]);
	ChatSession saved;
	saved.id = "rich-native-history";
	saved.provider_id = "codex-cli";
	saved.native_session_id = kCodexHistoryThread;
	saved.messages = parsed.messages;
	UAM_ASSERT(ChatRepository::SaveChat(root.root, saved));
	const auto restored = ChatRepository::LoadLocalChat(root.root, saved.id);
	UAM_ASSERT(restored && restored->messages.front().blocks == message.blocks);
	UAM_ASSERT_EQ(restored->messages.front().thoughts, message.thoughts);
	UAM_ASSERT_EQ(restored->messages.front().plan_summary, message.plan_summary);
	UAM_ASSERT(restored->messages.front().tool_calls == message.tool_calls);
	auto repeated = native;
	auto& repeated_items = repeated["thread"]["turns"][0]["items"];
	auto completed_compaction = repeated_items[3];
	completed_compaction["summary"] = "Enriched native summary";
	repeated_items.push_back(completed_compaction);
	const auto enriched = ChatHistorySyncService::ParseRemoteCodexTranscript(repeated);
	UAM_ASSERT(enriched.success && enriched.messages.front().blocks.size() == expected.size());
	UAM_ASSERT_EQ(enriched.messages.front().blocks[3].text, std::string("Enriched native summary"));
}

UAM_TEST(CodexRemoteHistoryRejectsMalformedRichItemsWithoutPublishingPrefix)
{
	for (const char* path : {"/thread/turns/0/items/0/content", "/thread/turns/0/items/0/summary/0/text", "/thread/turns/0/items/1/text", "/thread/turns/0/items/3/summary"})
	{
		auto malformed = CodexRichHistory();
		malformed[nlohmann::json::json_pointer(path)] = 42;
		const auto rejected = ChatHistorySyncService::ParseRemoteCodexTranscript(malformed);
		UAM_ASSERT(!rejected.success && !rejected.error.empty() && rejected.messages.empty());
	}
	for (const std::string type : {"reasoning", "plan", "contextCompaction"})
	{
		auto native = CodexRichHistory();
		const auto items = native["thread"]["turns"][0]["items"];
		native["thread"]["turns"][0]["items"] = nlohmann::json::array({items[type == "reasoning" ? 0 : type == "plan" ? 1 : 3]});
		const auto parsed = ChatHistorySyncService::ParseRemoteCodexTranscript(native);
		UAM_ASSERT(parsed.success && parsed.messages.size() == 1 && !parsed.messages.front().blocks.empty());
	}
}

UAM_TEST(CodexNativeChildMetadataRetainsEveryValidDistinctReceiver)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli);
	ToolCall tool;
	tool.name = "wait";
	const nlohmann::json item = {{"type", "collabAgentToolCall"}, {"senderThreadId", kCodexHistoryThread},
	    {"receiverThreadIds", nlohmann::json::array({nullptr, 42, "not-a-native-thread", kCodexHistoryThread, "urn:uuid:" + kCodexHistoryThread, kCodexHistoryChild, kCodexHistoryOtherChild})}};
	runtime.ApplyNativeToolMetadata(tool, item);
	UAM_ASSERT(tool.is_sub_agent);
	UAM_ASSERT_EQ(tool.sub_agent_id, kCodexHistoryChild);
	const std::vector<SubAgentChild> expected_children = {{kCodexHistoryChild, "", ""}, {kCodexHistoryOtherChild, "", ""}};
	UAM_ASSERT_EQ(tool.sub_agent_children, expected_children);
	ToolCall invalid;
	invalid.name = "spawnAgent";
	auto no_child = item;
	no_child["receiverThreadIds"] = nlohmann::json::array({"not-a-native-thread", kCodexHistoryThread});
	runtime.ApplyNativeToolMetadata(invalid, no_child);
	UAM_ASSERT(invalid.is_sub_agent && invalid.sub_agent_id.empty() && invalid.sub_agent_children.empty());
	ToolCall ordinary;
	ordinary.name = "read_file";
	runtime.ApplyNativeToolMetadata(ordinary, {{"type", "mcpToolCall"}, {"receiverThreadIds", {kCodexHistoryChild}}});
	UAM_ASSERT(!ordinary.is_sub_agent && ordinary.sub_agent_id.empty() && ordinary.sub_agent_children.empty());
}

UAM_TEST(CodexNativeChildMetadataRejectsAliasesAndNonStringStates)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli);
	const std::string upper_parent = "550E8400-E29B-41D4-A716-446655440000";
	const std::string upper_child = "660E8400-E29B-41D4-A716-446655440001";
	nlohmann::json item = {{"type", "collabAgentToolCall"}, {"status", "completed"}, {"senderThreadId", kCodexHistoryThread},
	    {"receiverThreadIds", nlohmann::json::array({upper_parent, "urn:uuid:" + upper_parent, kCodexHistoryChild, upper_child, "urn:uuid:" + upper_child, kCodexHistoryOtherChild})},
	    {"agentsStates", {{"urn:uuid:" + upper_child, {{"status", "running"}}}, {kCodexHistoryOtherChild, {{"status", 42}}}, {kCodexHistoryThread, {{"status", "completed"}}}}}};
	ToolCall tool;
	runtime.ApplyNativeToolMetadata(tool, item);
	const std::vector<SubAgentChild> expected = {{kCodexHistoryChild, "", "running"}, {kCodexHistoryOtherChild, "", ""}};
	UAM_ASSERT_EQ(tool.sub_agent_children, expected);
	for (const nlohmann::json malformed : {nlohmann::json(nullptr), nlohmann::json(42), nlohmann::json("running"), nlohmann::json::array({"running"}), nlohmann::json{{"status", false}}})
	{
		item["agentsStates"]["urn:uuid:" + upper_child] = malformed;
		runtime.ApplyNativeToolMetadata(tool, item);
		UAM_ASSERT(tool.sub_agent_children[0].status.empty());
		UAM_ASSERT(tool.sub_agent_children[1].status.empty());
	}
	for (const nlohmann::json malformed : {nlohmann::json(nullptr), nlohmann::json(42), nlohmann::json(kCodexHistoryChild), nlohmann::json::object()})
	{
		item["receiverThreadIds"] = malformed;
		runtime.ApplyNativeToolMetadata(tool, item);
		UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id.empty() && tool.sub_agent_title.empty() && tool.sub_agent_children.empty());
	}
}

UAM_TEST(CodexLiveAndHistoryShareNativeChildNormalization)
{
	TempDir root("uam-codex-multiple-live-children");
	uam::AppState app;
	app.data_root = root.root;
	app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
	ChatSession chat;
	chat.id = "parent-chat";
	chat.provider_id = "codex-cli";
	chat.native_session_id = kCodexHistoryThread;
	app.chats.push_back(chat);
	auto active = std::make_unique<uam::AcpSessionState>();
	active->chat_id = chat.id;
	active->provider_id = chat.provider_id;
	active->protocol_kind = "codex-app-server";
	active->codex_thread_id = active->session_id = kCodexHistoryThread;
	active->running = active->session_ready = active->processing = true;
	app.acp_sessions.push_back(std::move(active));
	auto& session = *app.acp_sessions.front();
	const auto event = [&](const std::string& method, const nlohmann::json& params)
	{
		(void)uam::ProcessAcpLineForTests(app, session, app.chats.front(), nlohmann::json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}.dump());
	};
	event("turn/started", {{"threadId", kCodexHistoryThread}, {"turn", {{"id", "actual-turn"}, {"status", "inProgress"}}}});
	// Replay the preceding assistant output from the same native fixture so the
	// live tool sync has an owned assistant message before checking persistence.
	event("item/agentMessage/delta", {{"threadId", kCodexHistoryThread}, {"turnId", "actual-turn"}, {"itemId", "reply-before"}, {"delta", "Before compaction"}});
	auto native = CodexRichHistory();
	auto& item = native["thread"]["turns"][0]["items"][4];
	item["receiverThreadIds"].insert(item["receiverThreadIds"].begin(), {"not-a-native-thread", kCodexHistoryThread});
	item["subAgentId"] = "unvalidated-acp-alias";
	event("item/completed", {{"threadId", kCodexHistoryThread}, {"turnId", "actual-turn"}, {"item", item}});
	const auto history = ChatHistorySyncService::ParseRemoteCodexTranscript(native);
	UAM_ASSERT(history.success && !session.tool_calls.empty());
	UAM_ASSERT(!history.messages.empty() && !history.messages.front().tool_calls.empty());
	const auto& expected = history.messages.front().tool_calls.front();
	UAM_ASSERT_EQ(session.tool_calls.front().sub_agent_id, expected.sub_agent_id);
	UAM_ASSERT_EQ(session.tool_calls.front().sub_agent_children, expected.sub_agent_children);
	UAM_ASSERT_EQ(uam::acp_detail::PersistedToolCallFromAcpToolCall(session.tool_calls.front()).sub_agent_children, expected.sub_agent_children);
	UAM_ASSERT(!app.chats.front().messages.empty() && !app.chats.front().messages.back().tool_calls.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.back().tool_calls.front().sub_agent_children, expected.sub_agent_children);
	const nlohmann::json expected_json = nlohmann::json::array({{{"id", kCodexHistoryChild}, {"status", "completed"}}, {{"id", kCodexHistoryOtherChild}, {"status", "running"}}});
	UAM_ASSERT_EQ(uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["toolCalls"][0]["subAgentChildren"], expected_json);
	const auto& persisted = app.chats.front();
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeSession(persisted)["messages"].back()["toolCalls"][0]["subAgentChildren"], expected_json);
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeMessagePage(persisted, 10, std::nullopt, true)["messages"].back()["toolCalls"][0]["subAgentChildren"], expected_json);
	ChatMessagePage detached;
	detached.chat = persisted;
	detached.total_count = persisted.messages.size();
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeDetachedMessagePage(detached, true)["messages"].back()["toolCalls"][0]["subAgentChildren"], expected_json);
	uam::AcpSessionState restored;
	uam::acp_detail::RestoreTurnEventsFromMessageBlocks(restored, persisted.messages.back());
	UAM_ASSERT(!restored.tool_calls.empty());
	UAM_ASSERT_EQ(restored.tool_calls.front().sub_agent_children, expected.sub_agent_children);
}

UAM_TEST(CodexChildMetadataPersistenceRejectsMalformedOptionalChildren)
{
	TempDir root("uam-codex-child-persistence");
	ChatSession chat;
	chat.id = "child-metadata";
	chat.provider_id = "codex-cli";
	chat.messages = ChatHistorySyncService::ParseRemoteCodexTranscript(CodexRichHistory()).messages;
	chat.messages.front().tool_calls.front().sub_agent_children[0].title = "Native reviewer";
	UAM_ASSERT(ChatRepository::SaveChat(root.root, chat));
	const auto original = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(root.root, chat.id)));
	for (const nlohmann::json malformed : {nlohmann::json(nullptr), nlohmann::json(42), nlohmann::json::object(), nlohmann::json("children"),
	    nlohmann::json::array({nullptr, 42, "child", {{"id", 42}}, {{"id", " "}}, {{"id", kCodexHistoryChild}, {"title", false}, {"status", 42}}, {{"id", kCodexHistoryChild}, {"status", "running"}}})})
	{
		auto changed = original;
		changed["messages"][0]["tool_calls"][0]["sub_agent_children"] = malformed;
		UAM_ASSERT(uam::io::WriteTextFile(AppPaths::UamChatFilePath(root.root, chat.id), changed.dump()));
		const auto restored = ChatRepository::LoadLocalChat(root.root, chat.id);
		UAM_ASSERT(restored && !restored->messages.empty());
		const auto& tool = restored->messages.front().tool_calls.front();
		UAM_ASSERT_EQ(tool.sub_agent_id, kCodexHistoryChild);
		UAM_ASSERT_EQ(tool.sub_agent_children.size(), malformed.is_array() ? std::size_t(1) : std::size_t(0));
		if (!tool.sub_agent_children.empty()) UAM_ASSERT(tool.sub_agent_children[0].status.empty() && tool.sub_agent_children[0].title.empty());
	}
	auto legacy = original;
	legacy["messages"][0]["tool_calls"][0].erase("sub_agent_children");
	UAM_ASSERT(uam::io::WriteTextFile(AppPaths::UamChatFilePath(root.root, chat.id), legacy.dump()));
	const auto restored = ChatRepository::LoadLocalChat(root.root, chat.id);
	UAM_ASSERT(restored && restored->messages.front().tool_calls.front().sub_agent_children.empty());
	UAM_ASSERT_EQ(restored->messages.front().tool_calls.front().sub_agent_id, kCodexHistoryChild);
}

UAM_TEST(CodexChildMetadataChangesInvalidateMessageFingerprints)
{
	ChatSession chat;
	chat.messages = ChatHistorySyncService::ParseRemoteCodexTranscript(CodexRichHistory()).messages;
	auto& children = chat.messages.front().tool_calls.front().sub_agent_children;
	std::string previous = uam::StateSerializer::MessageDigest(chat);
	const auto changed = [&]
	{
		const std::string next = uam::StateSerializer::MessageDigest(chat);
		UAM_ASSERT(next != previous);
		previous = next;
	};
	children[1].status = "completed";
	changed();
	children[1].title = "Reviewer";
	changed();
	children[1].id = "880e8400-e29b-41d4-a716-446655440003";
	changed();
	std::swap(children[0], children[1]);
	changed();
	children.pop_back();
	changed();
}
#endif
