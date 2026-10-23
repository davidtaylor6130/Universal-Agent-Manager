#include "test_harness.h"
#include "remote/native_history_snapshot.h"

using namespace uam_test;

namespace
{
	struct NativeRemoteHistoryFixture
	{
		TempDir temp{"uam-remote-native-history"};
		ScopedEnvVar claude{"CLAUDE_CONFIG_DIR", (temp.root / "claude").string()};
		ScopedEnvVar copilot{"COPILOT_HOME", (temp.root / "copilot").string()};
		ScopedEnvVar gemini{"GEMINI_CLI_HOME", (temp.root / "gemini").string()};
		const std::string id = "33333333-3333-4333-8333-333333333333";
		const std::string workspace = uam::paths::Utf8PathString(temp.root / "workspace");
		std::string claude_text, copilot_text, gemini_text;
		fs::path copilot_events;
		NativeRemoteHistoryFixture()
		{
			fs::create_directories(uam::paths::PathFromUtf8(workspace));
			std::string encoded = workspace;
			for (char& character : encoded) if (!uam::strings::IsAsciiAlnum(static_cast<unsigned char>(character))) character = '-';
			claude_text = nlohmann::json({{"type", "user"}, {"uuid", "user"}, {"parentUuid", nullptr}, {"sessionId", id}, {"cwd", workspace},
			    {"message", {{"role", "user"}, {"content", "Native terminal question"}}}}).dump() + "\n" +
			    nlohmann::json({{"type", "assistant"}, {"uuid", "assistant"}, {"parentUuid", "user"}, {"sessionId", id}, {"cwd", workspace},
			    {"message", {{"role", "assistant"}, {"content", "Native Claude answer"}}}}).dump() + "\n";
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "claude" / "projects" / encoded / (id + ".jsonl"), claude_text));
			copilot_text = nlohmann::json({{"id", "start"}, {"type", "session.start"}, {"data", {{"sessionId", id}, {"context", {{"cwd", workspace}}}}}}).dump() + "\n" +
			    nlohmann::json({{"id", "user"}, {"type", "user.message"}, {"data", {{"content", "Native terminal question"}}}}).dump() + "\n" +
			    nlohmann::json({{"id", "assistant"}, {"type", "assistant.message"}, {"data", {{"content", "Native Copilot answer"}}}}).dump() + "\n";
			copilot_events = temp.root / "copilot" / "session-state" / id / "events.jsonl";
			UAM_ASSERT(uam::io::WriteTextFile(copilot_events, copilot_text));
			UAM_ASSERT(uam::io::WriteTextFile(copilot_events.parent_path() / "workspace.yaml", "cwd: " + nlohmann::json(workspace).dump() + "\n"));
			gemini_text = nlohmann::json({{"sessionId", id}, {"startTime", "2026-10-16T12:00:00Z"},
			    {"messages", nlohmann::json::array({{{"type", "user"}, {"content", "Native terminal question"}},
			        {{"type", "gemini"}, {"content", "Native Gemini answer"}, {"thoughts", nlohmann::json::array({"Native thought"})}}})}}).dump();
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "gemini" / ".gemini" / "tmp" / "project" / ".project_root", workspace));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "gemini" / ".gemini" / "tmp" / "project" / "chats" / ("session-2026-10-16T12-00-" + id.substr(0, 8) + ".json"), gemini_text));
		}
		nlohmann::json Request(const std::string& provider) const
		{
			return {{"id", "history"}, {"type", "provider.history.read"}, {"providerId", provider}, {"sessionId", id}, {"workspace", workspace}, {"offset", 0}};
		}
	};
}

UAM_TEST(RemoteNativeHistoryReadsExactProviderSessionAndWorkspace)
{
	NativeRemoteHistoryFixture fixture;
	for (const std::string provider : {"claude-cli", "copilot-cli", "gemini-cli"})
	{
		uam::remote::native_history::Snapshot snapshot;
		std::string error;
		UAM_ASSERT(uam::remote::native_history::Load(provider, fixture.id, fixture.workspace, snapshot, error));
		UAM_ASSERT(error.empty());
		UAM_ASSERT_EQ(snapshot.session_id, fixture.id);
		UAM_ASSERT_EQ(snapshot.fingerprint, uam::remote::native_history::Fingerprint(snapshot));
		UAM_ASSERT(!uam::remote::native_history::Load(provider, fixture.id, fixture.workspace + "-foreign", snapshot, error));
		UAM_ASSERT(!uam::remote::native_history::Load(provider, "../escape", fixture.workspace, snapshot, error));
		UAM_ASSERT(!uam::remote::native_history::Load(provider, "different-session", fixture.workspace, snapshot, error));
	}
	std::error_code error;
	fs::rename(fixture.copilot_events, fixture.temp.root / "outside.jsonl", error);
	UAM_ASSERT(!error);
	fs::create_symlink(fixture.temp.root / "outside.jsonl", fixture.copilot_events, error);
	if (!error)
	{
		uam::remote::native_history::Snapshot snapshot;
		std::string read_error;
		UAM_ASSERT(!uam::remote::native_history::Load("copilot-cli", fixture.id, fixture.workspace, snapshot, read_error));
	}
}

UAM_TEST(RemoteNativeHistorySnapshotsAreImmutableBoundedAndExplicitlyReleased)
{
	NativeRemoteHistoryFixture fixture;
	fixture.copilot_text += nlohmann::json({{"type", "assistant.message"}, {"data", {{"content", std::string(300000, 'x')}}}}).dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.copilot_events, fixture.copilot_text));
	uam::remote::RunnerState state;
	auto request = fixture.Request("copilot-cli");
	const auto first = state.HandleProcessRequest(request);
	UAM_ASSERT(first.value("ok", false));
	UAM_ASSERT(first.dump().size() < uam::remote::kMaxRunnerFrameBytes);
	request["snapshotId"] = first["result"]["snapshotId"];
	request["fingerprint"] = first["result"]["fingerprint"];
	UAM_ASSERT(uam::io::WriteTextFile(fixture.copilot_events, "source changed after snapshot\n"));
	std::string assembled;
	for (;;)
	{
		request["offset"] = assembled.size();
		const auto response = state.HandleProcessRequest(request);
		UAM_ASSERT(response.value("ok", false));
		std::string chunk;
		UAM_ASSERT(uam::base64::Decode(response["result"].value("data", ""), chunk));
		UAM_ASSERT(chunk.size() <= uam::remote::native_history::kChunkBytes);
		assembled += chunk;
		if (assembled.size() == fixture.copilot_text.size()) break;
		UAM_ASSERT(!chunk.empty());
	}
	UAM_ASSERT_EQ(assembled, fixture.copilot_text);
	auto invalid = request;
	invalid["offset"] = 0;
	invalid["fingerprint"] = "wrong";
	UAM_ASSERT(!state.HandleProcessRequest(invalid).value("ok", true));
	invalid = request;
	invalid["workspace"] = fixture.workspace + "-foreign";
	UAM_ASSERT(!state.HandleProcessRequest(invalid).value("ok", true));
	invalid = request;
	invalid["providerId"] = "claude-cli";
	UAM_ASSERT(!state.HandleProcessRequest(invalid).value("ok", true));
	const auto second = state.HandleProcessRequest(fixture.Request("claude-cli"));
	UAM_ASSERT(second.value("ok", false));
	UAM_ASSERT(!state.HandleProcessRequest(fixture.Request("gemini-cli")).value("ok", true));
	request["type"] = "provider.history.release";
	UAM_ASSERT(state.HandleProcessRequest(request).value("ok", false));
	request["type"] = "provider.history.read";
	UAM_ASSERT(!state.HandleProcessRequest(request).value("ok", true));
	UAM_ASSERT(state.HandleProcessRequest(fixture.Request("gemini-cli")).value("ok", false));
}

UAM_TEST(RemoteNativeHistoryParsersPreserveExactIdentityAndRejectForeignSnapshots)
{
	NativeRemoteHistoryFixture fixture;
	ExecutionHost host;
	host.id = "fixture-ssh"; host.transport = "ssh"; host.platform = uam::remote::native_history::Platform(); host.runner_status = "ready";
	for (const std::string provider : {
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	    "claude-cli",
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	    "copilot-cli",
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    "gemini-cli",
#endif
	    ""})
	{
		if (provider.empty()) continue;
		uam::remote::native_history::Snapshot source;
		std::string error;
		UAM_ASSERT(uam::remote::native_history::Load(provider, fixture.id, fixture.workspace, source, error));
		nlohmann::json snapshot{{"providerId", provider}, {"sessionId", fixture.id}, {"workspace", fixture.workspace}, {"text", source.text}, {"workspaceYaml", source.workspace_yaml}};
		ChatSession chat;
		chat.provider_id = provider; chat.native_session_id = fixture.id; chat.workspace_directory = fixture.workspace; chat.execution_host_id = host.id;
		const auto parsed = ChatHistorySyncService().ParseRemoteNativeHistorySnapshot(host, chat, snapshot, &error);
		UAM_ASSERT(parsed.has_value());
		UAM_ASSERT(error.empty());
		UAM_ASSERT_EQ(parsed->execution_host_id, host.id);
		UAM_ASSERT_EQ(parsed->native_session_id, fixture.id);
		UAM_ASSERT_EQ(parsed->messages.back().content, provider == "claude-cli" ? std::string("Native Claude answer") : provider == "copilot-cli" ? std::string("Native Copilot answer") : std::string("Native Gemini answer"));
		auto foreign = snapshot; foreign["sessionId"] = "different";
		UAM_ASSERT(!ChatHistorySyncService().ParseRemoteNativeHistorySnapshot(host, chat, foreign, &error));
		foreign = snapshot; foreign["workspace"] = fixture.workspace + "-foreign";
		UAM_ASSERT(!ChatHistorySyncService().ParseRemoteNativeHistorySnapshot(host, chat, foreign, &error));
		foreign = snapshot; foreign["providerId"] = "different";
		UAM_ASSERT(!ChatHistorySyncService().ParseRemoteNativeHistorySnapshot(host, chat, foreign, &error));
		foreign = snapshot;
		std::string altered = source.text;
		const auto id = altered.find(fixture.id);
		UAM_ASSERT(id != std::string::npos);
		altered.replace(id, fixture.id.size(), "different-session");
		foreign["text"] = altered;
		UAM_ASSERT(!ChatHistorySyncService().ParseRemoteNativeHistorySnapshot(host, chat, foreign, &error));
		std::stop_source cancel; cancel.request_stop();
		UAM_ASSERT(!ChatHistorySyncService().ParseRemoteNativeHistorySnapshot(host, chat, snapshot, &error, cancel.get_token()));
		chat.execution_host_id = "different-host";
		UAM_ASSERT(!ChatHistorySyncService().ParseRemoteNativeHistorySnapshot(host, chat, snapshot, &error));
	}
}

UAM_TEST(RemoteNativeHistoryClientReassemblesAndReleasesAuthenticatedSnapshots)
{
	NativeRemoteHistoryFixture fixture;
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() /
#if defined(_WIN32)
	    "uam-runner.exe";
#else
	    "uam-runner";
#endif
	uam::remote::RunnerClient client(service, {uam::paths::Utf8PathString(runner), "bridge-direct"});
	for (const std::string provider : {"claude-cli", "copilot-cli", "gemini-cli", "claude-cli"})
	{
		nlohmann::json snapshot;
		std::string error;
		UAM_ASSERT(client.ReadNativeHistory(provider, fixture.id, fixture.workspace, snapshot, &error));
		UAM_ASSERT(error.empty());
		UAM_ASSERT_EQ(snapshot["sessionId"], nlohmann::json(fixture.id));
		UAM_ASSERT(snapshot["text"].get_ref<const std::string&>().find("Native terminal question") != std::string::npos);
	}
	nlohmann::json snapshot;
	std::string error;
	std::stop_source cancel; cancel.request_stop();
	UAM_ASSERT(!client.ReadNativeHistory("copilot-cli", fixture.id, fixture.workspace, snapshot, &error, cancel.get_token()));
	UAM_ASSERT(snapshot.is_null());
}

UAM_TEST(RemoteGeminiExactHistoryIgnoresUnrelatedOversizedAndUnreadableFiles)
{
	NativeRemoteHistoryFixture fixture;
	const fs::path chats = fixture.temp.root / "gemini" / ".gemini" / "tmp" / "project" / "chats";
	const fs::path unrelated = chats / "session-2026-10-16T00-00-ffffffff.json";
	UAM_ASSERT(uam::io::WriteTextFile(unrelated, "{}"));
	fs::resize_file(unrelated, uam::remote::native_history::kMaxSnapshotBytes + 1);
	UAM_ASSERT(uam::io::WriteTextFile(chats / "unrelated-malformed.json", "not native JSON"));
	std::error_code ignored;
	fs::create_symlink(fixture.temp.root / "missing", chats / "session-2026-10-16T00-00-eeeeeeee.json", ignored);
	uam::remote::native_history::Snapshot snapshot;
	std::string error;
	UAM_ASSERT(uam::remote::native_history::Load("gemini-cli", fixture.id, fixture.workspace, snapshot, error));
	UAM_ASSERT_EQ(snapshot.text, fixture.gemini_text);
	// A short-ID collision is only a candidate; JSON native ID remains authoritative.
	UAM_ASSERT(uam::io::WriteTextFile(chats / ("session-2026-10-16T00-01-" + fixture.id.substr(0, 8) + ".json"),
	    nlohmann::json({{"sessionId", fixture.id.substr(0, 8) + "-foreign"}, {"messages", nlohmann::json::array()}}).dump()));
	UAM_ASSERT(uam::remote::native_history::Load("gemini-cli", fixture.id, fixture.workspace, snapshot, error));
	UAM_ASSERT_EQ(snapshot.text, fixture.gemini_text);
}

std::optional<int> RunNativeRemoteHistoryReleaseFixture(int argc, char** argv)
{
	if (argc != 3 || std::string_view(argv[1]) != "--uam-test-native-history-release") return std::nullopt;
	uam::remote::RunnerState state;
	nlohmann::json request;
	while (uam::remote::ReadFrame(std::cin, request) == uam::remote::FrameReadResult::Ok)
	{
		nlohmann::json response;
		if (request.value("type", "") == "hello") response = uam::remote::HandleRunnerRequest(request, "development", &state);
		else if (request.value("type", "") == "provider.history.release")
		{
			if (!uam::io::WriteTextFile(uam::paths::PathFromUtf8(argv[2]), "release received")) return 2;
			// Simulate a stalled transport reply. The client must enforce its cleanup deadline and stop this fixture.
			std::this_thread::sleep_for(std::chrono::seconds(30));
			return 3;
		}
		else
		{
			uam::remote::native_history::Snapshot snapshot;
			snapshot.provider = request.value("providerId", "");
			snapshot.session_id = request.value("sessionId", "");
			snapshot.workspace = request.value("workspace", "");
			snapshot.text = "Isolated native transcript";
			response = {{"id", request["id"]}, {"ok", true}, {"result", {
			    {"providerId", snapshot.provider}, {"sessionId", snapshot.session_id}, {"workspace", snapshot.workspace},
			    {"snapshotId", "fixture-snapshot"}, {"fingerprint", uam::remote::native_history::Fingerprint(snapshot)},
			    {"totalBytes", snapshot.text.size()}, {"offset", 0}, {"data", uam::base64::Encode(snapshot.text)}, {"workspaceYaml", ""}}}};
		}
		if (!uam::remote::WriteFrame(std::cout, response)) return 1;
		std::cout.flush();
	}
	return 0;
}

UAM_TEST(RemoteNativeHistoryCleanupCannotStallAfterSuccessfulRead)
{
	TempDir temp("uam-native-history-release-timeout");
	ScopedEnvVar data_root("UAM_DATA_DIR", (temp.root / "data").string());
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path marker = temp.root / "release-attempted";
	uam::remote::RunnerClient client(service, {uam::paths::Utf8PathString(service.ResolveCurrentExecutablePath()),
	    "--uam-test-native-history-release", uam::paths::Utf8PathString(marker)});
	nlohmann::json snapshot;
	std::string error;
	const auto started = std::chrono::steady_clock::now();
	UAM_ASSERT(client.ReadNativeHistory("copilot-cli", "fixture-native-session", uam::paths::Utf8PathString(temp.root), snapshot, &error));
	const auto elapsed = std::chrono::steady_clock::now() - started;
	UAM_ASSERT(fs::exists(marker));
	UAM_ASSERT(elapsed < std::chrono::seconds(2));
	UAM_ASSERT_EQ(snapshot["text"], nlohmann::json("Isolated native transcript"));
}
