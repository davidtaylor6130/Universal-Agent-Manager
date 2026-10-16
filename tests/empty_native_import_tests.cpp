#include "test_harness.h"
#include "app/startup_native_import_service.h"

using namespace uam_test;

// A private runner fixture: no provider CLI or real SSH connection is launched.
std::optional<int> RunEmptyNativeImportFixture(int argc, char** argv)
{
	if (argc != 3 || std::string_view(argv[1]) != "--uam-test-empty-import-runner") return std::nullopt;
	const nlohmann::json data = nlohmann::json::parse(ReadFile(argv[2]));
	uam::remote::RunnerState state;
	nlohmann::json request;
	std::string output;
	std::uint64_t cursor = 0;
	bool rpc_process = false;
	while (uam::remote::ReadFrame(std::cin, request) == uam::remote::FrameReadResult::Ok)
	{
		const std::string type = request.value("type", "");
		nlohmann::json result = nlohmann::json::object();
		if (type == "process.start")
		{
			cursor = 0;
			const auto arguments = request.at("argv").get<std::vector<std::string>>();
			rpc_process = std::ranges::find(arguments, "app-server") != arguments.end();
			if (!rpc_process) output = (std::ranges::find(arguments, "export") != arguments.end() ? data.at("openCodeExport") : data.at("openCodeList")).dump();
		}
		if (type == "process.write")
		{
			std::string decoded;
			if (!uam::base64::Decode(request.at("dataBase64").get<std::string>(), decoded)) return 2;
			std::istringstream lines(decoded);
			std::string line;
			while (std::getline(lines, line))
			{
				const auto rpc = nlohmann::json::parse(line);
				if (!rpc.contains("id")) continue;
				nlohmann::json value = nlohmann::json::object();
				const std::string method = rpc.value("method", "");
				if (method == "thread/read") value = data.at("codexRead");
				if (method == "thread/list") value = {{"data", rpc["params"].value("archived", false) ? nlohmann::json::array() : data.at("codexList")}};
				output += nlohmann::json({{"jsonrpc", "2.0"}, {"id", rpc["id"]}, {"result", value}}).dump() + "\n";
			}
			result["inputSequence"] = request.value("inputSequence", std::uint64_t{0});
		}
		if (type == "process.poll")
		{
			cursor += output.size();
			result = {{"running", rpc_process}, {"exitCode", 0}, {"stdoutBase64", uam::base64::Encode(output)}, {"stderrBase64", ""}, {"stdoutCursor", cursor}, {"stderrCursor", 0}};
			output.clear();
		}
		const auto response = type == "hello" ? uam::remote::HandleRunnerRequest(request, "test", &state)
		    : nlohmann::json{{"id", request["id"]}, {"ok", true}, {"result", result}};
		if (!uam::remote::WriteFrame(std::cout, response)) return 3;
		std::cout.flush();
	}
	return 0;
}

namespace
{
	struct EmptyImportFixture
	{
		TempDir temp{"uam-empty-native-import"};
		uam::AppState app;
		EmptyImportFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.folders.push_back({"workspace", "Workspace", (temp.root / "workspace").string(), false, "local"});
			fs::create_directories(temp.root / "workspace");
		}
		ChatSession Candidate(const std::string& provider, const std::string& id = "native-empty") const
		{
			ChatSession chat;
			chat.id = provider == "codex-cli" && id == "native-empty" ? "11111111-1111-4111-8111-111111111111" : id;
			chat.native_session_id = chat.id;
			chat.provider_id = provider;
			chat.folder_id = app.folders.front().id;
			chat.execution_host_id = app.folders.front().execution_host_id;
			chat.workspace_directory = app.folders.front().directory;
			return chat;
		}
		ChatHistorySyncService::ImportResult Batch(ChatSession chat)
		{
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			discovery.chats.push_back(std::move(chat));
			if (app.folders.front().execution_host_id != "local") discovery.expected_hosts.emplace(app.settings.execution_hosts.front().id, app.settings.execution_hosts.front());
			ChatHistorySyncService::ImportResult result;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (discovery.Pending())
			{
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
				result.Merge(ChatHistorySyncService().ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly));
				std::this_thread::yield();
			}
			return result;
		}
	};

#if !defined(_WIN32)
	struct RemoteEmptyImportFixture : EmptyImportFixture
	{
		const std::string codex_id = "11111111-1111-4111-8111-111111111111";
		fs::path response = temp.root / "response.json";
		std::unique_ptr<ScopedEnvVar> path;
		nlohmann::json data;
		RemoteEmptyImportFixture()
		{
			ExecutionHost host;
			host.id = "private-runner"; host.ssh_alias = host.id; host.platform = "linux"; host.transport = "ssh";
			host.runner_status = "ready"; host.runner_version = "test"; host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
			app.settings.execution_hosts.push_back(host);
			app.folders.front().execution_host_id = host.id;
			app.folders.front().directory = "/srv/private-workspace";
			const auto executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "ssh", "#!/bin/sh\nexec " + ShellQuoteForTest(executable.string()) + " --uam-test-empty-import-runner " + ShellQuoteForTest(response.string()) + "\n"));
			fs::permissions(temp.root / "ssh", fs::perms::owner_all);
			path = std::make_unique<ScopedEnvVar>("PATH", temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin"));
			data = {{"openCodeList", nlohmann::json::array({{{"id", "ses_empty"}, {"title", "Empty"}, {"directory", app.folders.front().directory}, {"created", 1}, {"updated", 2}}})},
			    {"openCodeExport", {{"info", {{"id", "ses_empty"}, {"directory", app.folders.front().directory}}}, {"messages", nlohmann::json::array()}}},
			    {"codexList", nlohmann::json::array({{{"id", codex_id}, {"preview", "Empty"}, {"cwd", app.folders.front().directory}, {"createdAt", 1}, {"updatedAt", 2}}})},
			    {"codexRead", {{"thread", {{"id", codex_id}, {"cwd", app.folders.front().directory}, {"turns", nlohmann::json::array()}}}}}};
			Write();
		}
		void Write() { UAM_ASSERT(uam::io::WriteTextFile(response, data.dump())); }
		void Populate()
		{
			data["openCodeExport"]["messages"] = nlohmann::json::array({{{"info", {{"role", "user"}}}, {"parts", nlohmann::json::array({{{"type", "text"}, {"text", "Private question"}}})}}});
			data["codexRead"]["thread"]["turns"] = nlohmann::json::array({{{"items", nlohmann::json::array({{{"type", "userMessage"}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Private question"}}})}}})}}});
			Write();
		}
	};
#endif
}

UAM_TEST(EmptyNativeImportsAreFilteredAndNeverPublishedAcrossProviders)
{
	for (const std::string provider : {"gemini-cli", "codex-cli", "opencode-cli", "claude-cli", "copilot-cli", "antigravity-cli"})
	for (int mode = 0; mode < 4; ++mode)
	{
		EmptyImportFixture fixture;
		ChatSession chat = fixture.Candidate(provider);
		if (mode == 1) chat.messages.push_back({MessageRole::User, " \n\t"});
		if (mode == 2) chat.messages.push_back({MessageRole::System, "Only setup instructions"});
		if (mode == 3) { Message marker; marker.role = MessageRole::Assistant; marker.interrupted = true; chat.messages.push_back(marker); }
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(chat);
		UAM_ASSERT(ChatHistorySyncService().FilterStartupImportCandidates(fixture.app, discovery));
		UAM_ASSERT(discovery.chats.empty());
		const auto synchronous = ChatHistorySyncService().ImportDiscoveredProviderChatsForFolder(fixture.app, fixture.app.folders.front(), [&]() { ChatHistorySyncService::LocalHistoryDiscovery value; value.chats.push_back(chat); return value; }());
		UAM_ASSERT(synchronous.success && synchronous.imported_count == 0);
		const auto batch = fixture.Batch(chat);
		UAM_ASSERT(batch.success && batch.imported_count == 0);
		UAM_ASSERT(ChatRepository::LoadLocalChats(fixture.app.data_root).empty());
		UAM_ASSERT(!fs::exists(fixture.app.data_root / "native-import-tombstones.json"));
		chat.messages.push_back({MessageRole::User, "Question added later"});
		const auto retry = fixture.Batch(chat);
		UAM_ASSERT(retry.success && retry.imported_count == 1);
	}
}

UAM_TEST(NativeImportContentGuardPreservesAttachmentsToolsAndReasoning)
{
	for (int mode = 0; mode < 3; ++mode)
	{
		EmptyImportFixture fixture;
		ChatSession chat = fixture.Candidate("gemini-cli");
		Message message;
		message.role = mode == 0 ? MessageRole::User : MessageRole::Assistant;
		if (mode == 0) message.attachments.push_back({"attachment", "photo.png", "image", "image/png", "/private/photo.png"});
		if (mode == 1) { ToolCall tool; tool.id = "call"; tool.name = "bash"; message.tool_calls.push_back(tool); }
		if (mode == 2) message.thoughts = "Private reasoning";
		chat.messages.push_back(message);
		const auto result = fixture.Batch(chat);
		UAM_ASSERT(result.success && result.imported_count == 1);
		UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id)->messages, chat.messages);
	}
}

UAM_TEST(RemoteDiscoveryVerifiesEmptyHistoriesBeforeRecommendation)
{
#if !defined(_WIN32) && UAM_ENABLE_RUNTIME_OPENCODE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI
	RemoteEmptyImportFixture fixture;
	const auto profile = ProviderProfileStore::DefaultOpenCodeProfile();
	const auto discover = [&]() { return ChatHistorySyncService().DiscoverRemoteProviderChatsForFolder(fixture.app.folders.front(), fixture.app.settings.execution_hosts.front(), &profile); };
	const auto empty = discover();
	UAM_ASSERT(empty.result.success);
	UAM_ASSERT(empty.chats.empty());
	fixture.Populate();
	const auto full = discover();
	UAM_ASSERT(full.result.success);
	UAM_ASSERT_EQ(full.chats.size(), std::size_t{2});
	for (const auto& chat : full.chats) UAM_ASSERT(!chat.messages_loaded && chat.messages.empty() && chat.persisted_message_count > 0);
	UAM_ASSERT(ChatRepository::LoadLocalChats(fixture.app.data_root).empty());
#endif
}

UAM_TEST(RemoteCodexImportLoadsHistoryAndSkipsEmptyWithoutTombstone)
{
#if !defined(_WIN32) && UAM_ENABLE_RUNTIME_CODEX_CLI
	RemoteEmptyImportFixture fixture;
	const std::vector<ChatHistorySyncService::RemoteCodexSession> sessions = {{fixture.codex_id, "Unknown metadata", fixture.app.folders.front().directory, 1, 2}};
	const ChatHistorySyncService service;
	const auto empty = service.ImportRemoteCodexChatsForFolder(fixture.app, "workspace", sessions);
	UAM_ASSERT(empty.success && empty.imported_count == 0);
	UAM_ASSERT(fixture.Batch(fixture.Candidate("codex-cli", fixture.codex_id)).imported_count == 0);
	UAM_ASSERT(ChatRepository::LoadLocalChats(fixture.app.data_root).empty());
	UAM_ASSERT(!fs::exists(fixture.app.data_root / "native-import-tombstones.json"));
	fixture.Populate();
	const auto full = service.ImportRemoteCodexChatsForFolder(fixture.app, "workspace", sessions);
	UAM_ASSERT(full.success && full.imported_count == 1);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.codex_id)->messages.front().content, std::string("Private question"));
#endif
}

UAM_TEST(OpenCodeKnownEmptyMessagesDoNotMakeConversationEligible)
{
	for (int mode = 0; mode < 5; ++mode)
	{
		nlohmann::json entry = {{"info", {{"role", "user"}}}, {"parts", nlohmann::json::array()}};
		if (mode == 1) entry["parts"].push_back({{"type", "text"}, {"text", " \n\t"}});
		if (mode == 2) entry["info"]["role"] = "system";
		if (mode == 3) { entry["info"]["role"] = "assistant"; entry["info"]["error"] = {{"name", "MessageAbortedError"}}; }
		if (mode == 4) { entry["info"]["role"] = "assistant"; entry["info"]["summary"] = true; entry["parts"].push_back({{"type", "text"}, {"text", "Only a compaction summary"}}); }
		const nlohmann::json document = {{"info", {{"id", "ses_empty"}, {"directory", "/private"}}}, {"messages", nlohmann::json::array({entry})}};
		const auto parsed = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(document.dump());
		UAM_ASSERT(parsed.success && parsed.verified_empty);
	}
	const auto unknown = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(R"({"info":{"id":"ses_empty","directory":"/private"},"messages":[{"info":{"role":"future-role"},"parts":[]}]})");
	UAM_ASSERT(!unknown.verified_empty);
}

UAM_TEST(LocalOpenCodeDiscoverySkipsEmptyMarkersAndAllowsLaterContent)
{
#if !defined(_WIN32) && UAM_ENABLE_RUNTIME_OPENCODE_CLI
	for (int mode = 0; mode < 4; ++mode)
	{
		EmptyImportFixture fixture;
		const ChatSession chat = fixture.Candidate("opencode-cli", "ses_empty");
		const fs::path exported = fixture.temp.root / "export.json";
		const fs::path listed = fixture.temp.root / "list.json";
		const fs::path executable = fixture.temp.root / "private-opencode";
		UAM_ASSERT(uam::io::WriteTextFile(executable, "#!/bin/sh\nif [ \"$1\" = export ]; then exec /bin/cat " + ShellQuoteForTest(exported.string()) + "; else exec /bin/cat " + ShellQuoteForTest(listed.string()) + "; fi\n"));
		fs::permissions(executable, fs::perms::owner_all);
		ProviderProfile profile = ProviderProfileStore::DefaultOpenCodeProfile();
		profile.interactive_command = uam::shell::JoinEscapedArgs({executable.string()});
		UAM_ASSERT(uam::io::WriteTextFile(listed, nlohmann::json::array({{{"id", chat.id}, {"title", "Empty"}, {"directory", chat.workspace_directory}}}).dump()));
		nlohmann::json entry = {{"info", {{"role", "assistant"}}}, {"parts", nlohmann::json::array()}};
		if (mode == 0) entry["parts"].push_back({{"type", "text"}, {"text", " \n\t"}});
		if (mode == 1) entry["info"]["error"] = {{"name", "MessageAbortedError"}};
		if (mode == 2) { entry["info"]["summary"] = true; entry["parts"].push_back({{"type", "text"}, {"text", "Only a compaction summary"}}); }
		if (mode == 3) { entry["info"]["role"] = "system"; entry["parts"].push_back({{"type", "text"}, {"text", "Only setup instructions"}}); }
		nlohmann::json document = {{"info", {{"id", chat.native_session_id}, {"directory", chat.workspace_directory}}}, {"messages", nlohmann::json::array({entry})}};
		UAM_ASSERT(uam::io::WriteTextFile(exported, document.dump()));
		const ChatHistorySyncService service;
		const auto empty = service.DiscoverProviderChatsForFolder(fixture.app.folders.front(), &profile, {}, 0, true);
		UAM_ASSERT(empty.result.success && empty.chats.empty());
		UAM_ASSERT(empty.open_code_exports.empty());
		document["messages"][0] = {{"info", {{"role", "user"}}}, {"parts", nlohmann::json::array({{{"type", "text"}, {"text", "Content added later"}}})}};
		UAM_ASSERT(uam::io::WriteTextFile(exported, document.dump()));
		auto full = service.DiscoverProviderChatsForFolder(fixture.app.folders.front(), &profile, {}, 0, true);
		UAM_ASSERT(full.result.success && full.chats.size() == 1);
		const auto imported = service.ImportDiscoveredProviderChatsForFolder(fixture.app, fixture.app.folders.front(), std::move(full));
		UAM_ASSERT(imported.success && imported.imported_count == 1);
		UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id)->messages.front().content, std::string("Content added later"));
		UAM_ASSERT_EQ(ReadFile(exported), document.dump());
	}
#endif
}

UAM_TEST(RemoteCodexInvalidHistoryRemainsRetryableAndUnpublished)
{
#if !defined(_WIN32) && UAM_ENABLE_RUNTIME_CODEX_CLI
	for (int mode = 0; mode < 6; ++mode)
	{
		RemoteEmptyImportFixture fixture;
		if (mode == 0) fixture.data["codexRead"]["thread"].erase("turns");
		if (mode == 1) fixture.data["codexRead"]["thread"]["id"] = "22222222-2222-4222-8222-222222222222";
		if (mode == 2) fixture.data["codexRead"]["thread"]["cwd"] = "/different/workspace";
		if (mode == 3) fixture.data["codexRead"]["thread"]["turns"] = nlohmann::json::array({{{"items", nlohmann::json::array({{{"type", "future-item"}}})}}});
		if (mode == 4) fixture.data["codexRead"]["thread"]["nextCursor"] = "more-history";
		if (mode == 5) fixture.data["codexRead"]["thread"]["turns"] = nlohmann::json::array({{{"items", nlohmann::json::array({{{"type", "userMessage"}, {"content", nlohmann::json::array({{{"type", "future-attachment"}}})}}})}}});
		fixture.Write();
		const auto failed = fixture.Batch(fixture.Candidate("codex-cli", fixture.codex_id));
		UAM_ASSERT(!failed.success && failed.imported_count == 0);
		UAM_ASSERT(failed.confirmed_empty_native_keys.empty());
		UAM_ASSERT(ChatRepository::LoadLocalChats(fixture.app.data_root).empty());
		UAM_ASSERT(!fs::exists(fixture.app.data_root / "native-import-tombstones.json"));
		fixture.data["codexRead"]["thread"]["id"] = fixture.codex_id;
		fixture.data["codexRead"]["thread"]["cwd"] = fixture.app.folders.front().directory;
		fixture.data["codexRead"]["thread"].erase("nextCursor");
		fixture.Populate();
		const auto retry = fixture.Batch(fixture.Candidate("codex-cli", fixture.codex_id));
		UAM_ASSERT(retry.success && retry.imported_count == 1);
	}
#endif
}

UAM_TEST(OpenCodeUnmappedOrPartialEmptyStreamsAreNotConfirmedEmpty)
{
	for (int mode = 0; mode < 4; ++mode)
	{
		EmptyImportFixture fixture;
		const auto chat = fixture.Candidate("opencode-cli", "ses_unknown");
		nlohmann::json document = {{"info", {{"id", chat.id}, {"directory", chat.workspace_directory}}}, {"messages", nlohmann::json::array()}};
		if (mode == 0) document["messages"].push_back({{"info", {{"role", "future-role"}}}, {"parts", nlohmann::json::array()}});
		if (mode == 1) document["messages"].push_back({{"info", {{"role", "assistant"}}}, {"parts", nlohmann::json::array({{{"type", "future-part"}}})}});
		if (mode == 2) document["hasMore"] = true;
		if (mode == 3) document["nextCursor"] = "more-history";
		const auto file = fixture.temp.root / "source.json";
		UAM_ASSERT(uam::io::WriteTextFile(file, document.dump()));
		std::string error;
		UAM_ASSERT(!uam::opencode_import::Read(file, chat, [](const Message&) { return true; }, {}, error));
		UAM_ASSERT(!error.empty());
	}
}
