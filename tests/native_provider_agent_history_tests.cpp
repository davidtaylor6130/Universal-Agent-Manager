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
		host.id = "fixture-ssh"; host.transport = "ssh"; host.platform = "linux"; host.ssh_alias = "isolated-native-agent-fixture";
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
