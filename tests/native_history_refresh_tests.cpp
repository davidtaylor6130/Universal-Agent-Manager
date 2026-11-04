#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
UAM_TEST(LocalClaudeRefreshLoadsExactSessionAndRejectsOtherWorkspace)
{
	TempDir temp("uam-exact-claude-refresh");
	ScopedEnvVar home("CLAUDE_CONFIG_DIR", (temp.root / "claude").string());
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	std::string encoded = workspace.string();
	for (char& value : encoded) if (!uam::strings::IsAsciiAlnum(static_cast<unsigned char>(value))) value = '-';
	const fs::path directory = temp.root / "claude" / "projects" / encoded;
	fs::create_directories(directory);
	const std::string id = "11111111-1111-4111-8111-111111111111";
	nlohmann::json user{{"uuid", "user"}, {"parentUuid", nullptr}, {"type", "user"}, {"cwd", workspace.string()},
	    {"timestamp", "2026-10-16T12:00:00Z"}, {"message", {{"content", "From native terminal"}}}};
	nlohmann::json assistant{{"uuid", "assistant"}, {"parentUuid", "user"}, {"type", "assistant"}, {"cwd", workspace.string()},
	    {"timestamp", "2026-10-16T12:00:01Z"}, {"message", {{"content", "Native answer"}}}};
	UAM_ASSERT(uam::io::WriteTextFile(directory / (id + ".jsonl"), user.dump() + "\n" + assistant.dump() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(directory / "unrelated.jsonl", "malformed unrelated transcript\n"));
	ChatSession chat;
	chat.provider_id = "claude-cli";
	chat.native_session_id = id;
	chat.workspace_directory = workspace.string();
	std::string error;
	auto loaded = ChatHistorySyncService().LoadLocalNativeTranscript(chat, &error);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(loaded->messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(loaded->messages.back().content, std::string("Native answer"));
	chat.workspace_directory = (temp.root / "other").string();
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(chat, &error));
	chat.workspace_directory = workspace.string();
	chat.native_session_id = "../escape";
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(chat, &error));
}
#endif

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
UAM_TEST(LocalCopilotRefreshUsesSavedSessionAndHostOwnership)
{
	TempDir temp("uam-exact-copilot-refresh");
	ScopedEnvVar home("COPILOT_HOME", (temp.root / "copilot").string());
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	const std::string id = "22222222-2222-4222-8222-222222222222";
	const fs::path session = temp.root / "copilot" / "session-state" / id;
	fs::create_directories(session);
	UAM_ASSERT(uam::io::WriteTextFile(session / "workspace.yaml", "cwd: " + nlohmann::json(workspace.string()).dump() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(session / "events.jsonl",
	    nlohmann::json({{"type", "user.message"}, {"data", {{"content", "Native user input"}}}}).dump() + "\n" +
	    nlohmann::json({{"type", "assistant.message"}, {"data", {{"content", "Native final answer"}}}}).dump() + "\n"));
	ChatSession chat;
	chat.provider_id = "copilot-cli";
	chat.native_session_id = id;
	chat.workspace_directory = workspace.string();
	std::string error;
	auto loaded = ChatHistorySyncService().LoadLocalNativeTranscript(chat, &error);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->native_session_id, id);
	UAM_ASSERT_EQ(loaded->messages.back().content, std::string("Native final answer"));
	chat.execution_host_id = "remote-host";
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(chat, &error));
	chat.execution_host_id = "local";
	chat.native_session_id = "not-the-session";
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(chat, &error));
}
#endif

UAM_TEST(NativeMessageEventReceiptsRoundTripWithContentAndRecovery)
{
	TempDir temp("uam-native-event-receipts");
	ChatSession chat;
	chat.id = "native-event-receipts";
	chat.provider_id = "copilot-cli";
	Message message;
	message.role = MessageRole::Assistant;
	message.content = "Already projected native text";
	message.provider_event_ids = {"native-session:event-one", "native-session:event-two"};
	chat.messages.push_back(message);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(loaded->messages.front().content, message.content);
	UAM_ASSERT_EQ(loaded->messages.front().provider_event_ids, message.provider_event_ids);
}

namespace
{
ChatSession RefreshChildFixture()
{
	ChatSession chat;
	chat.id = "refresh-child-metadata";
	chat.updated_at = "2026-10-16T12:00:00Z";
	Message message;
	message.role = MessageRole::Assistant;
	message.content = "Delegating native work";
	message.created_at = chat.updated_at;
	ToolCall tool;
	tool.id = "native-child-tool";
	tool.name = "spawn_agent";
	tool.is_sub_agent = true;
	tool.sub_agent_id = "660e8400-e29b-41d4-a716-446655440001";
	tool.sub_agent_title = "Saved reviewer";
	tool.sub_agent_children = {{tool.sub_agent_id, "Saved reviewer", "running"}, {"770e8400-e29b-41d4-a716-446655440002", "Saved researcher", "pendingInit"}};
	message.tool_calls.push_back(tool);
	chat.messages.push_back(message);
	return chat;
}
}

UAM_TEST(NativeRefreshRetainsOmittedChildrenWithMatchingSingularIdentity)
{
	for (const bool singular_omitted : {false, true})
	{
		for (const bool regrouped : {false, true})
		{
			const ChatSession local = RefreshChildFixture();
			ChatSession native = local;
			native.updated_at = "2026-10-16T12:00:01Z";
			ToolCall& tool = native.messages.front().tool_calls.front();
			tool.sub_agent_children.clear();
			tool.sub_agent_title.clear();
			if (singular_omitted) tool.sub_agent_id.clear();
			if (regrouped) native.messages.front().content = "Regrouped native export";
			ChatHistorySyncService().OverlayPreparedLocalHistory(local, native);
			const ToolCall& saved = local.messages.front().tool_calls.front();
			const ToolCall& overlaid = native.messages.front().tool_calls.front();
			UAM_ASSERT_EQ(overlaid.sub_agent_id, saved.sub_agent_id);
			UAM_ASSERT_EQ(overlaid.sub_agent_title, saved.sub_agent_title);
			UAM_ASSERT_EQ(overlaid.sub_agent_children, saved.sub_agent_children);
		}
	}
	ChatSession legacy = RefreshChildFixture();
	legacy.messages.front().tool_calls.front().sub_agent_children.clear();
	ChatSession native = legacy;
	native.messages.front().tool_calls.front().sub_agent_id.clear();
	native.messages.front().tool_calls.front().sub_agent_title.clear();
	ChatHistorySyncService().OverlayPreparedLocalHistory(legacy, native);
	UAM_ASSERT_EQ(native.messages.front().tool_calls.front().sub_agent_id, legacy.messages.front().tool_calls.front().sub_agent_id);
	UAM_ASSERT_EQ(native.messages.front().tool_calls.front().sub_agent_title, std::string("Saved reviewer"));
	UAM_ASSERT(native.messages.front().tool_calls.front().sub_agent_children.empty());
}

UAM_TEST(NativeRefreshDoesNotCopyChildrenAcrossIdentityOrAmbiguousTools)
{
	const ChatSession local = RefreshChildFixture();
	ChatSession native = local;
	ToolCall& different = native.messages.front().tool_calls.front();
	different.sub_agent_id = "880e8400-e29b-41d4-a716-446655440003";
	different.sub_agent_title.clear();
	different.sub_agent_children.clear();
	ChatHistorySyncService().OverlayPreparedLocalHistory(local, native);
	UAM_ASSERT_EQ(native.messages.front().tool_calls.front().sub_agent_id, std::string("880e8400-e29b-41d4-a716-446655440003"));
	UAM_ASSERT(native.messages.front().tool_calls.front().sub_agent_title.empty());
	UAM_ASSERT(native.messages.front().tool_calls.front().sub_agent_children.empty());

	for (const bool empty_id : {false, true})
	{
		ChatSession ambiguous = local;
		if (empty_id) ambiguous.messages.front().tool_calls.front().id.clear();
		else ambiguous.messages.front().tool_calls.push_back(ambiguous.messages.front().tool_calls.front());
		native = local;
		ToolCall& tool = native.messages.front().tool_calls.front();
		if (empty_id) tool.id.clear();
		tool.sub_agent_id.clear();
		tool.sub_agent_title.clear();
		tool.sub_agent_children.clear();
		ChatHistorySyncService().OverlayPreparedLocalHistory(ambiguous, native);
		UAM_ASSERT(native.messages.front().tool_calls.front().sub_agent_id.empty());
		UAM_ASSERT(native.messages.front().tool_calls.front().sub_agent_children.empty());
	}

	// An already populated normalized collection also wins for legacy adapters without a presence flag.
	native = local;
	native.messages.front().tool_calls.front().sub_agent_children = {{local.messages.front().tool_calls.front().sub_agent_id, "Native title", "completed"}};
	const auto expected = native.messages.front().tool_calls.front().sub_agent_children;
	ChatHistorySyncService().OverlayPreparedLocalHistory(local, native);
	UAM_ASSERT_EQ(native.messages.front().tool_calls.front().sub_agent_children, expected);
}

#if UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
nlohmann::json RefreshCollaborationItem()
{
	return {{"id", "native-child-tool"}, {"type", "collabAgentToolCall"}, {"tool", "spawnAgent"}, {"status", "completed"},
	    {"senderThreadId", "550e8400-e29b-41d4-a716-446655440000"},
	    {"receiverThreadIds", nlohmann::json::array({"660e8400-e29b-41d4-a716-446655440001", "770e8400-e29b-41d4-a716-446655440002"})},
	    {"agentsStates", {{"660e8400-e29b-41d4-a716-446655440001", {{"status", "completed"}}}}}};
}

ChatSession ParsedRefreshCollaboration(const nlohmann::json& item)
{
	const auto parsed = ChatHistorySyncService::ParseRemoteCodexTranscript({{"thread", {{"id", "550e8400-e29b-41d4-a716-446655440000"}, {"cwd", "/workspace"},
	    {"turns", nlohmann::json::array({{{"id", "native-turn"}, {"items", nlohmann::json::array({item})}}})}}}});
	UAM_ASSERT(parsed.success);
	UAM_ASSERT_EQ(parsed.messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(parsed.messages.front().tool_calls.size(), std::size_t{1});
	ChatSession native;
	native.id = "refresh-child-metadata";
	native.provider_id = "codex-cli";
	native.native_session_id = parsed.session_id;
	native.workspace_directory = parsed.directory;
	native.messages = parsed.messages;
	return native;
}
}

UAM_TEST(CodexNativeRefreshKeepsParsedReceiversAndCurrentStatuses)
{
	for (const bool reversed : {false, true})
	{
		auto item = RefreshCollaborationItem();
		if (reversed) std::swap(item["receiverThreadIds"][0], item["receiverThreadIds"][1]);
		ChatSession native = ParsedRefreshCollaboration(item);
		const ToolCall expected = native.messages.front().tool_calls.front();
		UAM_ASSERT(expected.sub_agent_children_authoritative);
		UAM_ASSERT_EQ(expected.sub_agent_children.size(), std::size_t{2});
		ChatSession local = native;
		local.messages.front().tool_calls = RefreshChildFixture().messages.front().tool_calls;
		ChatHistorySyncService().OverlayPreparedLocalHistory(local, native);
		UAM_ASSERT_EQ(native.messages.front().tool_calls.front(), expected);
	}
}

UAM_TEST(CodexNativeRefreshNeverResurrectsRejectedReceivers)
{
	std::vector<nlohmann::json> rejected = {nlohmann::json::array(), nullptr, 42, "not-an-array", nlohmann::json::object(),
	    nlohmann::json::array({false, 42, "not-a-thread", "550e8400-e29b-41d4-a716-446655440000", "urn:uuid:550E8400-E29B-41D4-A716-446655440000"})};
	for (std::size_t index = 0; index <= rejected.size(); ++index)
	{
		auto item = RefreshCollaborationItem();
		if (index == rejected.size()) item.erase("receiverThreadIds");
		else item["receiverThreadIds"] = rejected[index];
		ChatSession native = ParsedRefreshCollaboration(item);
		const ToolCall expected = native.messages.front().tool_calls.front();
		UAM_ASSERT(expected.sub_agent_children_authoritative && expected.sub_agent_id.empty() && expected.sub_agent_children.empty());
		ChatSession local = native;
		local.messages.front().tool_calls = RefreshChildFixture().messages.front().tool_calls;
		ChatHistorySyncService().OverlayPreparedLocalHistory(local, native);
		UAM_ASSERT_EQ(native.messages.front().tool_calls.front(), expected);
	}
}

UAM_TEST(CodexNativeRefreshPersistsRejectedAndOmittedChildMetadata)
{
	TempDir temp("uam-child-metadata-refresh-save");
	uam::AppState app;
	app.data_root = temp.root;
	for (const bool rejected : {false, true})
	{
		ChatSession current = RefreshChildFixture();
		current.provider_id = "codex-cli";
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, current));
		auto item = RefreshCollaborationItem();
		item["receiverThreadIds"] = nlohmann::json::array();
		ChatSession native = ParsedRefreshCollaboration(item);
		if (!rejected)
		{
			ToolCall& omitted = native.messages.front().tool_calls.front();
			omitted.sub_agent_children_authoritative = false;
			omitted.sub_agent_id = current.messages.front().tool_calls.front().sub_agent_id;
		}
		const auto saved_children = current.messages.front().tool_calls.front().sub_agent_children;
		UAM_ASSERT(ChatHistorySyncService().SaveNativeTranscript(app, current, native.messages));
		const auto restored = ChatRepository::LoadLocalChat(temp.root, current.id);
		UAM_ASSERT(restored.has_value());
		const ToolCall& actual = restored->messages.front().tool_calls.front();
		UAM_ASSERT_EQ(actual.sub_agent_children_authoritative, rejected);
		if (rejected) UAM_ASSERT(actual.sub_agent_id.empty() && actual.sub_agent_title.empty() && actual.sub_agent_children.empty());
		else UAM_ASSERT_EQ(actual.sub_agent_children, saved_children);
	}
}

UAM_TEST(CodexNativeChildAuthoritySurvivesStorageAcpAndCef)
{
	TempDir temp("uam-child-metadata-authority");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli);
	for (const bool empty : {false, true})
	{
		auto item = RefreshCollaborationItem();
		if (empty) item["receiverThreadIds"] = nlohmann::json::array();
		ChatSession native = ParsedRefreshCollaboration(item);
		const ToolCall& expected = native.messages.front().tool_calls.front();
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, native));
		const auto restored = ChatRepository::LoadLocalChat(temp.root, native.id);
		UAM_ASSERT(restored.has_value());
		UAM_ASSERT_EQ(restored->messages.front().tool_calls.front(), expected);
		const auto expected_children = uam::StateSerializer::SerializeSession(native)["messages"][0]["toolCalls"][0]["subAgentChildren"];
		UAM_ASSERT(expected_children.is_array());
		UAM_ASSERT_EQ(expected_children.size(), expected.sub_agent_children.size());
		UAM_ASSERT_EQ(uam::StateSerializer::SerializeMessagePage(*restored, 10, std::nullopt, true)["messages"][0]["toolCalls"][0]["subAgentChildren"], expected_children);
		ChatMessagePage detached;
		detached.chat = *restored;
		detached.total_count = restored->messages.size();
		UAM_ASSERT_EQ(uam::StateSerializer::SerializeDetachedMessagePage(detached, true)["messages"][0]["toolCalls"][0]["subAgentChildren"], expected_children);

		uam::AcpToolCallState acp;
		acp.id = item["id"];
		runtime.ApplyAcpToolMetadata(acp, item);
		UAM_ASSERT(acp.sub_agent_children_authoritative);
		const ToolCall converted = uam::acp_detail::PersistedToolCallFromAcpToolCall(acp);
		UAM_ASSERT(converted.sub_agent_children_authoritative);
		UAM_ASSERT_EQ(converted.sub_agent_children, expected.sub_agent_children);
		uam::AcpSessionState session;
		uam::acp_detail::RestoreTurnEventsFromMessageBlocks(session, restored->messages.front());
		UAM_ASSERT_EQ(session.tool_calls.size(), std::size_t{1});
		UAM_ASSERT(session.tool_calls.front().sub_agent_children_authoritative);
		uam::AppState app;
		app.chats.push_back(*restored);
		auto live = std::make_unique<uam::AcpSessionState>();
		live->chat_id = native.id;
		live->tool_calls.push_back(acp);
		app.acp_sessions.push_back(std::move(live));
		UAM_ASSERT_EQ(uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["toolCalls"][0]["subAgentChildren"], expected_children);

		ChatSession omitted = native;
		omitted.messages.front().tool_calls.front().sub_agent_children_authoritative = false;
		UAM_ASSERT(uam::StateSerializer::MessageDigest(omitted) != uam::StateSerializer::MessageDigest(native));
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, omitted));
		const auto legacy = ChatRepository::LoadLocalChat(temp.root, omitted.id);
		UAM_ASSERT(legacy && !legacy->messages.front().tool_calls.front().sub_agent_children_authoritative);
		UAM_ASSERT_EQ(legacy->messages.front().tool_calls.front(), omitted.messages.front().tool_calls.front());
	}
}
#endif

UAM_TEST(NativeRefreshRetainsNewMediaBesideEquivalentLocalMessage)
{
	TempDir temp("uam-native-refresh-equivalent-media");
	uam::AppState app; app.data_root = temp.root;
	ChatSession current;
	current.id = "equivalent-native-media"; current.provider_id = "gemini-cli";
	current.native_session_id = "native-media-session"; current.workspace_directory = temp.root.string();
	current.updated_at = "2026-10-16T12:00:00Z";
	Message message; message.role = MessageRole::User; message.content = "Same recorded text";
	message.created_at = current.updated_at; message.continues_turn = true;
	MessageAttachment local; local.id = "local-file"; local.name = "local.txt"; local.path = "/recorded/local.txt";
	message.attachments.push_back(local); current.messages.push_back(message);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, current));
	Message native = message; native.continues_turn = false; native.attachments.clear();
	MessageAttachment reference; reference.id = "recorded-part"; reference.name = "Recorded image";
	reference.mime_type = "image/png"; reference.source = AttachmentSource::ProviderUri;
	reference.source_uri = "provider://opaque/original"; reference.origin_execution_host_id = "local";
	reference.native_part_key = "native-session/message/part";
	native.attachments.push_back(reference);
	UAM_ASSERT(ChatHistorySyncService().SaveNativeTranscript(app, current, {native}));
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, current.id);
	UAM_ASSERT(loaded); UAM_ASSERT_EQ(loaded->messages.front().attachments.size(), std::size_t{2});
	UAM_ASSERT_EQ(loaded->messages.front().attachments[0], local);
	UAM_ASSERT_EQ(loaded->messages.front().attachments[1], reference);
	UAM_ASSERT(loaded->messages.front().continues_turn);
	// Refresh the same snapshot again: the recorded part replaces its prior projection.
	UAM_ASSERT(ChatHistorySyncService().SaveNativeTranscript(app, current, {native}));
	UAM_ASSERT_EQ(current.messages.front().attachments.size(), std::size_t{2});
	native.attachments.front().source_uri = "provider://opaque/revised";
	UAM_ASSERT(ChatHistorySyncService().SaveNativeTranscript(app, current, {native}));
	UAM_ASSERT_EQ(current.messages.front().attachments.size(), std::size_t{2});
	UAM_ASSERT_EQ(current.messages.front().attachments[1].source_uri, native.attachments.front().source_uri);
}

#if UAM_ENABLE_RUNTIME_CODEX_CLI
UAM_TEST(CodexUnknownAuthoritativeToolsCannotReplaceOtherSavedMessageFields)
{
	for (const int boundary : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10})
	{
		ChatSession native = ParsedRefreshCollaboration(RefreshCollaborationItem());
		ChatSession local = native;
		local.messages.front().tool_calls = RefreshChildFixture().messages.front().tool_calls;
		UAM_ASSERT(local.updated_at.empty() && native.updated_at.empty());
		UAM_ASSERT(native.messages.front().tool_calls.front().sub_agent_children_authoritative);
		if (boundary == 0) local.messages.front().content = "New saved authored text";
		if (boundary == 1) local.messages.front().thoughts = "Saved reasoning must survive";
		if (boundary == 2) local.messages.front().blocks.push_back({"assistant_text", "Saved ordered block", "", ""});
		if (boundary == 3) local.messages.front().plan_summary = "Saved plan must survive";
		if (boundary == 4)
		{
			MessageAttachment authored;
			authored.id = "saved-authored-file";
			authored.path = "/private-fixture-only/no-file-read";
			local.messages.front().attachments.push_back(std::move(authored));
		}
		if (boundary == 5) local.messages.front().tool_calls.front().args_json = "Saved invocation input";
		if (boundary == 6) local.messages.front().tool_calls.front().result_text = "Saved invocation output";
		if (boundary == 7) local.messages.front().tool_calls.front().task_id = "Saved UAM task authority";
		// Full persisted-message identity includes presentation metadata; this is not a replay witness.
		if (boundary == 8) local.messages.front().managed_origin_run_id = "5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7";
		if (boundary == 9) native.messages.front().managed_origin_run_id = "5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7";
		if (boundary == 10)
		{
			local.messages.front().managed_origin_run_id = "5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7";
			native.messages.front().managed_origin_run_id = "6ad39bc5-8084-4e5e-8bbf-d3974ecca5c7";
		}
		const auto expected = local.messages;
		ChatHistorySyncService().OverlayPreparedLocalHistory(local, native);
		UAM_ASSERT_EQ(native.messages, expected);
	}
	for (const std::string operation : {"spawn_agent", "spawnAgent", "multi_agent_v1.spawn_agent", "functions.spawn_agent"})
	{
		ChatSession native = ParsedRefreshCollaboration(RefreshCollaborationItem());
		const auto parsed_tool = native.messages.front().tool_calls.front();
		ChatSession local = native;
		local.updated_at = "recorded";
		native.updated_at = "2026-10-16T13:00:00Z";
		local.messages.front().tool_calls = RefreshChildFixture().messages.front().tool_calls;
		local.messages.front().tool_calls.front().name = operation;
		ChatHistorySyncService().OverlayPreparedLocalHistory(local, native);
		UAM_ASSERT_EQ(native.messages.front().tool_calls.front(), parsed_tool);
		UAM_ASSERT_EQ(native.updated_at, local.updated_at); // Receiver detail does not advance conversation chronology.
	}
}

UAM_TEST(CodexUnknownAuthoritativeToolsRequireUniqueMatchingInvocationAndUnknownAge)
{
	for (const int boundary : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10})
	{
		ChatSession native = ParsedRefreshCollaboration(RefreshCollaborationItem());
		ChatSession local = native;
		local.messages.front().tool_calls = RefreshChildFixture().messages.front().tool_calls;
		UAM_ASSERT(local.updated_at.empty() && native.updated_at.empty());
		if (boundary == 0) local.messages.front().tool_calls.front().id = "different-saved-invocation";
		if (boundary == 1)
		{
			local.messages.front().tool_calls.front().id.clear();
			native.messages.front().tool_calls.front().id.clear();
		}
		if (boundary == 2)
		{
			local.messages.front().tool_calls.push_back(local.messages.front().tool_calls.front());
			native.messages.front().tool_calls.push_back(native.messages.front().tool_calls.front());
		}
		if (boundary == 3) native.messages.front().tool_calls.front().sub_agent_children_authoritative = false;
		if (boundary == 4)
		{
			local.updated_at = "2026-10-16T13:00:00Z";
			native.updated_at = "2026-10-16T12:00:00Z";
		}
		if (boundary == 5) native.native_session_id = "880e8400-e29b-41d4-a716-446655440003";
		if (boundary == 6) local.messages.front().tool_calls.front().name = "exec_command";
		if (boundary == 7) local.messages.front().tool_calls.front().name = "read_file";
		if (boundary == 8) local.messages.front().tool_calls.front().name.clear();
		if (boundary == 9) native.messages.front().tool_calls.front().name.clear();
		if (boundary == 10)
		{
			local.provider_id = "claude-cli";
			native.provider_id = "claude-cli";
		}
		const auto expected = local.messages;
		ChatHistorySyncService().OverlayPreparedLocalHistory(local, native);
		UAM_ASSERT_EQ(native.messages, expected);
	}
}
#endif


#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
namespace
{
	struct NativeRefreshMaximumTestZone
	{
		std::unique_ptr<ScopedEnvVar> zone = std::make_unique<ScopedEnvVar>("TZ", "UTC0");
		static void Reload()
		{
#if defined(_WIN32)
			_tzset();
#else
			tzset();
#endif
		}
		NativeRefreshMaximumTestZone()
		{
			Reload();
		}
		~NativeRefreshMaximumTestZone()
		{
			zone.reset();
			Reload();
		}
	};

	void CheckClaudeNativeRefreshTimestampMaximum(bool newer_native)
	{
		NativeRefreshMaximumTestZone timezone;
		TempDir temp("uam-claude-native-timestamp-maximum");
		const auto private_home = temp.root / "os-home";
		const auto profile = temp.root / "claude";
		const auto workspace = temp.root / "workspace";
		fs::create_directories(private_home);
		fs::create_directories(workspace);
		ScopedEnvVar os_home("HOME", uam::paths::Utf8PathString(private_home));
		ScopedEnvVar os_profile("USERPROFILE", uam::paths::Utf8PathString(private_home));
		ScopedEnvVar claude_home("CLAUDE_CONFIG_DIR", uam::paths::Utf8PathString(profile));
		std::string encoded = workspace.string();
		for (char& value : encoded)
		{
			if (!uam::strings::IsAsciiAlnum(static_cast<unsigned char>(value))) value = '-';
		}
		const auto directory = profile / "projects" / encoded;
		fs::create_directories(directory);
		const std::string native_id = "11111111-1111-4111-8111-111111111111";
		const auto native_file = directory / (native_id + ".jsonl");
		const auto write_native = [&](const std::string& stamp, const std::string& answer)
		{
			const nlohmann::json user{{"uuid", "max-user"}, {"parentUuid", nullptr}, {"type", "user"}, {"cwd", workspace.string()},
			    {"timestamp", "2026-10-16T09:00:00Z"}, {"message", {{"content", "Private native maximum question"}}}};
			const nlohmann::json assistant{{"uuid", "max-assistant"}, {"parentUuid", "max-user"}, {"type", "assistant"}, {"cwd", workspace.string()},
			    {"timestamp", stamp}, {"message", {{"content", answer}}}};
			const auto bytes = user.dump() + "\n" + assistant.dump() + "\n";
			UAM_ASSERT(uam::io::WriteTextFile(native_file, bytes));
			return bytes;
		};
		write_native("2026-10-16T12:00:00Z", "Initial actual native answer");
		ChatSession source;
		source.provider_id = "claude-cli";
		source.native_session_id = native_id;
		source.workspace_directory = workspace.string();
		std::string error;
		auto initial = ChatHistorySyncService().LoadLocalNativeTranscript(source, &error);
		UAM_ASSERT(initial && error.empty() && initial->messages_loaded && initial->messages.size() == 2);
		UAM_ASSERT_EQ(initial->native_session_id, native_id);
		UAM_ASSERT_EQ(initial->workspace_directory, workspace.string());
		const auto saved_epoch = std::chrono::sys_days{std::chrono::year{2026}/10/16} + std::chrono::hours{14};
		const auto saved_local = uam::time::detail::FormatTimestamp(uam::time::detail::LocalTimeSnapshot(std::chrono::system_clock::to_time_t(saved_epoch)), uam::time::kLocalDisplayTimestampFormat);
		UAM_ASSERT_EQ(saved_local, std::string("2026-10-16 14:00:00"));
		const auto incoming_epoch = saved_epoch + std::chrono::hours{newer_native ? 1 : -1};
		UAM_ASSERT(newer_native ? incoming_epoch > saved_epoch : incoming_epoch < saved_epoch);
		const auto incoming_iso = uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(std::chrono::system_clock::to_time_t(incoming_epoch)), uam::time::kIsoUtcTimestampFormat);
		UAM_ASSERT_EQ(incoming_iso, newer_native ? std::string("2026-10-16T15:00:00.000Z") : std::string("2026-10-16T13:00:00.000Z"));
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatFolder folder;
		folder.id = "native-maximum-folder";
		folder.directory = workspace.string();
		folder.execution_host_id = "local";
		app.folders.push_back(folder);
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		ChatSession owner = *initial; // Actual reader identity becomes the genuine saved imported owner.
		owner.title = "Keep native maximum title";
		owner.folder_id = folder.id;
		owner.pinned = true;
		owner.model_id = "saved-native-maximum-model";
		owner.updated_at = saved_local;
		owner.messages.back().content = "Saved answer before explicit native refresh";
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, owner));
		const auto saved_before = ChatRepository::LoadLocalChat(app.data_root, owner.id, true);
		UAM_ASSERT(saved_before && saved_before->messages_loaded && saved_before->messages.size() == 2);
		UAM_ASSERT_EQ(saved_before->updated_at, saved_local);
		app.chats = {*saved_before};
		ChatDomainService().SelectChatById(app, owner.id);
		app.composer_text = "Keep native maximum draft";
		app.resolved_native_sessions_by_chat_id[owner.id] = native_id;
		UAM_ASSERT(app.cli_terminals.empty() && app.acp_sessions.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.worktree_operation_chat_ids.empty());
		const auto native_bytes = write_native(incoming_iso, "Accepted explicit native maximum answer");
		const auto incoming = ChatHistorySyncService().LoadLocalNativeTranscript(app.chats.front(), &error);
		UAM_ASSERT(incoming && error.empty() && incoming->messages_loaded && incoming->messages.size() == saved_before->messages.size());
		UAM_ASSERT_EQ(incoming->native_session_id, native_id);
		UAM_ASSERT_EQ(incoming->provider_id, source.provider_id);
		UAM_ASSERT_EQ(incoming->workspace_directory, workspace.string());
		UAM_ASSERT_EQ(incoming->messages.back().created_at, incoming_iso);
		UAM_ASSERT_EQ(incoming->messages.back().content, std::string("Accepted explicit native maximum answer"));
		const auto folders_before = ChatFolderStore::Serialize(app.folders);
		UAM_ASSERT(ChatHistorySyncService().SaveNativeTranscript(app, app.chats.front(), incoming->messages));
		const auto durable = ChatRepository::LoadLocalChat(app.data_root, owner.id, true);
		const auto summary = ChatRepository::LoadLocalChat(app.data_root, owner.id, false);
		const auto* live = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(durable && durable->messages_loaded && durable->messages.size() == 2 && summary && live && live->messages_loaded);
		// Explicit refresh keeps native graph authority in BOTH arms; an older
		// native message must not be blocked merely to retain newer metadata time.
		UAM_ASSERT_EQ(durable->messages.back().content, std::string("Accepted explicit native maximum answer"));
		UAM_ASSERT_EQ(durable->messages.back().created_at, incoming_iso);
		UAM_ASSERT_EQ(live->messages, durable->messages);
		for (const auto* current : {&*durable, &*summary, live})
		{
			UAM_ASSERT_EQ(current->id, owner.id);
			UAM_ASSERT_EQ(current->native_session_id, native_id);
			UAM_ASSERT_EQ(current->provider_id, owner.provider_id);
			UAM_ASSERT_EQ(current->execution_host_id, owner.execution_host_id);
			UAM_ASSERT_EQ(current->workspace_directory, owner.workspace_directory);
			UAM_ASSERT_EQ(current->title, owner.title);
			UAM_ASSERT_EQ(current->folder_id, owner.folder_id);
			UAM_ASSERT_EQ(current->model_id, owner.model_id);
			UAM_ASSERT(current->pinned);
		}
		UAM_ASSERT_EQ(app.composer_text, std::string("Keep native maximum draft"));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), owner.id);
		UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(owner.id), native_id);
		UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(native_file), native_bytes);
		// Intended narrow metadata oracle follows actual content/save controls.
		const auto expected_maximum = newer_native ? incoming_iso : saved_local;
		UAM_ASSERT_EQ(durable->updated_at, expected_maximum);
		UAM_ASSERT_EQ(live->updated_at, expected_maximum);
		UAM_ASSERT_EQ(summary->updated_at, expected_maximum);
	}
}

UAM_TEST(ClaudeNativeRefreshKeepsNewerSavedTimestampWhileAcceptingNativeContent)
{
	CheckClaudeNativeRefreshTimestampMaximum(false);
}

UAM_TEST(ClaudeNativeRefreshAdvancesTimestampForNewerNativeContent)
{
	CheckClaudeNativeRefreshTimestampMaximum(true);
}
#endif

UAM_TEST(NativeRefreshMetadataBoundariesKeepExplicitGraphAndDurableState)
{
	struct Case
	{
		std::string saved;
		std::string incoming;
		std::string expected;
	};
	const std::vector<Case> cases = {
	    {"recorded", "2026-10-16T15:00:00Z", "recorded"},
	    {"2026-10-16T14:00:00Z", "", "2026-10-16T14:00:00Z"},
	    {"2026-10-16T14:00:00Z", "recorded", "2026-10-16T14:00:00Z"},
	    {"2026-10-16T13:00:00Z", "2026-10-16T14:00:00+01:00", "2026-10-16T13:00:00Z"},
	    {"2026-10-16T13:00:00.123456788Z", "2026-10-16T13:00:00.123456789Z", "2026-10-16T13:00:00.123456789Z"},
	    {"2026-10-16T13:00:00.123456789Z", "2026-10-16T13:00:00.123456788Z", "2026-10-16T13:00:00.123456789Z"},
	};
	for (const auto& entry : cases)
	{
		TempDir temp("uam-native-metadata-boundary");
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		const auto workspace = temp.root / "workspace";
		fs::create_directories(workspace);
		ChatFolder folder{"metadata-boundary-folder", "Metadata boundary", workspace.string(), false, "local"};
		app.folders.push_back(folder);
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		ChatSession owner;
		owner.id = "metadata-boundary-owner";
		owner.provider_id = "claude-cli";
		owner.native_session_id = "11111111-1111-4111-8111-111111111111";
		owner.execution_host_id = "local";
		owner.workspace_directory = workspace.string();
		owner.folder_id = folder.id;
		owner.title = "Preserve metadata boundary title";
		owner.pinned = true;
		owner.created_at = "2026-10-16T09:00:00Z";
		owner.updated_at = entry.saved;
		owner.messages = {{MessageRole::User, "Saved boundary question"}, {MessageRole::Assistant, "Saved boundary answer"}};
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, owner));
		const auto before = ChatRepository::LoadLocalChat(app.data_root, owner.id, true);
		UAM_ASSERT(before && before->messages_loaded && before->messages.size() == 2);
		// Each persisted baseline is genuine and retains the intended timestamp;
		// empty saved dates are tested separately because repository reads fill them.
		UAM_ASSERT_EQ(before->updated_at, entry.saved);
		app.chats = {*before};
		ChatDomainService().SelectChatById(app, owner.id);
		app.composer_text = "Preserve boundary draft";
		app.resolved_native_sessions_by_chat_id[owner.id] = owner.native_session_id;
		const auto folders_before = ChatFolderStore::Serialize(app.folders);
		const auto mappings_before = app.resolved_native_sessions_by_chat_id;
		std::vector<Message> incoming = before->messages;
		incoming.back().content = "Explicit native graph adopted despite metadata age";
		incoming.back().created_at = entry.incoming;
		UAM_ASSERT(ChatHistorySyncService().SaveNativeTranscript(app, app.chats.front(), incoming));
		const auto durable = ChatRepository::LoadLocalChat(app.data_root, owner.id, true);
		const auto summary = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, owner.id);
		const auto* live = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(durable && durable->messages_loaded && durable->messages.size() == 2 && summary && live && live->messages_loaded);
		UAM_ASSERT_EQ(durable->messages, incoming);
		UAM_ASSERT_EQ(live->messages, incoming);
		for (const auto* current : {&*durable, &*summary, live})
		{
			UAM_ASSERT_EQ(current->id, owner.id);
			UAM_ASSERT_EQ(current->native_session_id, owner.native_session_id);
			UAM_ASSERT_EQ(current->provider_id, owner.provider_id);
			UAM_ASSERT_EQ(current->execution_host_id, owner.execution_host_id);
			UAM_ASSERT_EQ(current->workspace_directory, owner.workspace_directory);
			UAM_ASSERT_EQ(current->folder_id, owner.folder_id);
			UAM_ASSERT_EQ(current->title, owner.title);
			UAM_ASSERT(current->pinned);
		}
		UAM_ASSERT_EQ(app.composer_text, std::string("Preserve boundary draft"));
		UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id, mappings_before);
		UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), owner.id);
		// Metadata assertions follow actual native graph/save/owner controls.
		UAM_ASSERT_EQ(durable->updated_at, entry.expected);
		UAM_ASSERT_EQ(live->updated_at, entry.expected);
		UAM_ASSERT_EQ(summary->updated_at, entry.expected);
	}
}

UAM_TEST(NativeRefreshEmptyMetadataCandidateBootstrapsOnlyValidObservedTime)
{
	for (const bool valid : {false, true})
	{
		// This public detached-candidate boundary is intentionally NOT a durable
		// empty-timestamp claim: repository reads supply a stable date fallback.
		ChatSession candidate;
		candidate.id = "empty-metadata-detached-candidate";
		candidate.messages_loaded = true;
		candidate.messages = {{MessageRole::User, "Existing detached question"}, {MessageRole::Assistant, "Existing detached answer"}};
		UAM_ASSERT(candidate.updated_at.empty());
		const std::string observed = valid ? "2026-10-16T15:00:00.123456789Z" : "recorded";
		auto incoming = candidate.messages;
		incoming.back().content = "Explicit detached native answer";
		incoming.back().created_at = observed;
		UAM_ASSERT(ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, incoming));
		UAM_ASSERT(candidate.messages_loaded);
		UAM_ASSERT_EQ(candidate.messages, incoming);
		UAM_ASSERT_EQ(candidate.persisted_message_count, incoming.size());
		UAM_ASSERT_EQ(candidate.updated_at, valid ? observed : std::string{});
	}
}
