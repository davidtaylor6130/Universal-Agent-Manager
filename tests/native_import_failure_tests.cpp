#include "test_harness.h"
#include "app/runtime_orchestration_services.h"

#include <nlohmann/json.hpp>

using namespace uam_test;

UAM_TEST(OpenCodeImportAcceptsMoreThanTenThousandMessages)
{
	nlohmann::json exported = {
	    {"info", {{"id", "ses_many_messages"}, {"directory", "/workspace"}}},
	    {"messages", nlohmann::json::array()},
	};
	const nlohmann::json message = {
	    {"info", {{"role", "user"}}},
	    {"parts", nlohmann::json::array({{{"type", "text"}, {"text", "Message"}}})},
	};
	for (std::size_t index = 0; index < 10001; ++index)
	{
		exported["messages"].push_back(message);
	}

	const ChatHistorySyncService::RemoteOpenCodeTranscript parsed =
	    ChatHistorySyncService::ParseRemoteOpenCodeTranscript(exported.dump());
	UAM_ASSERT(parsed.success);
	UAM_ASSERT_EQ(parsed.messages.size(), std::size_t{10001});
}

UAM_TEST(OpenCodeImportReportsInvalidRootAndMessagesSeparately)
{
	const ChatHistorySyncService::RemoteOpenCodeTranscript invalid_root =
	    ChatHistorySyncService::ParseRemoteOpenCodeTranscript(R"({"unexpected":true})");
	UAM_ASSERT(!invalid_root.success);
	UAM_ASSERT_EQ(invalid_root.error, std::string("OpenCode export is missing valid session information."));

	const ChatHistorySyncService::RemoteOpenCodeTranscript invalid_messages =
	    ChatHistorySyncService::ParseRemoteOpenCodeTranscript(
	        R"({"info":{"id":"ses_invalid_messages","directory":"/workspace"},"messages":{}})");
	UAM_ASSERT(!invalid_messages.success);
	UAM_ASSERT_EQ(invalid_messages.error, std::string("OpenCode export is missing a valid messages array."));
}

UAM_TEST(CodexDiscoveryExcludesSessionsWithoutAuthoredUsers)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-authored-discovery");
	ScopedEnvVar codex_home("CODEX_HOME", (temp.root / "codex").string());
	ScopedEnvVar gemini_home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
	ScopedEnvVar copilot_home("COPILOT_HOME", (temp.root / "copilot").string());
	ScopedEnvVar claude_home("CLAUDE_CONFIG_DIR", (temp.root / "claude").string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	app.folders.push_back({"known", "Known", workspace.string(), false, "local"});
	const fs::path sessions = temp.root / "codex" / "sessions";
	fs::create_directories(sessions);
	const std::string authored_id = "44444444-4444-4444-8444-444444444444";
	const std::string context = "<environment_context>\nprivate fixture context\n</environment_context>";
	const nlohmann::json context_message = {{"type", "response_item"}, {"payload", {{"type", "message"},
	    {"role", "user"}, {"content", nlohmann::json::array({{{"type", "input_text"}, {"text", context}}})}}}};
	for (const int mode : {0, 1, 2, 3})
	{
		const std::string id = mode == 3 ? authored_id : "11111111-1111-4111-8111-11111111111" + std::to_string(mode);
		std::string records = nlohmann::json{{"type", "session_meta"}, {"payload", {{"id", id}, {"cwd", workspace.string()},
		    {"timestamp", "2026-10-10T01:25:39Z"}}}}.dump() + "\n";
		if (mode == 0)
			records += nlohmann::json{{"type", "event_msg"}, {"payload", {{"type", "thread_settings_applied"}}}}.dump() + "\n";
		else if (mode == 1)
			records += nlohmann::json{{"type", "realtime_item"}, {"payload", {{"type", "realtime_session_closed"}}}}.dump() + "\n";
		else records += context_message.dump() + "\n";
		if (mode == 3)
		{
			records += nlohmann::json{{"type", "response_item"}, {"payload", {{"type", "message"}, {"role", "user"},
			    {"content", nlohmann::json::array({{{"type", "input_text"}, {"text", context + "\nAuthored request"}}})}}}}.dump() + "\n";
			records += nlohmann::json{{"type", "response_item"}, {"payload", {{"type", "message"}, {"role", "assistant"},
			    {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", "Authored response"}}})}}}}.dump() + "\n";
		}
		UAM_ASSERT(uam::io::WriteTextFile(sessions / ("rollout-" + id + ".jsonl"), records));
	}
	const ChatHistorySyncService::LocalHistoryDiscovery discovery = ChatHistorySyncService().DiscoverProviderChatsForFolder(app.folders.front());
	UAM_ASSERT(discovery.result.success);
	UAM_ASSERT_EQ(discovery.codex_rollouts.size(), std::size_t{1});
	UAM_ASSERT_EQ(discovery.codex_rollouts.front().first.native_session_id, authored_id);
	UAM_ASSERT(discovery.codex_rollouts.front().first.messages.empty());
	UAM_ASSERT(!discovery.codex_rollouts.front().first.messages_loaded);
	const ChatHistorySyncService::ImportResult imported = ChatHistorySyncService().ImportDiscoveredProviderChatsForFolder(app, app.folders.front(), discovery, true);
	UAM_ASSERT(imported.success);
	UAM_ASSERT_EQ(imported.imported_count, 1);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, authored_id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(saved->messages.front().content, std::string("Authored request"));
	UAM_ASSERT_EQ(saved->messages.back().content, std::string("Authored response"));
	std::stop_source cancelled;
	cancelled.request_stop();
	UAM_ASSERT(ChatHistorySyncService().DiscoverProviderChatsForFolder(app.folders.front(), nullptr, cancelled.get_token()).codex_rollouts.empty());
#endif
}

UAM_TEST(CodexDiscoveryKeepsMalformedHistoryFailClosed)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-malformed-discovery");
	ScopedEnvVar codex_home("CODEX_HOME", (temp.root / "codex").string());
	ScopedEnvVar gemini_home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
	ScopedEnvVar copilot_home("COPILOT_HOME", (temp.root / "copilot").string());
	ScopedEnvVar claude_home("CLAUDE_CONFIG_DIR", (temp.root / "claude").string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	app.folders.push_back({"known", "Known", workspace.string(), false, "local"});
	const fs::path sessions = temp.root / "codex" / "sessions";
	fs::create_directories(sessions);
	const std::string id = "55555555-5555-4555-8555-555555555555";
	const fs::path rollout = sessions / ("rollout-" + id + ".jsonl");
	const std::string metadata = nlohmann::json{{"type", "session_meta"}, {"payload", {{"id", id}, {"cwd", workspace.string()}}}}.dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(rollout, metadata + "{incomplete"));
	const ChatHistorySyncService::LocalHistoryDiscovery malformed = ChatHistorySyncService().DiscoverProviderChatsForFolder(app.folders.front());
	UAM_ASSERT(!malformed.result.success);
	UAM_ASSERT(malformed.codex_rollouts.empty());
	const std::string message = nlohmann::json{{"type", "response_item"}, {"payload", {{"type", "message"}, {"role", "user"},
	    {"content", nlohmann::json::array({{{"type", "input_text"}, {"text", "Authored request"}}})}}}}.dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(rollout, metadata + message + "{incomplete"));
	const ChatHistorySyncService::LocalHistoryDiscovery discovered = ChatHistorySyncService().DiscoverProviderChatsForFolder(app.folders.front());
	UAM_ASSERT_EQ(discovered.codex_rollouts.size(), std::size_t{1});
	const ChatHistorySyncService::ImportResult imported = ChatHistorySyncService().ImportDiscoveredProviderChatsForFolder(app, app.folders.front(), discovered, true);
	UAM_ASSERT(!imported.success);
	UAM_ASSERT_EQ(imported.imported_count, 0);
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, id)));
#endif
}

UAM_TEST(CodexDiscoveredUnknownChronologyReachesSavedReconciliationBeforeOverlay)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (const bool native_unknown : {false, true})
	{
		for (const bool grow : {false, true})
		{
			TempDir temp("uam-codex-unknown-reconciliation");
			const auto private_home = temp.root / "home";
			fs::create_directories(private_home);
			ScopedEnvVar home("HOME", private_home.string());
			ScopedEnvVar userprofile("USERPROFILE", private_home.string());
			ScopedEnvVar codex_home("CODEX_HOME", (temp.root / "codex").string());
			uam::AppState app;
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			const auto workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			app.folders.push_back({"unknown-workspace", "Unknown workspace", workspace.string(), false, "local"});
			UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
			const std::string id = "44444444-4444-4444-8444-444444444444";
			const auto rollout = temp.root / "codex" / "sessions" / ("rollout-" + id + ".jsonl");
			const auto write = [&](const std::string& stamp, const std::string& answer, bool add_row)
			{
				std::string records = nlohmann::json{{"type", "session_meta"}, {"payload", {{"id", id}, {"cwd", workspace.string()}, {"timestamp", "2026-10-16T09:00:00Z"}}}}.dump() + "\n";
				const auto message = [&](const std::string& role, const std::string& content)
				{
					return nlohmann::json{{"type", "response_item"}, {"timestamp", stamp}, {"payload", {{"type", "message"}, {"role", role}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", content}}})}}}}.dump() + "\n";
				};
				records += message("user", "Genuine private rollout question") + message("assistant", answer);
				if (add_row)
				{
					records += message("user", "Additional genuine rollout row");
				}
				UAM_ASSERT(uam::io::WriteTextFile(rollout, records));
			};
			const ChatHistorySyncService service;
			const auto discover = [&]
			{
				// Existing provider filter avoids other provider discovery/execution.
				auto discovery = service.DiscoverProviderChatsForFolder(app.folders.front(), nullptr, {}, 0, false, {}, {}, "codex-cli");
				UAM_ASSERT(discovery.result.success && discovery.chats.empty() && discovery.codex_rollouts.size() == 1);
				UAM_ASSERT_EQ(discovery.codex_rollouts.front().second, rollout);
				UAM_ASSERT_EQ(discovery.codex_rollouts.front().first.native_session_id, id);
				return discovery;
			};
			const auto drain = [&](ChatHistorySyncService::LocalHistoryDiscovery& discovery)
			{
				ChatHistorySyncService::ImportResult result;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
				while (discovery.Pending())
				{
					UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
					result.Merge(service.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly));
					std::this_thread::yield();
				}
				UAM_ASSERT(result.success && result.confirmed_empty_native_keys.empty());
				return result;
			};
			write("2026-10-16T09:01:00Z", "Initial saved rollout answer", false);
			auto initial = discover();
			UAM_ASSERT_EQ(drain(initial).imported_count, 1);
			auto saved = ChatRepository::LoadLocalChat(app.data_root, id, true);
			UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() == 2);
			saved->updated_at = native_unknown ? "2026-10-16T12:00:00Z" : "recorded";
			saved->title = "Preserve owned rollout title";
			saved->pinned = true;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *saved));
			saved = ChatRepository::LoadLocalChat(app.data_root, id, true);
			UAM_ASSERT(saved && saved->messages.size() == 2);
			app.chats = {*saved}; // Genuine complete persisted owner; no inferred/forged ownership.
			const auto baseline = *saved;
			const auto primary = AppPaths::UamChatFilePath(app.data_root, id);
			const auto summary = AppPaths::UamChatSummaryFilePath(app.data_root, id);
			const auto before = uam::io::ReadTextFile(primary), summary_before = uam::io::ReadTextFile(summary);
			write(native_unknown ? "recorded" : "2026-10-16T13:00:00Z", "Different actual native rollout answer", grow);
			const auto source_bytes = uam::io::ReadTextFile(rollout);
			// Existing public reader resolves this exact genuine session and reads
			// the complete rollout; it neither changes eligibility nor overlays UAM.
			std::string source_error;
			const auto source = service.LoadLocalCodexChildChat(baseline, &source_error);
			UAM_ASSERT(source && source_error.empty() && source->messages_loaded);
			UAM_ASSERT_EQ(source->provider_id, std::string("codex-cli"));
			UAM_ASSERT_EQ(source->native_session_id, id);
			UAM_ASSERT_EQ(source->workspace_directory, workspace.string());
			UAM_ASSERT_EQ(source->updated_at, native_unknown ? std::string("recorded") : std::string("2026-10-16T13:00:00Z"));
			UAM_ASSERT_EQ(source->messages.size(), grow ? std::size_t{3} : std::size_t{2});
			UAM_ASSERT_EQ(source->messages[1].content, std::string("Different actual native rollout answer"));
			UAM_ASSERT(source->messages != baseline.messages);
			UAM_ASSERT_EQ(uam::io::ReadTextFile(rollout), source_bytes);
			auto update = discover();
			const auto admitted = service.ImportDiscoveredProviderChatsBatch(app, {}, update, 8, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly);
			UAM_ASSERT(admitted.success && admitted.imported_count == 0 && update.preparation.valid());
			UAM_ASSERT_EQ(update.preparation.wait_for(std::chrono::seconds(15)), std::future_status::ready);
			{
				const auto prepared = update.preparation.get();
				UAM_ASSERT(prepared && prepared->chats.size() == 1);
				const auto& candidate = prepared->chats.front();
				UAM_ASSERT(candidate.expected_live && candidate.expected_live->messages == baseline.messages);
				UAM_ASSERT(candidate.error.empty() && !candidate.confirmed_empty);
				UAM_ASSERT_EQ(candidate.skipped, !grow);
				if (!grow)
				{
					UAM_ASSERT(!candidate.files && !candidate.native_media);
					UAM_ASSERT_EQ(candidate.chat.messages, baseline.messages);
				}
			}
			UAM_ASSERT_EQ(drain(update).imported_count, grow ? 1 : 0);
			const auto after = ChatRepository::LoadLocalChat(app.data_root, id, true);
			UAM_ASSERT(after && after->messages_loaded);
			if (grow)
			{
				UAM_ASSERT_EQ(after->messages.size(), std::size_t{3});
				UAM_ASSERT_EQ(after->messages[1].content, std::string("Different actual native rollout answer"));
				UAM_ASSERT_EQ(after->messages.back().content, std::string("Additional genuine rollout row"));
			}
			else
			{
				UAM_ASSERT_EQ(after->messages, baseline.messages);
				UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), before);
				UAM_ASSERT_EQ(uam::io::ReadTextFile(summary), summary_before);
			}
			UAM_ASSERT_EQ(after->title, baseline.title);
			UAM_ASSERT_EQ(after->pinned, baseline.pinned);
			UAM_ASSERT_EQ(after->native_session_id, id);
			UAM_ASSERT_EQ(after->provider_id, std::string("codex-cli"));
			UAM_ASSERT_EQ(uam::io::ReadTextFile(rollout), source_bytes);
		}
	}
#endif
}
