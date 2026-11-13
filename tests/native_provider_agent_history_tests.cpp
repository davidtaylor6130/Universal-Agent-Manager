#include "test_harness.h"
#include "app/native_agent_history_service.h"
#include "common/provider/native_provider_agent_history.h"
#include "remote/native_history_snapshot.h"

using namespace uam_test;

namespace
{
struct NativeProviderAgentFixture
{
	TempDir temp{"uam-exact-provider-agents"};
	ScopedEnvVar claude{"CLAUDE_CONFIG_DIR", (temp.root / "claude").string()};
	ScopedEnvVar gemini{"GEMINI_CLI_HOME", (temp.root / "gemini").string()};
	const std::string parent = "44444444-4444-4444-8444-444444444444";
	const std::string claude_agent = "a1234567890abcdef";
	const std::string gemini_agent = "abc123";
	const std::string workspace = uam::paths::Utf8PathString(temp.root / "workspace");
	std::string claude_text, gemini_text;
	fs::path claude_child, gemini_child;
	NativeProviderAgentFixture()
	{
		fs::create_directories(uam::paths::PathFromUtf8(workspace));
		std::string encoded = workspace;
		for (char& value : encoded) if (!uam::strings::IsAsciiAlnum(static_cast<unsigned char>(value))) value = '-';
		const fs::path project = temp.root / "claude" / "projects" / encoded;
		const auto record = [&](const char* type, const char* uuid, const char* previous, nlohmann::json content)
		{
			return nlohmann::json{{"type", type}, {"uuid", uuid}, {"parentUuid", previous == nullptr ? nlohmann::json(nullptr) : nlohmann::json(previous)},
			    {"sessionId", parent}, {"agentId", claude_agent}, {"isSidechain", true}, {"cwd", workspace},
			    {"timestamp", "2026-10-16T12:00:00Z"}, {"message", {{"content", std::move(content)}}}};
		};
		const auto user = record("user", "u", nullptr, "Native child question");
		claude_text = user.dump() + "\n" + record("assistant", "a", "u", nlohmann::json::array({
		    {{"type", "thinking"}, {"thinking", "Native child reasoning"}},
		    {{"type", "tool_use"}, {"id", "read"}, {"name", "Read"}, {"input", {{"file_path", "source.cpp"}}}}})).dump() + "\n" +
		    record("user", "r", "a", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "read"}, {"content", "Source file contents"}}})).dump() + "\n" +
		    record("assistant", "f", "r", "Exact Claude child answer").dump() + "\n";
		claude_child = project / parent / "subagents" / ("agent-" + claude_agent + ".jsonl");
		UAM_ASSERT(uam::io::WriteTextFile(claude_child, claude_text));
		auto root_user = user; root_user["isSidechain"] = false; root_user.erase("agentId");
		UAM_ASSERT(uam::io::WriteTextFile(project / (parent + ".jsonl"), root_user.dump() + "\n"));
		const fs::path chats = temp.root / "gemini" / ".gemini" / "tmp" / "project" / "chats";
		UAM_ASSERT(uam::io::WriteTextFile(chats.parent_path() / ".project_root", workspace));
		UAM_ASSERT(uam::io::WriteTextFile(chats / (parent + ".json"), nlohmann::json{{"sessionId", parent},
		    {"messages", nlohmann::json::array({{{"type", "user"}, {"content", "Native parent"}}})}}.dump()));
		gemini_text = nlohmann::json{{"sessionId", gemini_agent}, {"kind", "subagent"}, {"directories", nlohmann::json::array({workspace})},
		    {"messages", nlohmann::json::array({{{"type", "user"}, {"content", "Native child question"}},
		        {{"type", "gemini"}, {"content", "Exact Gemini child answer"}, {"thoughts", nlohmann::json::array({"Native child reasoning"})},
		            {"toolCalls", nlohmann::json::array({{{"id", "read"}, {"name", "read_file"}, {"status", "success"}, {"args", {{"path", "source.cpp"}}}, {"result", "Source file contents"}}})}}})}}.dump();
		gemini_child = chats / parent / (gemini_agent + ".json");
		UAM_ASSERT(uam::io::WriteTextFile(gemini_child, gemini_text));
	}
	uam::native_agents::HistoryReference Reference(const std::string& provider, const std::string& host = "local") const
	{
		return {provider, parent, provider == "claude-cli" ? claude_agent : gemini_agent, host};
	}
};
}

UAM_TEST(NativeAgentReopenKeepsSamePassiveTargetAcrossPreparation)
{
	NativeProviderAgentFixture fixture;
	for (const ProviderProfile profile : {ProviderProfileStore::DefaultClaudeProfile(), ProviderProfileStore::DefaultGeminiProfile(), ProviderProfileStore::DefaultCopilotProfile()})
	for (const std::string host_id : {"local", "fixture-ssh"})
	{
		uam::AppState app;
		app.data_root = fixture.temp.root / "reopen" / profile.id / host_id;
		app.provider_profiles = {profile};
		ExecutionHost host;
		host.id = "fixture-ssh"; host.transport = "ssh"; host.ssh_alias = "isolated-native-agent-fixture";
		// The fixture reuses this controller's real workspace path, so the host platform must match its syntax.
#if defined(_WIN32)
		host.platform = "windows";
#else
		host.platform = "linux";
#endif
		app.settings.execution_hosts = {uam::execution_hosts::LocalHost(), host};
		ChatSession source;
		source.id = "source-parent"; source.provider_id = profile.id; source.native_session_id = fixture.parent;
		source.workspace_directory = fixture.workspace; source.execution_host_id = host_id;
		app.chats.push_back(source);
		const std::string reference = uam::native_agents::BuildHistoryId(profile.id, fixture.parent, "actual-native-child", host_id);
		UAM_ASSERT(!reference.empty());
		ChatSession child;
		child.id = reference; child.provider_id = profile.id; child.parent_chat_id = fixture.parent;
		child.workspace_directory = fixture.workspace; child.execution_host_id = host_id; child.imported_read_only = true;
		child.messages.push_back({MessageRole::Assistant, "Native child response"});
		const ChatHistorySyncService history;
		UAM_ASSERT(history.FindInMemoryNativeSessionChatForOpen(app, source, profile, reference, false) == nullptr);
		UAM_ASSERT(history.FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &child, true) != nullptr);
		UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.back()));
		// CEF captures its target by the requested history reference before preparation.
		ChatSession* initial = history.FindInMemoryNativeSessionChatForOpen(app, source, profile, reference, false);
		UAM_ASSERT(initial != nullptr && initial->native_session_id.empty());
		const auto incarnation = initial->metadata_incarnation;
		const std::string initial_id = initial->id;
		uam::AppState isolated;
		isolated.data_root = app.data_root; isolated.settings = app.settings; isolated.provider_profiles = {profile};
		isolated.chats = {source, *initial};
		child.messages.front().content = "Actual refreshed native response";
		ChatSession* prepared = history.FindOrImportNativeSessionChatForOpen(isolated, source, profile, reference, false, &child, true);
		UAM_ASSERT(prepared != nullptr && prepared->native_session_id.empty());
		// CEF's completion guard looks up the prepared passive snapshot by its UAM ID.
		ChatSession* current = history.FindInMemoryNativeSessionChatForOpen(app, source, profile, prepared->id, false);
		UAM_ASSERT(current == initial);
		UAM_ASSERT_EQ(current->id, initial_id);
		UAM_ASSERT(history.FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, prepared, true) == initial);
		UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
		UAM_ASSERT_EQ(initial->metadata_incarnation, incarnation);
		UAM_ASSERT_EQ(initial->messages.front().content, std::string("Actual refreshed native response"));
		UAM_ASSERT(initial->native_session_id.empty() && initial->imported_read_only);
		UAM_ASSERT(!app.resolved_native_sessions_by_chat_id.contains(reference));
	}
}

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
UAM_TEST(ClaudeExactChildHistoryIsRichPassiveAndExcludedFromBulkImport)
{
	NativeProviderAgentFixture fixture;
	const auto reference = fixture.Reference("claude-cli");
	std::string error;
	const auto loaded = uam::claude::LoadNativeAgentHistory(reference, fixture.workspace, &error);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->imported_read_only && loaded->native_session_id.empty());
	UAM_ASSERT_EQ(loaded->parent_chat_id, fixture.parent);
	UAM_ASSERT_EQ(loaded->messages.back().content, std::string("Exact Claude child answer"));
	UAM_ASSERT_EQ(loaded->messages[1].thoughts, std::string("Native child reasoning"));
	UAM_ASSERT_EQ(loaded->messages[1].tool_calls[0].result_text, std::string("Source file contents"));
	UAM_ASSERT(uam::native_agents::ParseHistoryId(loaded->id).has_value());
	ChatFolder folder; folder.directory = fixture.workspace;
	const auto discovery = ChatHistorySyncService().DiscoverProviderChatsForFolder(folder, nullptr, {}, 0, false, {}, {}, "claude-cli");
	UAM_ASSERT_EQ(discovery.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(discovery.chats.front().native_session_id, fixture.parent);
	auto foreign = reference; foreign.agent_id = "different-agent";
	UAM_ASSERT(!uam::claude::ParseNativeAgentHistory(foreign, fixture.claude_text, fixture.workspace, &error));
	foreign = reference; foreign.parent_session_id = "55555555-5555-4555-8555-555555555555";
	UAM_ASSERT(!uam::claude::ParseNativeAgentHistory(foreign, fixture.claude_text, fixture.workspace, &error));
	UAM_ASSERT(!uam::claude::LoadNativeAgentHistory(reference, fixture.workspace + "-foreign", &error));
	std::stop_source cancel; cancel.request_stop();
	UAM_ASSERT(!uam::claude::ParseNativeAgentHistory(reference, fixture.claude_text, fixture.workspace, &error, cancel.get_token()));
}

UAM_TEST(ClaudeNativeAgentResultsAndTaskEventsLinkOnlyExactOwnedChild)
{
	NativeProviderAgentFixture fixture;
	uam::AppState app; app.data_root = fixture.temp.root / "data";
	ChatSession chat; chat.id = "claude-owned-child"; chat.provider_id = "claude-cli"; chat.native_session_id = fixture.parent; chat.execution_host_id = "fixture-ssh";
	uam::AcpSessionState session; session.chat_id = chat.id; session.provider_id = chat.provider_id; session.session_id = fixture.parent; session.running = true; session.processing = true;
	uam::AcpToolCallState tool; tool.id = "agent-tool"; tool.title = "Agent"; tool.is_sub_agent = true; session.tool_calls.push_back(tool);
	const auto& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
	nlohmann::json result{{"type", "user"}, {"session_id", fixture.parent}, {"parent_tool_use_id", nullptr},
	    {"tool_use_result", {{"agentId", fixture.claude_agent}}},
	    {"message", {{"content", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "agent-tool"}, {"content", "Native child finished"}}})}}}};
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, result, nullptr));
	auto reference = uam::native_agents::ParseHistoryId(session.tool_calls.front().sub_agent_id);
	UAM_ASSERT(reference.has_value());
	UAM_ASSERT_EQ(reference->parent_session_id, fixture.parent);
	UAM_ASSERT_EQ(reference->agent_id, fixture.claude_agent);
	UAM_ASSERT_EQ(reference->execution_host_id, chat.execution_host_id);
	nlohmann::json task{{"type", "system"}, {"subtype", "task_started"}, {"session_id", fixture.parent}, {"task_id", "a0987654321abcdef"},
	    {"task_type", "local_agent"}, {"tool_use_id", "agent-tool"}};
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, task, nullptr));
	reference = uam::native_agents::ParseHistoryId(session.tool_calls.front().sub_agent_id);
	UAM_ASSERT(reference.has_value());
	UAM_ASSERT_EQ(reference->agent_id, std::string("a0987654321abcdef"));
	const auto owned = session.tool_calls.front().sub_agent_id;
	task["session_id"] = "55555555-5555-4555-8555-555555555555";
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, task, nullptr));
	UAM_ASSERT_EQ(session.tool_calls.front().sub_agent_id, owned);
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
UAM_TEST(GeminiExactChildHistoryUsesParentDirectoryAndNeverInventsResumeId)
{
	NativeProviderAgentFixture fixture;
	const auto reference = fixture.Reference("gemini-cli");
	std::string error;
	const auto loaded = uam::gemini::LoadNativeAgentHistory(reference, fixture.workspace, &error);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->imported_read_only && loaded->native_session_id.empty());
	UAM_ASSERT_EQ(loaded->parent_chat_id, fixture.parent);
	UAM_ASSERT_EQ(loaded->messages.back().content, std::string("Exact Gemini child answer"));
	UAM_ASSERT_EQ(loaded->messages.back().tool_calls[0].result_text, std::string("Source file contents"));
	const ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	const auto parent_only = ProviderRuntimeRegistry::Resolve(profile).LoadHistory(profile, fixture.temp.root / "data", fixture.gemini_child.parent_path().parent_path(), {});
	UAM_ASSERT_EQ(parent_only.size(), std::size_t{1});
	UAM_ASSERT_EQ(parent_only.front().native_session_id, fixture.parent);
	auto foreign = reference; foreign.parent_session_id = "55555555-5555-4555-8555-555555555555";
	UAM_ASSERT(!uam::gemini::LoadNativeAgentHistory(foreign, fixture.workspace, &error));
	foreign = reference; foreign.agent_id = "foreign";
	UAM_ASSERT(!uam::gemini::ParseNativeAgentHistory(foreign, fixture.gemini_text, fixture.workspace, &error));
	UAM_ASSERT(!uam::gemini::LoadNativeAgentHistory(reference, fixture.workspace + "-foreign", &error));
	auto malformed = nlohmann::json::parse(fixture.gemini_text); malformed["kind"] = "main";
	UAM_ASSERT(!uam::gemini::ParseNativeAgentHistory(reference, malformed.dump(), fixture.workspace, &error));
}
#endif

UAM_TEST(RemoteNativeChildSnapshotsBindParentAgentWorkspaceAndHost)
{
	NativeProviderAgentFixture fixture;
	for (const std::string provider : {
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	    "claude-cli",
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    "gemini-cli",
#endif
	    ""})
	{
		if (provider.empty()) continue;
		const auto reference = fixture.Reference(provider, "fixture-ssh");
		uam::remote::RunnerState state;
		nlohmann::json request{{"id", "child"}, {"type", "provider.history.read"}, {"providerId", provider}, {"sessionId", fixture.parent},
		    {"agentId", reference.agent_id}, {"workspace", fixture.workspace}, {"offset", 0}};
		const auto response = state.HandleProcessRequest(request);
		UAM_ASSERT(response.value("ok", false));
		std::string text;
		UAM_ASSERT(uam::base64::Decode(response["result"].value("data", ""), text));
		nlohmann::json snapshot{{"providerId", provider}, {"sessionId", fixture.parent}, {"agentId", reference.agent_id}, {"workspace", fixture.workspace}, {"text", text}};
		ExecutionHost host; host.id = reference.execution_host_id; host.transport = "ssh"; host.platform = uam::remote::native_history::Platform();
		ChatSession source; source.provider_id = provider; source.native_session_id = fixture.parent; source.execution_host_id = host.id; source.workspace_directory = fixture.workspace;
		const auto id = uam::native_agents::BuildHistoryId(provider, fixture.parent, reference.agent_id, host.id);
		std::string error;
		const auto parsed = uam::native_agents::ParseHistorySnapshot(host, source, id, snapshot, &error);
		UAM_ASSERT(parsed.has_value());
		UAM_ASSERT(parsed->native_session_id.empty() && parsed->imported_read_only);
		for (const char* field : {"sessionId", "agentId", "workspace"})
		{
			auto foreign = snapshot; foreign[field] = "foreign";
			UAM_ASSERT(!uam::native_agents::ParseHistorySnapshot(host, source, id, foreign, &error));
		}
		request["snapshotId"] = response["result"]["snapshotId"]; request["fingerprint"] = response["result"]["fingerprint"];
		request["agentId"] = "foreign";
		UAM_ASSERT(!state.HandleProcessRequest(request).value("ok", true));
		host.id = "different-host";
		UAM_ASSERT(!uam::native_agents::ParseHistorySnapshot(host, source, id, snapshot, &error));
	}
}

UAM_TEST(RemoteNativeChildClientUsesExactReadOnlyAgentSelector)
{
	NativeProviderAgentFixture fixture;
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() /
#if defined(_WIN32)
	    "uam-runner.exe";
#else
	    "uam-runner";
#endif
	uam::remote::RunnerClient client(service, {uam::paths::Utf8PathString(runner), "bridge-direct"});
	for (const std::string provider : {"claude-cli", "gemini-cli"})
	{
		const auto reference = fixture.Reference(provider);
		nlohmann::json snapshot;
		std::string error;
		UAM_ASSERT(client.ReadNativeHistory(provider, fixture.parent, fixture.workspace, snapshot, &error, {}, reference.agent_id));
		UAM_ASSERT_EQ(snapshot["agentId"], nlohmann::json(reference.agent_id));
		UAM_ASSERT(snapshot["text"].get_ref<const std::string&>().find("Native child") != std::string::npos);
	}
}

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{
	using Json = nlohmann::json;
	struct GeminiChildListingFixture : NativeProviderAgentFixture
	{
		fs::path children = gemini_child.parent_path();
		Json Child(const std::string& id, const std::string& question) const
		{
			return Json{{"sessionId", id}, {"kind", "subagent"}, {"directories", Json::array({workspace})}, {"startTime", "2026-10-16T12:00:00Z"}, {"lastUpdated", "2026-10-16T12:05:00Z"},
			    {"messages", Json::array({{{"type", "user"}, {"content", question}}, {{"type", "gemini"}, {"content", "Answer"}}})}};
		}
		void Write(const std::string& filename, const Json& document) const { UAM_ASSERT(uam::io::WriteTextFile(children / filename, document.dump())); }
		ChatSession Source(const std::string& host = {}) const
		{
			ChatSession source; source.id = "source"; source.provider_id = "gemini-cli"; source.native_session_id = parent; source.workspace_directory = workspace; source.execution_host_id = host;
			return source;
		}
	};
}

// Only a journal filed under the attested parent, naming itself, kind subagent and this workspace is offered.
UAM_TEST(GeminiChildListingReturnsOnlyAttestedJournalsOfExactParent)
{
	GeminiChildListingFixture fixture;
	fixture.Write("named-other.json", fixture.Child("not-this-file", "Mismatched filename"));
	auto wrong_kind = fixture.Child("wrong-kind", "Main session"); wrong_kind["kind"] = "main";
	fixture.Write("wrong-kind.json", wrong_kind);
	auto foreign_workspace = fixture.Child("foreign-workspace", "Another project"); foreign_workspace["directories"] = Json::array({"/somewhere/else"});
	fixture.Write("foreign-workspace.json", foreign_workspace);
	auto foreign_parent = fixture.Child("foreign-parent", "Claims another parent"); foreign_parent["parentSessionId"] = "55555555-5555-4555-8555-555555555555";
	fixture.Write("foreign-parent.json", foreign_parent);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.children / "corrupt.jsonl", "{not json\n{still not\n"));
	// Same preview, workspace and times under a different parent directory: never offered.
	const fs::path other_parent = fixture.children.parent_path() / "55555555-5555-4555-8555-555555555555";
	UAM_ASSERT(uam::io::WriteTextFile(other_parent / "decoy.json", fixture.Child("decoy", "Native child question").dump()));
#if !defined(_WIN32)
	fs::create_symlink(other_parent / "decoy.json", fixture.children / "linked.json");
#endif
	auto preamble = fixture.Child("with-preamble", "Real task");
	preamble["messages"].insert(preamble["messages"].begin(), Json{{"id", std::string(uam::gemini::kSessionContextRowId)}, {"type", "user"},
	    {"content", Json::array({{{"text", std::string(uam::gemini::kSessionContextOpening) + "context" + std::string(uam::gemini::kSessionContextClosing)}}})}});
	fixture.Write("with-preamble.json", preamble);
	auto placeholder = preamble; placeholder["sessionId"] = "only-preamble"; placeholder["messages"] = Json::array({placeholder["messages"][0]});
	fixture.Write("only-preamble.json", placeholder);

	uam::remote::native_history::GeminiChildListing listing;
	std::string error;
	UAM_ASSERT(uam::remote::native_history::ListGeminiChildren(fixture.parent, fixture.workspace, listing, error));
	UAM_ASSERT_EQ(listing.children.size(), std::size_t{2});
	std::map<std::string, uam::remote::native_history::GeminiChild> by_id;
	for (const auto& child : listing.children) by_id[child.agent_id] = child;
	UAM_ASSERT_EQ(by_id.at(fixture.gemini_agent).preview, std::string("Native child question"));
	UAM_ASSERT_EQ(by_id.at(fixture.gemini_agent).messages, std::size_t{2});
	UAM_ASSERT_EQ(by_id.at("with-preamble").preview, std::string("Real task"));
	UAM_ASSERT_EQ(by_id.at("with-preamble").messages, std::size_t{2});
	UAM_ASSERT_EQ(by_id.at("with-preamble").updated_at, std::string("2026-10-16T12:05:00Z"));
	UAM_ASSERT_EQ(listing.unreadable, std::size_t{5}); // mismatched name, kind, workspace, parent, corrupt
	UAM_ASSERT(!listing.truncated);

	UAM_ASSERT(!uam::remote::native_history::ListGeminiChildren(fixture.parent, "/different/workspace", listing, error));
	UAM_ASSERT(!uam::remote::native_history::ListGeminiChildren("66666666-6666-4666-8666-666666666666", fixture.workspace, listing, error));
	const fs::path empty_parent_file = fixture.children.parent_path() / "77777777-7777-4777-8777-777777777777.json";
	UAM_ASSERT(uam::io::WriteTextFile(empty_parent_file, Json{{"sessionId", "77777777-7777-4777-8777-777777777777"}, {"messages", Json::array()}}.dump()));
	UAM_ASSERT(uam::remote::native_history::ListGeminiChildren("77777777-7777-4777-8777-777777777777", fixture.workspace, listing, error));
	UAM_ASSERT(listing.children.empty() && listing.unreadable == 0);
}

UAM_TEST(GeminiChildListingBoundsCountAndBytesNewestFirst)
{
	GeminiChildListingFixture fixture;
	std::error_code removed;
	fs::remove(fixture.gemini_child, removed);
	const auto base = fs::file_time_type::clock::now() - std::chrono::hours(1000);
	for (std::size_t index = 0; index < uam::remote::native_history::kMaxGeminiChildren + 2; ++index)
	{
		const std::string id = "child-" + std::to_string(index);
		fixture.Write(id + ".json", fixture.Child(id, "Task " + std::to_string(index)));
		fs::last_write_time(fixture.children / (id + ".json"), base + std::chrono::minutes(index));
	}
	uam::remote::native_history::GeminiChildListing listing;
	std::string error;
	UAM_ASSERT(uam::remote::native_history::ListGeminiChildren(fixture.parent, fixture.workspace, listing, error));
	UAM_ASSERT_EQ(listing.children.size(), uam::remote::native_history::kMaxGeminiChildren);
	UAM_ASSERT(listing.truncated);
	UAM_ASSERT_EQ(listing.children.front().agent_id, std::string("child-201"));

	fs::remove(fixture.children / "child-0.json");
	fs::resize_file(fixture.children / "child-201.json", uam::remote::native_history::kMaxSnapshotBytes + 1);
	UAM_ASSERT(uam::remote::native_history::ListGeminiChildren(fixture.parent, fixture.workspace, listing, error));
	UAM_ASSERT(!listing.truncated); // Exactly 200 remain; the oversized newest one is reported, not hidden as "not recent".
	UAM_ASSERT_EQ(listing.unreadable, std::size_t{1});
	UAM_ASSERT_EQ(listing.children.front().agent_id, std::string("child-200"));

	// The shared byte budget ends the list at that point: an older small journal never fills a gap.
	fs::resize_file(fixture.children / "child-200.json", uam::remote::native_history::kMaxSnapshotBytes - 16);
	fs::resize_file(fixture.children / "child-199.json", 32);
	UAM_ASSERT(uam::remote::native_history::ListGeminiChildren(fixture.parent, fixture.workspace, listing, error));
	UAM_ASSERT(listing.truncated);
	UAM_ASSERT(listing.children.empty());
}

// The listed reference is the exact reader's own identity, and only a writable Gemini parent can list.
UAM_TEST(GeminiChildListingReferenceOpensExactReader)
{
	GeminiChildListingFixture fixture;
	const auto source = fixture.Source();
	std::string error;
	auto listing = uam::native_agents::ListGeminiChildren(source, std::nullopt, &error);
	UAM_ASSERT(listing.has_value());
	UAM_ASSERT_EQ(listing->children.size(), std::size_t{1});
	const auto expected = uam::native_agents::BuildHistoryId("gemini-cli", fixture.parent, fixture.gemini_agent, "local");
	UAM_ASSERT_EQ(listing->children.front().reference, expected);
	const auto opened = uam::native_agents::LoadHistory(source, listing->children.front().reference, std::nullopt, &error);
	UAM_ASSERT(opened.has_value());
	UAM_ASSERT_EQ(opened->id, expected);
	UAM_ASSERT(opened->imported_read_only && opened->native_session_id.empty());

	auto not_gemini = source; not_gemini.provider_id = "claude-cli";
	UAM_ASSERT(!uam::native_agents::ListGeminiChildren(not_gemini, std::nullopt, &error));
	auto child_chat = source; child_chat.imported_read_only = true;
	UAM_ASSERT(!uam::native_agents::ListGeminiChildren(child_chat, std::nullopt, &error));
}

UAM_TEST(RemoteGeminiChildListingBindsParentWorkspaceAndHost)
{
	GeminiChildListingFixture fixture;
	uam::remote::RunnerState state;
	const nlohmann::json request{{"id", "children"}, {"type", "provider.history.children"}, {"providerId", "gemini-cli"}, {"sessionId", fixture.parent}, {"workspace", fixture.workspace}};
	const auto response = state.HandleProcessRequest(request);
	UAM_ASSERT(response.value("ok", false));
	const auto listing = response["result"];
	ExecutionHost host; host.id = "fixture-ssh"; host.transport = "ssh"; host.platform = uam::remote::native_history::Platform();
	const auto source = fixture.Source(host.id);
	std::string error;
	const auto parsed = uam::native_agents::ParseGeminiChildListing(host, source, listing, &error);
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->children.size(), std::size_t{1});
	UAM_ASSERT_EQ(parsed->children.front().reference, uam::native_agents::BuildHistoryId("gemini-cli", fixture.parent, fixture.gemini_agent, host.id));
	for (const char* field : {"providerId", "sessionId", "workspace"})
	{
		auto foreign = listing; foreign[field] = "foreign";
		UAM_ASSERT(!uam::native_agents::ParseGeminiChildListing(host, source, foreign, &error));
	}
	for (const char* identifier : {"../escape", ""})
	{
		auto unsafe = listing; unsafe["children"][0]["agentId"] = identifier;
		UAM_ASSERT(!uam::native_agents::ParseGeminiChildListing(host, source, unsafe, &error));
	}
	auto duplicate = listing; duplicate["children"].push_back(duplicate["children"][0]);
	UAM_ASSERT(!uam::native_agents::ParseGeminiChildListing(host, source, duplicate, &error));
	auto wrong_host = host; wrong_host.id = "different-host";
	UAM_ASSERT(!uam::native_agents::ParseGeminiChildListing(wrong_host, source, listing, &error));
	for (const char* field : {"providerId", "sessionId", "workspace"})
	{
		auto invalid = request; invalid[field] = "../foreign";
		UAM_ASSERT(!state.HandleProcessRequest(invalid).value("ok", true));
	}
}

UAM_TEST(RemoteGeminiChildListingClientUsesActualHelperAndCapability)
{
	GeminiChildListingFixture fixture;
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() /
#if defined(_WIN32)
	    "uam-runner.exe";
#else
	    "uam-runner";
#endif
	uam::remote::RunnerClient client(service, {uam::paths::Utf8PathString(runner), "bridge-direct"});
	nlohmann::json listing;
	std::string error;
	UAM_ASSERT(client.ListGeminiChildren(fixture.parent, fixture.workspace, listing, &error));
	UAM_ASSERT_EQ(listing["children"].size(), std::size_t{1});
	UAM_ASSERT_EQ(listing["children"][0].value("agentId", ""), fixture.gemini_agent);
}
#endif
