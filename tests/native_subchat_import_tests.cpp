#include "test_harness.h"
#include "app/opencode_import_stream.h"
#include "app/startup_native_import_service.h"
#include "common/chat/native_chat_identity.h"
#include "common/provider/copilot_native_history.h"
#include "common/provider/native_agent_history.h"

#include <thread>

using namespace uam_test;

// Every history here is a synthetic fixture written by the test; no real provider history is read.
namespace
{
	const std::string kParentId = "11111111-1111-4111-8111-111111111111";
	const std::string kChildId = "22222222-2222-4222-8222-222222222222";
	const std::string kGrandchildId = "33333333-3333-4333-8333-333333333333";

	struct SubchatFixture
	{
		TempDir temp{"uam-native-subchat-import"};
		fs::path workspace = temp.root / "workspace";
		uam::AppState app;

		SubchatFixture()
		{
			fs::create_directories(workspace);
			app.data_root = temp.root / "data";
			app.folders.push_back({"known", "Known", uam::paths::Utf8PathString(workspace), false, "local"});
			// Fixtures carry fixed dates; these cases are about lineage, not the startup age limit.
			app.settings.native_import_startup_max_age_days = 0;
		}

		ChatSession History(const std::string& id, const std::string& parent = {}, const std::string& provider = "opencode-cli") const
		{
			ChatSession chat;
			chat.id = chat.native_session_id = chat.branch_root_chat_id = id;
			chat.provider_id = provider;
			chat.execution_host_id = "local";
			chat.workspace_directory = uam::paths::Utf8PathString(workspace);
			chat.title = "Fixture " + id;
			chat.created_at = chat.updated_at = "2026-11-01T10:00:00Z";
			chat.native_parent_session_id = parent;
			chat.messages.push_back(Message{MessageRole::User, "Fixture request for " + id});
			chat.messages.push_back(Message{MessageRole::Assistant, "Fixture answer for " + id});
			return chat;
		}

		static uam::StartupNativeImportService::Scanner Scanner(std::vector<ChatSession> chats)
		{
			return [chats = std::move(chats)](const uam::StartupNativeImportScanRequest&, std::stop_token)
			{
				ChatHistorySyncService::LocalHistoryDiscovery discovery;
				discovery.chats = chats;
				return discovery;
			};
		}

		void Wait(uam::StartupNativeImportService& service)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (service.Snapshot().phase == "scanning" || service.ImportPending())
			{
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
				service.Poll(app);
				std::this_thread::yield();
			}
		}

		uam::StartupNativeImportSnapshot Scan(uam::StartupNativeImportService& service)
		{
			service.Initialize(app);
			Wait(service);
			return service.Snapshot();
		}

		/// Scans, selects every listed history and waits for the import.
		uam::StartupNativeImportSnapshot Import(uam::StartupNativeImportService& service)
		{
			const auto ready = Scan(service);
			std::string error;
			UAM_ASSERT(service.Decide(app, ready.generation, {}, {}, &error, {}, true));
			Wait(service);
			return service.Snapshot();
		}

		const ChatSession* Chat(const std::string& native_or_chat_id) const
		{
			const auto found = std::ranges::find_if(app.chats, [&](const ChatSession& chat) { return chat.id == native_or_chat_id || chat.native_session_id == native_or_chat_id; });
			return found == app.chats.end() ? nullptr : &*found;
		}

		void AssertLinked(const std::string& child_id, const std::string& parent_id) const
		{
			const ChatSession* child = Chat(child_id);
			const ChatSession* parent = Chat(parent_id);
			UAM_ASSERT(child != nullptr && parent != nullptr);
			UAM_ASSERT_EQ(child->subagent_parent_chat_id, parent->id);
			UAM_ASSERT_EQ(child->subagent_parent_created_at, parent->created_at);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, child->id, true);
			UAM_ASSERT(saved);
			UAM_ASSERT_EQ(saved->subagent_parent_chat_id, parent->id);
			UAM_ASSERT_EQ(saved->subagent_parent_created_at, parent->created_at);
			UAM_ASSERT(!saved->messages.empty());
		}
	};

	std::string Joined(const std::vector<std::string>& errors)
	{
		std::string text;
		for (const std::string& error : errors) text += error + "\n";
		return text;
	}

	std::string JsonLines(const std::vector<nlohmann::json>& rows)
	{
		std::string text;
		for (const nlohmann::json& row : rows) text += row.dump() + "\n";
		return text;
	}
}

UAM_TEST(NativeSubchatImportFollowsParentLinksNestedChildrenAndIsIdempotent)
{
	SubchatFixture fixture;
	const std::vector<ChatSession> found = {fixture.History("ses_grandchild", "ses_child"), fixture.History("ses_child", "ses_parent"),
	    fixture.History("ses_parent"), fixture.History("ses_other")};
	{
		uam::StartupNativeImportService service(SubchatFixture::Scanner(found));
		const auto ready = fixture.Scan(service);
		// Only root sessions are reviewed; a sub-chat follows its parent's decision.
		UAM_ASSERT_EQ(ready.total_count, std::size_t{2});
		for (const auto& candidate : ready.candidates) UAM_ASSERT(candidate.native_session_id == "ses_parent" || candidate.native_session_id == "ses_other");
		const auto parent_row = std::ranges::find_if(ready.candidates, [](const auto& candidate) { return candidate.native_session_id == "ses_parent"; });
		std::string error;
		UAM_ASSERT(service.Decide(fixture.app, ready.generation, {parent_row->candidate_id}, {}, &error));
		fixture.Wait(service);
		UAM_ASSERT_EQ(Joined(service.Snapshot().errors), std::string{});
		UAM_ASSERT_EQ(service.Snapshot().imported_count, 3);
	}
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{3});
	UAM_ASSERT(fixture.Chat("ses_other") == nullptr);
	UAM_ASSERT(fixture.Chat("ses_parent")->subagent_parent_chat_id.empty());
	fixture.AssertLinked("ses_child", "ses_parent");
	fixture.AssertLinked("ses_grandchild", "ses_child");
	// The parent hint is discovery data only; the saved link is the UAM parent, never a native ID.
	UAM_ASSERT(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.Chat("ses_child")->id)).find("native_parent_session_id") == std::string::npos);

	// A second scan finds the same sessions: nothing is offered twice or duplicated.
	uam::StartupNativeImportService again(SubchatFixture::Scanner(found));
	const auto rescanned = fixture.Scan(again);
	UAM_ASSERT_EQ(rescanned.total_count, std::size_t{1});
	UAM_ASSERT_EQ(rescanned.candidates.front().native_session_id, std::string("ses_other"));
	std::string error;
	UAM_ASSERT(again.Decide(fixture.app, rescanned.generation, {}, {}, &error, {}, true));
	fixture.Wait(again);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{4});
	fixture.AssertLinked("ses_child", "ses_parent");
}

UAM_TEST(NativeSubchatImportLinksChildrenAdmittedAfterTheirParentWasPublished)
{
	// The import stages eight histories per pass, so most of these children are admitted after the parent is already saved.
	SubchatFixture fixture;
	std::vector<ChatSession> found;
	for (int index = 0; index < 20; ++index) found.push_back(fixture.History("ses_child_" + std::to_string(index), "ses_parent"));
	found.insert(found.begin() + 7, fixture.History("ses_parent"));
	uam::StartupNativeImportService service(SubchatFixture::Scanner(found));
	const auto imported = fixture.Import(service);
	UAM_ASSERT_EQ(Joined(imported.errors), std::string{});
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{21});
	for (int index = 0; index < 20; ++index) fixture.AssertLinked("ses_child_" + std::to_string(index), "ses_parent");
}

UAM_TEST(NativeSubchatImportRespectsTombstonesDeclinesAndMissingParents)
{
	{
		// A deleted child stays deleted when its parent is imported.
		SubchatFixture fixture;
		std::vector<std::string> added;
		UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(fixture.app.data_root, {fixture.History("ses_child")}, added));
		uam::StartupNativeImportService service(SubchatFixture::Scanner({fixture.History("ses_child", "ses_parent"), fixture.History("ses_kept", "ses_parent"), fixture.History("ses_parent")}));
		(void)fixture.Import(service);
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
		UAM_ASSERT(fixture.Chat("ses_child") == nullptr);
		fixture.AssertLinked("ses_kept", "ses_parent");
	}
	{
		// Declining the parent imports nothing, and the child is not offered on its own afterwards.
		SubchatFixture fixture;
		const std::vector<ChatSession> found = {fixture.History("ses_child", "ses_parent"), fixture.History("ses_parent")};
		uam::StartupNativeImportService service(SubchatFixture::Scanner(found));
		const auto ready = fixture.Scan(service);
		UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
		std::string error;
		UAM_ASSERT(service.Decide(fixture.app, ready.generation, {}, {ready.candidates.front().candidate_id}, &error));
		fixture.Wait(service);
		UAM_ASSERT(fixture.app.chats.empty());
		uam::StartupNativeImportService again(SubchatFixture::Scanner(found));
		UAM_ASSERT_EQ(fixture.Scan(again).total_count, std::size_t{0});
		UAM_ASSERT(fixture.app.chats.empty());
	}
	{
		// A child whose parent is not in the scan is neither a top-level row nor declined.
		SubchatFixture fixture;
		uam::StartupNativeImportService service(SubchatFixture::Scanner({fixture.History("ses_child", "ses_missing_parent")}));
		UAM_ASSERT_EQ(fixture.Scan(service).total_count, std::size_t{0});
		UAM_ASSERT(fixture.app.chats.empty());
		const auto tombstones = ChatHistorySyncService().NativeImportTombstoneKeys(fixture.app.data_root);
		UAM_ASSERT(tombstones && tombstones->empty());
	}
}

UAM_TEST(NativeSubchatImportRelinksSavedOrphanWhenItsParentIsImported)
{
	SubchatFixture fixture;
	// An earlier import saved this child as an unrelated top-level chat.
	ChatSession orphan = fixture.History("ses_child");
	orphan.id = "chat-earlier-import";
	orphan.folder_id = "known";
	orphan.title = "Kept title";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, orphan));
	ChatHistorySyncService().LoadSidebarChats(fixture.app);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});

	uam::StartupNativeImportService service(SubchatFixture::Scanner({fixture.History("ses_child", "ses_parent"), fixture.History("ses_parent")}));
	const auto ready = fixture.Scan(service);
	UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
	// No parent chat exists yet, so the saved child is still unlinked.
	UAM_ASSERT(fixture.Chat("chat-earlier-import")->subagent_parent_chat_id.empty());
	std::string error;
	UAM_ASSERT(service.Decide(fixture.app, ready.generation, {}, {}, &error, {}, true));
	fixture.Wait(service);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	fixture.AssertLinked("chat-earlier-import", "ses_parent");
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, "chat-earlier-import", true);
	UAM_ASSERT_EQ(saved->title, std::string("Kept title"));
	UAM_ASSERT_EQ(saved->messages.size(), orphan.messages.size());
}

UAM_TEST(NativeSubchatManualScanBackfillsChildrenOfSavedParentsButStartupDoesNot)
{
	SubchatFixture fixture;
	// The parent is already a UAM chat: imported earlier, or started in UAM.
	ChatSession parent = fixture.History("ses_parent");
	parent.id = "chat-saved-parent";
	parent.folder_id = "known";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, parent));
	ChatHistorySyncService().LoadSidebarChats(fixture.app);
	std::vector<std::string> added;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(fixture.app.data_root, {fixture.History("ses_deleted")}, added));
	const std::vector<ChatSession> found = {fixture.History("ses_grandchild", "ses_child"), fixture.History("ses_child", "ses_parent"),
	    fixture.History("ses_deleted", "ses_parent"), fixture.History("ses_parent"), fixture.History("ses_orphan", "ses_unknown_parent")};
	{
		// A startup scan never adds chats nobody asked for.
		uam::StartupNativeImportService startup(SubchatFixture::Scanner(found));
		UAM_ASSERT_EQ(fixture.Scan(startup).total_count, std::size_t{0});
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	}
	for (int pass = 0; pass < 2; ++pass)
	{
		// A scan the user starts imports the missing sub-chats without listing them; a second one changes nothing.
		uam::StartupNativeImportService manual(SubchatFixture::Scanner(found));
		std::string error;
		UAM_ASSERT(manual.DiscoverLocalHistory(fixture.app, &error));
		fixture.Wait(manual);
		const auto done = manual.Snapshot();
		UAM_ASSERT_EQ(Joined(done.errors), std::string{});
		UAM_ASSERT_EQ(done.total_count, std::size_t{0});
		UAM_ASSERT_EQ(done.imported_count, pass == 0 ? 2 : 0);
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{3});
		UAM_ASSERT(fixture.Chat("ses_deleted") == nullptr && fixture.Chat("ses_orphan") == nullptr);
		fixture.AssertLinked("ses_child", "chat-saved-parent");
		fixture.AssertLinked("ses_grandchild", "ses_child");
	}
}

UAM_TEST(NativeSubchatLineageRequiresTheParentTheChildNames)
{
	SubchatFixture fixture;
	ChatSession parent = fixture.History("ses_parent");
	ChatSession child = fixture.History("ses_child", "ses_parent");
	UAM_ASSERT(ChatRepository::ApplyImportedNativeChildLineage(parent, child));
	UAM_ASSERT_EQ(child.subagent_parent_chat_id, parent.id);
	// An existing link is never moved to another parent.
	ChatSession other = fixture.History("ses_other");
	child.native_parent_session_id = "ses_other";
	UAM_ASSERT(!ChatRepository::ApplyImportedNativeChildLineage(other, child));
	UAM_ASSERT_EQ(child.subagent_parent_chat_id, parent.id);

	for (int mismatch = 0; mismatch < 5; ++mismatch)
	{
		ChatSession source = fixture.History("ses_parent");
		ChatSession candidate = fixture.History("ses_child", "ses_parent");
		if (mismatch == 0) candidate.native_parent_session_id = "ses_someone_else";
		if (mismatch == 1) candidate.native_parent_session_id.clear();
		if (mismatch == 2) candidate.provider_id = "codex-cli";
		if (mismatch == 3) candidate.execution_host_id = "another-host";
		if (mismatch == 4) candidate.workspace_directory = uam::paths::Utf8PathString(fixture.temp.root / "elsewhere");
		UAM_ASSERT(!ChatRepository::ApplyImportedNativeChildLineage(source, candidate));
		UAM_ASSERT(candidate.subagent_parent_chat_id.empty() && candidate.subagent_parent_created_at.empty());
	}
	// The explicit-open rule is unchanged: without a named parent, the parent's loaded tool calls must name the child.
	ChatSession unnamed = fixture.History("ses_child");
	UAM_ASSERT(!ChatRepository::ApplyNativeChildDisplayLineage(parent, unnamed, "ses_child"));
}

#if UAM_ENABLE_RUNTIME_CODEX_CLI
UAM_TEST(NativeSubchatImportCodexRolloutsNameTheirParentThread)
{
	SubchatFixture fixture;
	const fs::path codex = fixture.temp.root / "codex";
	ScopedEnvVar codex_home{"CODEX_HOME", codex.string()};
	const std::string workspace = uam::paths::Utf8PathString(fixture.workspace);
	const std::string review_id = "44444444-4444-4444-8444-444444444444";
	const auto message = [](const char* role, const std::string& text, const char* second)
	{
		return nlohmann::json{{"timestamp", std::string("2026-11-01T10:00:") + second + "Z"}, {"type", "response_item"},
		    {"payload", {{"type", "message"}, {"role", role}, {"content", nlohmann::json::array({{{"type", role == std::string("user") ? "input_text" : "output_text"}, {"text", text}}})}}}};
	};
	const auto meta = [&](const std::string& id, nlohmann::json extra)
	{
		nlohmann::json payload{{"id", id}, {"timestamp", "2026-11-01T10:00:00Z"}, {"cwd", workspace}, {"originator", "codex_cli_rs"}, {"cli_version", "0.60.0"}};
		payload.update(extra);
		return nlohmann::json{{"timestamp", "2026-11-01T10:00:00Z"}, {"type", "session_meta"}, {"payload", payload}};
	};
	const auto spawned = [](const std::string& parent, int depth)
	{
		return nlohmann::json{{"parent_thread_id", parent}, {"thread_source", "subagent"}, {"agent_nickname", "Ada"}, {"agent_role", "explorer"},
		    {"source", {{"subagent", {{"thread_spawn", {{"parent_thread_id", parent}, {"depth", depth}, {"agent_nickname", "Ada"}, {"agent_role", "explorer"}}}}}}}};
	};
	const fs::path day = codex / "sessions" / "2026" / "11" / "01";
	// The parent records the spawn as a function call whose output carries the child thread ID.
	UAM_ASSERT(uam::io::WriteTextFile(day / ("rollout-2026-11-01T10-00-00-" + kParentId + ".jsonl"), JsonLines({
	    meta(kParentId, {{"source", "cli"}}), message("user", "Parent request", "01"),
	    {{"timestamp", "2026-11-01T10:00:02Z"}, {"type", "response_item"}, {"payload", {{"type", "function_call"}, {"name", "spawn_agent"}, {"call_id", "call_spawn"}, {"arguments", R"({"message":"Inspect the tests"})"}}}},
	    {{"timestamp", "2026-11-01T10:00:03Z"}, {"type", "response_item"}, {"payload", {{"type", "function_call_output"}, {"call_id", "call_spawn"}, {"output", nlohmann::json{{"agent_id", kChildId}, {"nickname", "Ada"}}.dump()}}}},
	    message("assistant", "Parent answer", "04")})));
	UAM_ASSERT(uam::io::WriteTextFile(day / ("rollout-2026-11-01T10-00-05-" + kChildId + ".jsonl"), JsonLines({
	    meta(kChildId, spawned(kParentId, 1)), message("user", "Inspect the tests", "05"), message("assistant", "Child answer", "06")})));
	// An older child rollout names its parent only inside the spawn source.
	nlohmann::json older = spawned(kChildId, 2);
	older.erase("parent_thread_id");
	older.erase("thread_source");
	UAM_ASSERT(uam::io::WriteTextFile(day / ("rollout-2026-11-01T10-00-07-" + kGrandchildId + ".jsonl"), JsonLines({
	    meta(kGrandchildId, older), message("user", "Check one file", "07"), message("assistant", "Grandchild answer", "08")})));
	// Review, compaction and memory subagent threads are not conversations to import.
	UAM_ASSERT(uam::io::WriteTextFile(day / ("rollout-2026-11-01T10-00-09-" + review_id + ".jsonl"), JsonLines({
	    meta(review_id, {{"thread_source", "subagent"}, {"source", {{"subagent", "review"}}}}), message("user", "Review this", "09"), message("assistant", "Review answer", "10")})));

	ChatFolder all;
	const auto discovery = ChatHistorySyncService().DiscoverProviderChatsForFolder(all, nullptr, {}, 0, false, {}, {}, "codex-cli");
	UAM_ASSERT_EQ(Joined(discovery.result.errors), std::string{});
	UAM_ASSERT_EQ(discovery.codex_rollouts.size(), std::size_t{3});
	for (const auto& [chat, path] : discovery.codex_rollouts)
	{
		UAM_ASSERT(chat.native_session_id != review_id);
		UAM_ASSERT_EQ(chat.native_parent_session_id, chat.native_session_id == kChildId ? kParentId : chat.native_session_id == kGrandchildId ? kChildId : std::string{});
	}

	uam::StartupNativeImportService service([](const uam::StartupNativeImportScanRequest&, std::stop_token stop)
	{
		return ChatHistorySyncService().DiscoverProviderChatsForFolder({}, nullptr, stop, 0, false, {}, {}, "codex-cli");
	});
	const auto ready = fixture.Scan(service);
	UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
	UAM_ASSERT_EQ(ready.candidates.front().native_session_id, kParentId);
	std::string error;
	UAM_ASSERT(service.Decide(fixture.app, ready.generation, {}, {}, &error, {}, true));
	fixture.Wait(service);
	UAM_ASSERT_EQ(Joined(service.Snapshot().errors), std::string{});
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{3});
	fixture.AssertLinked(kChildId, kParentId);
	fixture.AssertLinked(kGrandchildId, kChildId);
	const auto child = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat(kChildId)->id, true);
	UAM_ASSERT_EQ(child->messages.back().content, std::string("Child answer"));
	UAM_ASSERT(!child->imported_read_only);
	// The spawning tool call in the parent names the same child the sub-chat was imported as.
	const auto parent = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat(kParentId)->id, true);
	bool spawn_names_child = false;
	for (const Message& row : parent->messages)
		for (const ToolCall& tool : row.tool_calls) spawn_names_child = spawn_names_child || (tool.id == "call_spawn" && tool.sub_agent_id == child->native_session_id);
	UAM_ASSERT(spawn_names_child);
}

UAM_TEST(NativeSubchatImportRemoteCodexListKeepsSpawnedThreadsWithTheirParent)
{
	const nlohmann::json listed = {{"data", nlohmann::json::array({
	    {{"id", kChildId}, {"cwd", "/work/project"}, {"preview", "Inspect the tests"}, {"createdAt", 1793527200}, {"updatedAt", 1793527260},
	        {"parentThreadId", kParentId}, {"source", {{"subAgent", {{"thread_spawn", {{"parent_thread_id", kParentId}, {"depth", 1}}}}}}}},
	    {{"id", kGrandchildId}, {"cwd", "/work/project"}, {"preview", "Not a thread parent"}, {"createdAt", 1793527200}, {"updatedAt", 1793527260}, {"parentThreadId", "not-a-thread-id"}},
	    {{"id", "44444444-4444-4444-8444-444444444444"}, {"cwd", "/work/project"}, {"preview", "Review thread"}, {"createdAt", 1793527200}, {"updatedAt", 1793527260},
	        {"parentThreadId", kParentId}, {"source", {{"subAgent", "review"}}}},
	    {{"id", "55555555-5555-4555-8555-555555555555"}, {"cwd", "/work/project"}, {"preview", "Parented without a source"}, {"createdAt", 1793527200}, {"updatedAt", 1793527260}, {"parentThreadId", kParentId}},
	    {{"id", kParentId}, {"cwd", "/work/project"}, {"preview", "Parent request"}, {"createdAt", 1793527100}, {"updatedAt", 1793527300}}})}};
	std::vector<ChatHistorySyncService::RemoteCodexSession> sessions;
	std::string error;
	UAM_ASSERT(ChatHistorySyncService::AppendRemoteCodexSessions(listed, sessions, &error));
	UAM_ASSERT_EQ(sessions.size(), std::size_t{2});
	UAM_ASSERT_EQ(sessions[0].id, kChildId);
	UAM_ASSERT_EQ(sessions[0].parent_thread_id, kParentId);
	UAM_ASSERT(sessions[1].id == kParentId && sessions[1].parent_thread_id.empty());
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
UAM_TEST(NativeSubchatImportClaudeSubagentJournalsFollowTheirSession)
{
	SubchatFixture fixture;
	ScopedEnvVar claude{"CLAUDE_CONFIG_DIR", (fixture.temp.root / "claude").string()};
	const std::string workspace = uam::paths::Utf8PathString(fixture.workspace);
	const std::string agent = "a1b2c3d4e5f60718", missing_agent = "a0000000000000ff";
	std::string encoded = workspace;
	for (char& value : encoded) if (!uam::strings::IsAsciiAlnum(static_cast<unsigned char>(value))) value = '-';
	const fs::path project = fixture.temp.root / "claude" / "projects" / encoded;
	const auto row = [&](const char* type, const char* uuid, const char* previous, nlohmann::json content, const std::string& sidechain_agent = {})
	{
		nlohmann::json record{{"type", type}, {"uuid", uuid}, {"parentUuid", previous == nullptr ? nlohmann::json(nullptr) : nlohmann::json(previous)},
		    {"sessionId", kParentId}, {"isSidechain", !sidechain_agent.empty()}, {"cwd", workspace}, {"timestamp", "2026-11-01T10:00:00Z"},
		    {"message", {{"role", type}, {"content", std::move(content)}}}};
		if (!sidechain_agent.empty()) record["agentId"] = sidechain_agent;
		return record;
	};
	const auto spawn = [](const char* id, const char* description)
	{
		return nlohmann::json{{"type", "tool_use"}, {"id", id}, {"name", "Task"}, {"input", {{"description", description}, {"prompt", "Do the work"}, {"subagent_type", "general-purpose"}}}};
	};
	auto first_result = row("user", "p3", "p2", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "toolu_spawn"}, {"content", "Child finished"}}}));
	first_result["toolUseResult"] = {{"status", "completed"}, {"agentId", agent}};
	auto second_result = row("user", "p5", "p4", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "toolu_gone"}, {"content", "Child finished"}}}));
	second_result["toolUseResult"] = {{"status", "completed"}, {"agentId", missing_agent}};
	UAM_ASSERT(uam::io::WriteTextFile(project / (kParentId + ".jsonl"), JsonLines({
	    row("user", "p1", nullptr, "Parent request"), row("assistant", "p2", "p1", nlohmann::json::array({spawn("toolu_spawn", "Inspect the tests")})), first_result,
	    row("assistant", "p4", "p3", nlohmann::json::array({spawn("toolu_gone", "Cleaned up journal")})), second_result,
	    row("assistant", "p6", "p5", "Parent answer")})));
	UAM_ASSERT(uam::io::WriteTextFile(project / kParentId / "subagents" / ("agent-" + agent + ".jsonl"), JsonLines({
	    row("user", "c1", nullptr, "Inspect the tests", agent), row("assistant", "c2", "c1", "Child answer", agent)})));
	const std::string child_id = uam::native_agents::BuildHistoryId("claude-cli", kParentId, agent, "local");

	uam::StartupNativeImportService service([](const uam::StartupNativeImportScanRequest&, std::stop_token stop)
	{
		return ChatHistorySyncService().DiscoverProviderChatsForFolder({}, nullptr, stop, 0, false, {}, {}, "claude-cli");
	});
	const auto ready = fixture.Scan(service);
	UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
	std::string error;
	UAM_ASSERT(service.Decide(fixture.app, ready.generation, {}, {}, &error, {}, true));
	fixture.Wait(service);
	// The journal that no longer exists is reported by name; the parent and the readable child still import.
	const auto errors = service.Snapshot().errors;
	UAM_ASSERT_EQ(errors.size(), std::size_t{1});
	UAM_ASSERT(errors.front().find("Cleaned up journal") != std::string::npos);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	fixture.AssertLinked(child_id, kParentId);
	const auto child = ChatRepository::LoadLocalChat(fixture.app.data_root, child_id, true);
	UAM_ASSERT(child->imported_read_only && child->native_session_id.empty());
	UAM_ASSERT_EQ(child->parent_chat_id, kParentId);
	UAM_ASSERT_EQ(child->messages.back().content, std::string("Child answer"));
	const auto parent = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat(kParentId)->id, true);
	UAM_ASSERT_EQ(parent->messages[1].tool_calls.front().id, std::string("toolu_spawn"));
	UAM_ASSERT_EQ(parent->messages[1].tool_calls.front().sub_agent_id, child_id);

	// A second scan offers nothing and leaves one copy of each chat.
	uam::StartupNativeImportService again([](const uam::StartupNativeImportScanRequest&, std::stop_token stop)
	{
		return ChatHistorySyncService().DiscoverProviderChatsForFolder({}, nullptr, stop, 0, false, {}, {}, "claude-cli");
	});
	UAM_ASSERT_EQ(fixture.Scan(again).total_count, std::size_t{0});
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
UAM_TEST(NativeSubchatImportGeminiSubagentFilesFollowTheirSession)
{
	SubchatFixture fixture;
	ScopedEnvVar gemini{"GEMINI_CLI_HOME", (fixture.temp.root / "gemini").string()};
	const std::string workspace = uam::paths::Utf8PathString(fixture.workspace);
	const fs::path chats = fixture.temp.root / "gemini" / ".gemini" / "tmp" / "project" / "chats";
	UAM_ASSERT(uam::io::WriteTextFile(chats.parent_path() / ".project_root", workspace));
	// The parent's tool call records the child session ID as agentId; the child file sits in a folder named after the parent.
	UAM_ASSERT(uam::io::WriteTextFile(chats / (kParentId + ".json"), nlohmann::json{{"sessionId", kParentId}, {"startTime", "2026-11-01T10:00:00Z"}, {"lastUpdated", "2026-11-01T10:05:00Z"},
	    {"messages", nlohmann::json::array({{{"id", "m1"}, {"timestamp", "2026-11-01T10:00:00Z"}, {"type", "user"}, {"content", "Parent request"}},
	        {{"id", "m2"}, {"timestamp", "2026-11-01T10:05:00Z"}, {"type", "gemini"}, {"content", "Parent answer"},
	            {"toolCalls", nlohmann::json::array({{{"id", "call_spawn"}, {"name", "codebase_investigator"}, {"status", "success"}, {"args", {{"objective", "Inspect the tests"}}},
	                {"result", "Child finished"}, {"agentId", kChildId}, {"resultDisplay", {{"agentName", "Codebase Investigator"}, {"state", "completed"}}}}})}}})}}.dump()));
	UAM_ASSERT(uam::io::WriteTextFile(chats / kParentId / (kChildId + ".json"), nlohmann::json{{"sessionId", kChildId}, {"kind", "subagent"}, {"startTime", "2026-11-01T10:01:00Z"},
	    {"lastUpdated", "2026-11-01T10:04:00Z"}, {"directories", nlohmann::json::array({workspace})},
	    {"messages", nlohmann::json::array({{{"id", "c1"}, {"timestamp", "2026-11-01T10:01:00Z"}, {"type", "user"}, {"content", "Inspect the tests"}},
	        {{"id", "c2"}, {"timestamp", "2026-11-01T10:04:00Z"}, {"type", "gemini"}, {"content", "Child answer"}}})}}.dump()));
	const std::string child_id = uam::native_agents::BuildHistoryId("gemini-cli", kParentId, kChildId, "local");

	uam::StartupNativeImportService service([](const uam::StartupNativeImportScanRequest&, std::stop_token stop)
	{
		return ChatHistorySyncService().DiscoverProviderChatsForFolder({}, nullptr, stop, 0, false, {}, {}, "gemini-cli");
	});
	const auto imported = fixture.Import(service);
	UAM_ASSERT_EQ(Joined(imported.errors), std::string{});
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	fixture.AssertLinked(child_id, kParentId);
	const auto child = ChatRepository::LoadLocalChat(fixture.app.data_root, child_id, true);
	UAM_ASSERT(child->imported_read_only && child->native_session_id.empty());
	UAM_ASSERT_EQ(child->messages.back().content, std::string("Child answer"));
	const auto parent = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat(kParentId)->id, true);
	UAM_ASSERT_EQ(parent->messages.back().tool_calls.front().sub_agent_id, child_id);
}
#endif

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
UAM_TEST(NativeSubchatImportCopilotSubagentEventsBecomeReadOnlySubchats)
{
	SubchatFixture fixture;
	ScopedEnvVar home("COPILOT_HOME", (fixture.temp.root / "copilot").string());
	const std::string workspace = uam::paths::Utf8PathString(fixture.workspace);
	const std::string agent = "native-agent-1";
	// Copilot keeps a subagent's events in the parent's own journal, tagged with its agent ID and spawning tool call.
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot" / "session-state" / kParentId / "events.jsonl", JsonLines({
	    {{"id", "start"}, {"type", "session.start"}, {"timestamp", "2026-11-01T10:00:00Z"}, {"data", {{"sessionId", kParentId}, {"startTime", "2026-11-01T10:00:00Z"}, {"context", {{"cwd", workspace}}}}}},
	    {{"id", "root-user"}, {"type", "user.message"}, {"timestamp", "2026-11-01T10:00:01Z"}, {"data", {{"content", "Parent request"}}}},
	    {{"id", "root-tool"}, {"type", "tool.execution_start"}, {"timestamp", "2026-11-01T10:00:02Z"}, {"data", {{"toolCallId", "spawn"}, {"toolName", "task"}}}},
	    {{"id", "agent-start"}, {"type", "subagent.started"}, {"timestamp", "2026-11-01T10:00:03Z"}, {"agentId", agent}, {"data", {{"toolCallId", "spawn"}, {"agentName", "reviewer"}, {"agentDisplayName", "Reviewer"}}}},
	    {{"id", "agent-tool"}, {"type", "tool.execution_start"}, {"timestamp", "2026-11-01T10:00:04Z"}, {"agentId", agent}, {"data", {{"toolCallId", "read"}, {"parentToolCallId", "spawn"}, {"toolName", "view"}, {"arguments", {{"path", "fixture.txt"}}}}}},
	    {{"id", "agent-result"}, {"type", "tool.execution_complete"}, {"timestamp", "2026-11-01T10:00:05Z"}, {"agentId", agent}, {"data", {{"toolCallId", "read"}, {"success", true}, {"result", {{"content", "Fixture file text"}}}}}},
	    {{"id", "agent-text"}, {"type", "assistant.message"}, {"timestamp", "2026-11-01T10:00:06Z"}, {"agentId", agent}, {"data", {{"messageId", "answer"}, {"parentToolCallId", "spawn"}, {"content", "Child answer"}}}},
	    {{"id", "agent-done"}, {"type", "subagent.completed"}, {"timestamp", "2026-11-01T10:00:07Z"}, {"agentId", agent}, {"data", {{"toolCallId", "spawn"}, {"agentName", "reviewer"}}}},
	    {{"id", "root-text"}, {"type", "assistant.message"}, {"timestamp", "2026-11-01T10:00:08Z"}, {"data", {{"messageId", "root-answer"}, {"content", "Parent answer"}}}}})));
	const std::string child_id = BuildCopilotAgentHistoryId(kParentId, agent, "local");

	uam::StartupNativeImportService service([](const uam::StartupNativeImportScanRequest&, std::stop_token stop)
	{
		return ChatHistorySyncService().DiscoverProviderChatsForFolder({}, nullptr, stop, 0, false, {}, {}, "copilot-cli");
	});
	const auto imported = fixture.Import(service);
	UAM_ASSERT_EQ(Joined(imported.errors), std::string{});
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	fixture.AssertLinked(child_id, kParentId);
	const auto child = ChatRepository::LoadLocalChat(fixture.app.data_root, child_id, true);
	UAM_ASSERT(child->imported_read_only && child->native_session_id.empty());
	UAM_ASSERT_EQ(child->messages.back().content, std::string("Child answer"));
	UAM_ASSERT_EQ(child->messages.back().tool_calls.front().result_text, std::string("Fixture file text"));
}
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
namespace
{
	/// Stands in for the OpenCode CLI: `session list` prints the root sessions, `export <id>` that session's export.
	struct OpenCodeCliFixture
	{
		SubchatFixture base;
		ProviderProfile profile = ProviderProfileStore::DefaultOpenCodeProfile();

		OpenCodeCliFixture()
		{
#if defined(_WIN32)
			const fs::path executable = base.temp.root / "fixture.cmd";
			const std::string script = "@echo off\r\nfor %%a in (%*) do if exist \"export-%%~a.json\" (type \"export-%%~a.json\" & exit /b 0)\r\n"
			    "for %%a in (%*) do if \"%%~a\"==\"list\" (type list.json & exit /b 0)\r\nexit /b 1\r\n";
#else
			const fs::path executable = base.temp.root / "fixture";
			const std::string script = "#!/bin/sh\nfor a in \"$@\"; do [ -f \"export-$a.json\" ] && exec /bin/cat \"export-$a.json\"; done\n"
			    "for a in \"$@\"; do [ \"$a\" = list ] && exec /bin/cat list.json; done\nexit 1\n";
#endif
			UAM_ASSERT(uam::io::WriteTextFile(executable, script));
			fs::permissions(executable, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
			profile.interactive_command = uam::shell::JoinEscapedArgs({uam::paths::Utf8PathString(executable)});
			base.app.provider_profiles = {profile};
		}

		nlohmann::json Text(const char* role, const std::string& text) const
		{
			nlohmann::json info{{"role", role}, {"time", {{"created", 1793527200000}, {"completed", 1793527201000}}}};
			if (role == std::string("assistant")) info.update({{"providerID", "openai"}, {"modelID", "fixture"}});
			return {{"info", info}, {"parts", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}};
		}

		void WriteExport(const std::string& id, const std::string& parent, nlohmann::json messages) const
		{
			nlohmann::json info{{"id", id}, {"directory", uam::paths::Utf8PathString(base.workspace)}, {"title", "Session " + id}, {"time", {{"created", 1793527200000}, {"updated", 1793527260000}}}};
			if (!parent.empty()) info["parentID"] = parent;
			UAM_ASSERT(uam::io::WriteTextFile(base.workspace / ("export-" + id + ".json"), nlohmann::json{{"info", info}, {"messages", std::move(messages)}}.dump()));
		}

		/// The parent's task tool part names its child session in state.metadata, as OpenCode exports it.
		nlohmann::json Task(const std::string& child, const char* key = "sessionId") const
		{
			nlohmann::json message = Text("assistant", "Parent answer");
			message["parts"].insert(message["parts"].begin(), nlohmann::json{{"type", "tool"}, {"callID", "call_" + child}, {"tool", "task"},
			    {"state", {{"status", "completed"}, {"title", "Inspect the tests"}, {"input", {{"description", "Inspect the tests"}, {"prompt", "Inspect the tests."}, {"subagent_type", "general"}}},
			        {"output", "Child finished"}, {"metadata", {{key, child}}}}}});
			return message;
		}

		uam::StartupNativeImportService::Scanner Scanner() const
		{
			return [profile = profile, workspace = uam::paths::Utf8PathString(base.workspace)](const uam::StartupNativeImportScanRequest&, std::stop_token stop)
			{
				ChatFolder folder;
				folder.directory = workspace;
				return ChatHistorySyncService().DiscoverProviderChatsForFolder(folder, &profile, stop, 0, false, {}, {}, "opencode-cli");
			};
		}
	};
}

UAM_TEST(NativeSubchatImportOpenCodeTaskCallsNameChildSessions)
{
	OpenCodeCliFixture fixture;
	const std::string workspace = uam::paths::Utf8PathString(fixture.base.workspace);
	// OpenCode's session list returns root sessions only; children are reachable only through the parent's task call.
	UAM_ASSERT(uam::io::WriteTextFile(fixture.base.workspace / "list.json", nlohmann::json::array({
	    {{"id", "ses_parent"}, {"title", "Parent session"}, {"directory", workspace}, {"created", 1793527200000}, {"updated", 1793527260000}}}).dump()));
	fixture.WriteExport("ses_parent", {}, nlohmann::json::array({fixture.Text("user", "Parent request"), fixture.Task("ses_child"), fixture.Task("ses_v2child", "sessionID"), fixture.Task("ses_foreign")}));
	fixture.WriteExport("ses_child", "ses_parent", nlohmann::json::array({fixture.Text("user", "Inspect the tests."), fixture.Text("assistant", "Child answer")}));
	fixture.WriteExport("ses_v2child", "ses_parent", nlohmann::json::array({fixture.Text("user", "Inspect the tests."), fixture.Text("assistant", "Second child answer")}));
	// This session exists but names another parent, so it is not this chat's sub-chat.
	fixture.WriteExport("ses_foreign", "ses_someone_else", nlohmann::json::array({fixture.Text("user", "Other work"), fixture.Text("assistant", "Other answer")}));

	uam::StartupNativeImportService service(fixture.Scanner());
	const auto ready = fixture.base.Scan(service);
	UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
	std::string error;
	UAM_ASSERT(service.Decide(fixture.base.app, ready.generation, {}, {}, &error, {}, true));
	fixture.base.Wait(service);
	const auto errors = service.Snapshot().errors;
	UAM_ASSERT_EQ(errors.size(), std::size_t{1});
	UAM_ASSERT(errors.front().find("ses_foreign") != std::string::npos);
	UAM_ASSERT_EQ(fixture.base.app.chats.size(), std::size_t{3});
	UAM_ASSERT(fixture.base.Chat("ses_foreign") == nullptr);
	fixture.base.AssertLinked("ses_child", "ses_parent");
	fixture.base.AssertLinked("ses_v2child", "ses_parent");
	const auto child = ChatRepository::LoadLocalChat(fixture.base.app.data_root, fixture.base.Chat("ses_child")->id, true);
	UAM_ASSERT(!child->imported_read_only);
	UAM_ASSERT_EQ(child->native_session_id, std::string("ses_child"));
	UAM_ASSERT_EQ(child->messages.back().content, std::string("Child answer"));
	UAM_ASSERT(!child->created_at.empty());
}

UAM_TEST(NativeSubchatImportOpenCodeExportsReportTheirParent)
{
	OpenCodeCliFixture fixture;
	fixture.WriteExport("ses_child", "ses_parent", nlohmann::json::array({fixture.Text("user", "Inspect the tests."), fixture.Text("assistant", "Child answer")}));
	ChatSession identity = fixture.base.History("ses_child");
	std::string error, parent = "unset";
	UAM_ASSERT(uam::opencode_import::Read(fixture.base.workspace / "export-ses_child.json", identity, [](const Message&) { return true; }, {}, error, &parent));
	UAM_ASSERT_EQ(parent, std::string("ses_parent"));
	// The remote reader parses the same export and carries the same parent ID.
	const auto remote = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(ReadFile(fixture.base.workspace / "export-ses_child.json"));
	UAM_ASSERT_EQ(remote.parent_session_id, std::string("ses_parent"));
	fixture.WriteExport("ses_root", {}, nlohmann::json::array({fixture.Text("user", "Root request"), fixture.Text("assistant", "Root answer")}));
	identity = fixture.base.History("ses_root");
	UAM_ASSERT(uam::opencode_import::Read(fixture.base.workspace / "export-ses_root.json", identity, [](const Message&) { return true; }, {}, error, &parent));
	UAM_ASSERT(parent.empty());
}
#endif
