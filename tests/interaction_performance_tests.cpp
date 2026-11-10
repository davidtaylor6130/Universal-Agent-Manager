#include "test_harness.h"
#include "app/transcript_retention_service.h"
#include "common/config/custom_icon.h"
#include "common/memory/memory_levels.h"
#include "common/memory/memory_policy.h"
#include "common/paths/recent_path_probe.h"

#include <algorithm>
#include <array>
#include <numeric>

using namespace uam_test;

namespace
{
	constexpr std::chrono::milliseconds kInteractionBudget{250};
	constexpr std::chrono::milliseconds kHistoryPageBudget{1000};
	constexpr std::chrono::milliseconds kStartupBudget{10000};

	/// <summary>Times real service completion on this machine and reports the failing operation.</summary>
	void RequireBudget(const std::string& label, std::chrono::steady_clock::time_point started,
	                   std::chrono::milliseconds budget)
	{
		const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		std::cout << "[TIMING] " << label << ": " << elapsed << " ms (limit " << budget.count() << " ms)\n";
		if (elapsed > static_cast<double>(budget.count()))
		{
			throw TestFailure(label + " took " + std::to_string(elapsed) + " ms; limit " + std::to_string(budget.count()) + " ms");
		}
	}

	ChatSession HistoryFixture(std::size_t count)
	{
		ChatSession chat;
		chat.id = "performance-history";
		chat.updated_at = "2026-10-11T20:00:00Z";
		chat.messages.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
		{
			Message message;
			message.role = index % 2 == 0 ? MessageRole::User : MessageRole::Assistant;
			message.content = "Message " + std::to_string(index) + ": " + std::string(2048, 'x');
			chat.messages.push_back(std::move(message));
		}
		return chat;
	}

	void CheckColdHistoryPage(std::size_t count)
	{
		TempDir temp("uam-performance-cold-history");
		const ChatSession fixture = HistoryFixture(count);
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, fixture));
		// Fixture creation and writes are excluded; the timed path is the actual cold-read fallback.
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, fixture.id, true);
		UAM_ASSERT(loaded.has_value());
		const nlohmann::json page = uam::StateSerializer::SerializeMessagePage(*loaded, 50, std::nullopt, true);
		UAM_ASSERT_EQ(page["totalCount"].get<std::size_t>(), count);
		UAM_ASSERT_EQ(page["messages"].size(), std::min(count, std::size_t{50}));
		UAM_ASSERT_EQ(page["messages"].back()["content"].get<std::string>(), fixture.messages.back().content);
		RequireBudget("cold initial history page, " + std::to_string(count) + " messages", started, kHistoryPageBudget);
	}
}

UAM_TEST(PerformanceColdHistoryPage20Messages)
{
	CheckColdHistoryPage(20);
}

UAM_TEST(PerformanceColdHistoryPage50Messages)
{
	CheckColdHistoryPage(50);
}

UAM_TEST(PerformanceColdHistoryPage200Messages)
{
	CheckColdHistoryPage(200);
}

UAM_TEST(PerformanceColdHistoryPage10000Messages)
{
	CheckColdHistoryPage(10000);
}

UAM_TEST(PerformanceCachedHugeHistoryPageResponse)
{
	TempDir temp("uam-performance-cached-history");
	const ChatSession fixture = HistoryFixture(10000);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, fixture));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, fixture.id, true);
	UAM_ASSERT(loaded.has_value());
	const std::shared_ptr<PreparedChatSave> prepared = uam::MessageTailCache::Prepare(temp.root, *loaded);
	UAM_ASSERT(prepared != nullptr);
	UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared));
	const ChatSession summary = ChatRepository::LoadLocalChatSummaries(temp.root).front();
	for (int repetition = 0; repetition < 5; ++repetition)
	{
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		const std::optional<nlohmann::json> page = uam::MessageTailCache::Read(temp.root, fixture.id,
		    summary.persisted_message_count, summary.persisted_messages_digest, 50);
		UAM_ASSERT(page.has_value());
		UAM_ASSERT_EQ((*page)["messages"].size(), std::size_t{50});
		UAM_ASSERT_EQ((*page)["startIndex"].get<std::size_t>(), std::size_t{9950});
		RequireBudget("cached huge history page", started, kInteractionBudget);
	}
}

UAM_TEST(PerformanceInitialStateSummaryResponse)
{
	uam::AppState app;
	for (int index = 0; index < 1000; ++index)
	{
		ChatSession chat;
		chat.id = "summary-" + std::to_string(index);
		chat.title = chat.id;
		chat.messages_loaded = false;
		chat.persisted_message_count = 10000;
		chat.persisted_messages_digest = "persisted-summary";
		app.chats.push_back(std::move(chat));
	}
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	const nlohmann::json response = uam::StateSerializer::Serialize(app, true);
	UAM_ASSERT_EQ(response["chats"].size(), std::size_t{1000});
	RequireBudget("initial-state summary service response (not application launch)", started, kStartupBudget);
}

UAM_TEST(PerformanceColdLocalSummaryCatalogue)
{
	TempDir temp("uam-performance-cold-summary-catalogue");
	const fs::path chats = AppPaths::UamChatsRootPath(temp.root);
	fs::create_directories(chats);
	for (int index = 0; index < 1000; ++index)
	{
		const std::string id = "cold-summary-" + std::to_string(index);
		const nlohmann::json document = {
		    {"id", id}, {"title", id}, {"provider_id", "gemini-cli"},
		    {"created_at", "2026-01-01 12:00:00"}, {"updated_at", "2026-01-01 12:00:00"},
		    {"messages", nlohmann::json::array({{{"role", "user"}, {"content", "Request"}},
		                                      {{"role", "assistant"}, {"content", "Response"}}})}};
		std::ofstream file(chats / (id + ".json"));
		file << document.dump();
		UAM_ASSERT(file.good());
	}
	// The catalogue must read every primary and republish every summary cache, so on shared runners the
	// file-system floor alone approaches the fixed 1000 ms (#526). Measure that floor on this machine with
	// the same reads plus a plain unsynced temp write and rename per file, outside production code; the
	// budget allows twice it with 1000 ms kept as the floor, so a durable sync per cache write still fails.
	TempDir floor_root("uam-performance-cold-summary-floor");
	std::array<std::chrono::steady_clock::duration, 3> floors{};
	for (std::size_t sample = 0; sample < floors.size(); ++sample)
	{
		const fs::path copies = floor_root.root / std::to_string(sample);
		fs::create_directories(copies);
		const std::chrono::steady_clock::time_point floor_started = std::chrono::steady_clock::now();
		for (const fs::directory_entry& entry : fs::directory_iterator(chats))
		{
			const fs::path temp_copy = copies / (entry.path().filename().string() + ".tmp");
			{
				std::ofstream copy(temp_copy, std::ios::binary);
				copy << ReadFile(entry.path());
				UAM_ASSERT(copy.good());
			}
			fs::rename(temp_copy, copies / entry.path().filename());
		}
		floors[sample] = std::chrono::steady_clock::now() - floor_started;
		fs::remove_all(copies);
	}
	std::ranges::sort(floors);
	const std::chrono::milliseconds budget = std::max(kHistoryPageBudget,
	    std::chrono::ceil<std::chrono::milliseconds>(2 * floors[1]));
	std::cout << "[TIMING] cold catalogue file-system floor: "
	          << std::chrono::duration<double, std::milli>(floors[1]).count() << " ms\n";
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	const std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(summaries.size(), std::size_t{1000});
	for (const ChatSession& chat : summaries)
	{
		UAM_ASSERT(chat.messages.empty());
		UAM_ASSERT_EQ(chat.persisted_message_count, std::size_t{2});
	}
	RequireBudget("cold 1000-chat summary catalogue with cache regeneration", started, budget);
}

UAM_TEST(ColdSummaryCacheRegenerationPreservesPrimaryHistory)
{
	TempDir temp("uam-cold-summary-recovery");
	const ChatSession fixture = HistoryFixture(20);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, fixture));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, fixture.id);
	const fs::path cache = AppPaths::UamChatSummaryFilePath(temp.root, fixture.id);
	const std::string original = uam::io::ReadTextFile(primary);
	for (const bool corrupt : {false, true})
	{
		if (corrupt) UAM_ASSERT(uam::io::WriteTextFile(cache, "invalid cache"));
		else fs::remove(cache);
		std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(temp.root);
		UAM_ASSERT_EQ(summaries.size(), std::size_t{1});
		UAM_ASSERT_EQ(summaries.front().persisted_message_count, std::size_t{20});
		UAM_ASSERT(summaries.front().messages.empty());
		UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, summaries.front()));
		UAM_ASSERT_EQ(summaries.front().messages.back().content, fixture.messages.back().content);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), original);
		UAM_ASSERT(!uam::io::ReadTextFile(cache).empty());
	}
}

UAM_TEST(UndatedChatSummaryCacheIsReusedWithoutRegeneration)
{
	TempDir temp("uam-undated-summary-reuse");
	ChatSession chat;
	chat.id = "undated-summary";
	chat.messages = {Message{MessageRole::User, "Request"}};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path cache = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
	const std::string saved = uam::io::ReadTextFile(cache);
	const std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(summaries.size(), std::size_t{1});
	// A rejected cache is rewritten from the primary on every catalogue load.
	UAM_ASSERT_EQ(uam::io::ReadTextFile(cache), saved);
	const std::optional<ChatSession> full = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(full.has_value());
	UAM_ASSERT_EQ(summaries.front().persisted_messages_digest, full->persisted_messages_digest);
}

UAM_TEST(AtomicCacheWritesPreserveFaultSafetyAndDurableDefaults)
{
	TempDir temp("uam-atomic-cache-safety");
	const fs::path cache = temp.root / "cache.json";
	bool synced = false;
	UAM_ASSERT(uam::io::AtomicWriteFileDetailed(cache, "old", [&](uam::io::AtomicWriteStage stage)
	{
		if (stage == uam::io::AtomicWriteStage::AfterFileSync) synced = true;
		return true;
	}).success);
	UAM_ASSERT(synced);
	for (const uam::io::AtomicWriteStage fault : {uam::io::AtomicWriteStage::DuringWrite, uam::io::AtomicWriteStage::BeforeReplace})
	{
		const uam::io::AtomicWriteResult interrupted = uam::io::AtomicWriteFileDetailed(cache, "partial replacement", [&](uam::io::AtomicWriteStage stage)
		{
			return stage != fault;
		}, false, false);
		UAM_ASSERT(!interrupted.success);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(cache), std::string("old"));
	}
	synced = false;
	UAM_ASSERT(uam::io::AtomicWriteFileDetailed(cache, "new", [&](uam::io::AtomicWriteStage stage)
	{
		if (stage == uam::io::AtomicWriteStage::AfterFileSync) synced = true;
		return true;
	}, false, false).success);
	UAM_ASSERT(!synced);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(cache), std::string("new"));
	UAM_ASSERT(uam::io::WriteCacheFile(cache, "complete"));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(cache), std::string("complete"));
	UAM_ASSERT(!uam::io::WriteCacheFile(temp.root, "invalid destination"));
#if !defined(_WIN32)
	const fs::path link = temp.root / "linked-cache.json";
	fs::create_symlink(cache, link);
	UAM_ASSERT(!uam::io::WriteCacheFile(link, "invalid link write"));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(cache), std::string("complete"));
#endif
}

UAM_TEST(PerformanceSettingsSaveResponse)
{
	TempDir temp("uam-performance-settings-save");
	AppSettings settings;
	settings.git_enabled = false;
	settings.svn_enabled = false;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	RequireBudget("settings durable save response", started, kInteractionBudget);
	AppSettings restored;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", restored).loaded);
	UAM_ASSERT(!restored.git_enabled && !restored.svn_enabled);
}

UAM_TEST(PerformanceSettingsLoadResponse)
{
	TempDir temp("uam-performance-settings-load");
	AppSettings settings;
	settings.git_enabled = false;
	settings.svn_enabled = true;
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	AppSettings restored;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", restored).loaded);
	UAM_ASSERT(!restored.git_enabled && restored.svn_enabled);
	RequireBudget("settings load response", started, kInteractionBudget);
}

namespace
{
	// The streaming fixture's UI-thread cost (ms of push work per simulated second) is printed, not gated:
	// on the shared, heavily loaded build host the same binary measured 230-446 ms/s between runs, so a
	// wall-clock limit only measured host load. Its deterministic push/rebuild counts are the regression gate
	// (they fail without the throttle fixes), and CustomPngDataUrlIsReusedUntilTheIconFileChanges guards the icon cache.
	// The structural slice comparison is not gated; a revert would show only in the printed figure.

	std::string FixturePng(std::size_t size)
	{
		std::string bytes("\x89PNG\r\n\x1a\n", 8);
		bytes += std::string("\0\0\0\rIHDR", 8);
		bytes += std::string("\0\0\1\0\0\0\1\0", 8); // 256 x 256
		bytes.resize(size, '\x5a');
		return bytes;
	}

	Message StreamingTurn(std::size_t tool_count, std::size_t tool_bytes)
	{
		Message message{MessageRole::Assistant, "Working on it"};
		message.created_at = "2026-10-25T10:00:00Z";
		for (std::size_t index = 0; index < tool_count; ++index)
		{
			ToolCall tool;
			tool.id = "tool-" + std::to_string(index);
			tool.name = "Read";
			tool.kind = "read";
			tool.status = "completed";
			tool.args_json = "{\"path\":\"src/file_" + std::to_string(index) + ".cpp\"}";
			tool.result_text = std::string(tool_bytes, static_cast<char>('a' + index % 26));
			message.tool_calls.push_back(std::move(tool));
		}
		return message;
	}

	std::unique_ptr<uam::AcpSessionState> StreamingSession(const ChatSession& chat, std::size_t tool_count, std::size_t tool_bytes)
	{
		std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat.id;
		session->provider_id = chat.provider_id;
		session->session_id = chat.id + "-native";
		session->running = true;
		session->processing = true;
		session->lifecycle_state = "processing";
		for (std::size_t index = 0; index < tool_count; ++index)
		{
			uam::AcpToolCallState tool;
			tool.id = "tool-" + std::to_string(index);
			tool.title = "Read src/file_" + std::to_string(index) + ".cpp";
			tool.kind = "read";
			tool.status = "completed";
			tool.content = std::string(tool_bytes, static_cast<char>('a' + index % 26));
			session->tool_calls.push_back(tool);
			session->turn_events.push_back({"assistant_text", "Reading file " + std::to_string(index) + ". "});
			session->turn_events.push_back({"tool_call", "", tool.id});
		}
		session->turn_events.push_back({"assistant_text", "Summarising"});
		return session;
	}

	/// 63 chats, PNG folder/chat icons, one selected streaming chat with 30 x 50 KB tools, two background streaming chats.
	void BuildStreamingPushFixture(uam::AppState& app, const fs::path& root)
	{
		app.data_root = root;
		fs::create_directories(root / "icons");
		std::vector<CustomIcon> icons;
		for (int index = 0; index < 8; ++index)
		{
			const CustomIcon icon{"png", "00000000-0000-4000-8000-00000000000" + std::to_string(index) + ".png"};
			UAM_ASSERT(uam::icons::IsValid(icon));
			UAM_ASSERT(uam::io::WriteBinaryFile(root / "icons" / icon.value, FixturePng(100 * 1024 + index * 20 * 1024)));
			icons.push_back(icon);
			ChatFolder folder;
			folder.id = "folder-" + std::to_string(index);
			folder.title = folder.id;
			folder.directory = (root / folder.id).generic_string();
			folder.custom_icon = icon;
			app.folders.push_back(std::move(folder));
		}
		for (int index = 0; index < 63; ++index)
		{
			ChatSession chat;
			chat.id = "push-chat-" + std::to_string(index);
			chat.title = chat.id;
			chat.provider_id = "claude-cli";
			chat.folder_id = "folder-" + std::to_string(index % 8);
			chat.custom_icon = icons[index % icons.size()];
			chat.updated_at = "2026-10-25T10:00:00Z";
			if (index < 3)
			{
				const std::size_t history = index == 0 ? 120 : 200;
				for (std::size_t message = 0; message < history; ++message)
					chat.messages.push_back(Message{message % 2 == 0 ? MessageRole::User : MessageRole::Assistant,
					    "History " + std::to_string(message) + ": " + std::string(2048, 'h')});
				chat.messages.push_back(StreamingTurn(index == 0 ? 30 : 5, 50 * 1024));
				app.acp_sessions.push_back(StreamingSession(chat, index == 0 ? 30 : 5, 50 * 1024));
			}
			else
			{
				chat.messages_loaded = false;
				chat.persisted_message_count = 80;
				chat.persisted_messages_digest = "persisted-" + std::to_string(index);
			}
			app.chats.push_back(std::move(chat));
		}
		app.selected_chat_index = 0;
	}

	void AppendStreamToken(uam::AppState& app, int token)
	{
		for (std::size_t index = 0; index < 3; ++index)
		{
			const std::string text = " token-" + std::to_string(token);
			app.chats[index].messages.back().content += text;
			app.acp_sessions[index]->turn_events.back().text += text;
		}
	}

	double Median(std::vector<double> values)
	{
		if (values.empty()) return 0.0;
		std::sort(values.begin(), values.end());
		const std::size_t middle = values.size() / 2;
		return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
	}
}

namespace
{
	std::chrono::steady_clock::time_point g_fake_push_now;

	/// <summary>Steps the push throttle clock by hand and restores the real clock even when an assertion throws.</summary>
	struct FakeStatePushClock
	{
		// Starts at real time so recorded push times stay plausible for later tests.
		FakeStatePushClock() { g_fake_push_now = std::chrono::steady_clock::now(); uam::SetStatePushClockForTests([] { return g_fake_push_now; }); }
		~FakeStatePushClock() { uam::SetStatePushClockForTests(nullptr); }
		void Advance(std::chrono::milliseconds by) { g_fake_push_now += by; }
	};
}

UAM_TEST(PerformanceStreamingStatePushTick)
{
	const WithoutStateCacheVerification unverified;
	TempDir temp("uam-performance-streaming-push");
	uam::AppState app;
	BuildStreamingPushFixture(app, temp.root);

	std::vector<double> fingerprints;
	for (int iteration = 0; iteration < 11; ++iteration)
	{
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app, true);
		fingerprints.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
		UAM_ASSERT_EQ(fingerprint["chats"].size(), std::size_t{63});
	}

	// Mirrors Application::PollTick: tokens land on every other 16 ms tick; quiet ticks only flush deferred pushes.
	// Throttle decisions use a stepped clock so counts are deterministic; push work is timed with the real clock.
	// The one-second window runs three times; the cheapest run is reported as the least load-affected figure.
	FakeStatePushClock clock;
	constexpr int kTicks = 62;
	constexpr std::chrono::milliseconds kTick{16};
	const double window_seconds = std::chrono::duration<double>(kTicks * kTick).count();
	double per_second = 0.0;
	std::vector<double> calls;
	std::vector<double> rebuilds;
	int pushes = 0;
	for (int run = 0; run < 3; ++run)
	{
		uam::PushStateUpdate(nullptr, app);
		std::vector<double> run_calls;
		std::vector<double> run_rebuilds;
		int run_pushes = 0;
		for (int tick = 0; tick < kTicks; ++tick)
		{
			const bool streamed = tick % 2 == 0;
			if (streamed) AppendStreamToken(app, run * kTicks + tick);
			if (streamed || uam::DeferredStatePushDue())
			{
				const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
				const std::size_t rebuilds_before = uam::RuntimeStateRebuildCountForTests();
				run_pushes += uam::PushRuntimeStateUpdateIfChanged(nullptr, app) ? 1 : 0;
				const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
				run_calls.push_back(elapsed);
				if (uam::RuntimeStateRebuildCountForTests() != rebuilds_before) run_rebuilds.push_back(elapsed);
			}
			clock.Advance(kTick);
		}

		// The three streaming chats share one 100 ms window: pushes land on ticks 0, 7, 14 ... 56 (every 112 ms),
		// so 9 pushes; calls are the 31 streamed ticks plus the quiet ticks 7, 21, 35 and 49 that are due.
		// Only the 9 due calls rebuild the fingerprint; before the runtime rebuild window all 35 rebuilt.
		// Without the background-chat throttle pushes rise to ~31; rebuilding every deferred quiet tick gives ~58.
		UAM_ASSERT_EQ(run_pushes, 9);
		UAM_ASSERT_EQ(run_calls.size(), std::size_t{35});
		UAM_ASSERT_EQ(run_rebuilds.size(), std::size_t{9});

		// Final streamed state must reach the frontend: flush deferred work, then nothing may remain unsent.
		for (int tick = 0; tick < 20 && uam::HasDeferredStatePush(); ++tick)
		{
			clock.Advance(kTick);
			if (uam::DeferredStatePushDue()) uam::PushRuntimeStateUpdateIfChanged(nullptr, app);
		}
		UAM_ASSERT(!uam::HasDeferredStatePush());
		clock.Advance(std::chrono::milliseconds(120));
		UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

		const double run_per_second = std::accumulate(run_calls.begin(), run_calls.end(), 0.0) / window_seconds;
		if (run == 0 || run_per_second < per_second)
		{
			per_second = run_per_second;
			calls = std::move(run_calls);
			rebuilds = std::move(run_rebuilds);
			pushes = run_pushes;
		}
	}

	std::cout << "[TIMING] streaming push fixture: fingerprint p50=" << Median(fingerprints) << " ms; ticks=62 calls="
	          << calls.size() << " rebuilds=" << rebuilds.size() << " pushes=" << pushes << " per-rebuild p50=" << Median(rebuilds)
	          << " ms; UI-thread push cost="
	          << per_second << " ms/s, cheapest of 3 windows\n";
}


UAM_TEST(RuntimeStatePushBypassesRebuildWindowForPendingRequestsAndCliLaunches)
{
	FakeStatePushClock clock;
	uam::AppState app;
	ChatSession chat;
	chat.id = "urgent-poll";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->running = true;
	uam::AcpSessionState* acp = session.get();
	app.acp_sessions.push_back(std::move(session));
	uam::PushStateUpdate(nullptr, app);

	// Open a rebuild window with an ordinary change, then make a non-urgent change inside it: deferred.
	app.chats.front().title = "first";
	UAM_ASSERT(uam::PushRuntimeStateUpdateIfChanged(nullptr, app));
	clock.Advance(std::chrono::milliseconds(16));
	app.chats.front().title = "second";
	UAM_ASSERT(!uam::PushRuntimeStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(uam::HasDeferredStatePush());

	// A new pending permission inside the same window is pushed on this tick.
	clock.Advance(std::chrono::milliseconds(16));
	acp->pending_permission.request_id_json = "41";
	UAM_ASSERT(uam::PushRuntimeStateUpdateIfChanged(nullptr, app));

	// A new CLI launch inside the next window is pushed on this tick too.
	clock.Advance(std::chrono::milliseconds(16));
	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->terminal_id = "term-urgent";
	terminal->frontend_chat_id = chat.id;
	terminal->running = true;
	terminal->startup_time_s = 1.0;
	app.cli_terminals.push_back(std::move(terminal));
	UAM_ASSERT(uam::PushRuntimeStateUpdateIfChanged(nullptr, app));

	// Caller-flagged urgent slices (settings, catalogs, folders) also rebuild inside the window.
	clock.Advance(std::chrono::milliseconds(16));
	const std::size_t rebuilds = uam::RuntimeStateRebuildCountForTests();
	uam::PushRuntimeStateUpdateIfChanged(nullptr, app, true);
	UAM_ASSERT_EQ(uam::RuntimeStateRebuildCountForTests(), rebuilds + 1);
}

namespace
{
	/// <summary>Follows PollTick's delay from an idle 1000 ms tick: sleeps the capped delay, then ticks. Returns the wake-ups used.</summary>
	int WakeUpsUntilDeferredPushPublishes(FakeStatePushClock& clock, uam::AppState& app)
	{
		int wake_ups = 0;
		bool published = false;
		while (!published && wake_ups < 20)
		{
			clock.Advance(std::chrono::milliseconds(uam::CapPollDelayForDeferredStatePush(1000)));
			++wake_ups;
			if (uam::DeferredStatePushDue()) published = uam::PushRuntimeStateUpdateIfChanged(nullptr, app);
		}
		return wake_ups;
	}
}

UAM_TEST(DeferredStatePushThatComesDueInsideARebuildWindowDoesNotBusyPoll)
{
	FakeStatePushClock clock;
	uam::AppState app;
	ChatSession chat;
	chat.id = "deferred-in-window";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	uam::PushStateUpdate(nullptr, app);

	// t=0 publishes the summary, so its next publish is due at t=100.
	app.chats.front().title = "first";
	UAM_ASSERT(uam::PushRuntimeStateUpdateIfChanged(nullptr, app));
	// t=30: an urgent rebuild restarts the rebuild window (until t=130) and defers the summary to t=100.
	clock.Advance(std::chrono::milliseconds(30));
	app.chats.front().title = "second";
	UAM_ASSERT(!uam::PushRuntimeStateUpdateIfChanged(nullptr, app, true));
	UAM_ASSERT_EQ(uam::MillisecondsUntilDeferredStatePushDue(), 70);
	// t=100: the tick cannot rebuild yet; the scheduler must sleep to the window's end instead of polling every 1 ms.
	clock.Advance(std::chrono::milliseconds(70));
	UAM_ASSERT(uam::DeferredStatePushDue());
	UAM_ASSERT(!uam::PushRuntimeStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(!uam::DeferredStatePushDue());
	UAM_ASSERT_EQ(uam::CapPollDelayForDeferredStatePush(1000), 30);
	UAM_ASSERT_EQ(uam::CapPollDelayForDeferredStatePush(16), 16);
	UAM_ASSERT_EQ(WakeUpsUntilDeferredPushPublishes(clock, app), 1);
	UAM_ASSERT(!uam::HasDeferredStatePush());
	UAM_ASSERT_EQ(uam::CapPollDelayForDeferredStatePush(1000), 1000);
	// PollTick needs CEF to run, so this checks that it schedules through the capped delay.
	const std::string application = ReadFile(fs::path(__FILE__).parent_path().parent_path() / "src/app/application.cpp");
	UAM_ASSERT(application.find("ScheduleNextUpdate(uam::CapPollDelayForDeferredStatePush(next_poll_delay_ms));") != std::string::npos);

	// A bridge push's per-chat window can end inside a later rebuild window: bridge at t=0, rebuild at t=62, due at t=100,
	// rebuild window until t=162, so the summary publishes 162 ms after the previous publish.
	clock.Advance(std::chrono::milliseconds(200));
	app.chats.front().title = "bridge";
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	clock.Advance(std::chrono::milliseconds(62));
	app.chats.front().title = "streamed";
	UAM_ASSERT(!uam::PushRuntimeStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::MillisecondsUntilDeferredStatePushDue(), 38);
	clock.Advance(std::chrono::milliseconds(38));
	UAM_ASSERT(!uam::PushRuntimeStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::MillisecondsUntilDeferredStatePushDue(), 62);
	UAM_ASSERT_EQ(WakeUpsUntilDeferredPushPublishes(clock, app), 1);
}

UAM_TEST(StatePushThrottlesBackgroundSummariesAndFlushesOnlyWhenDue)
{
	FakeStatePushClock clock;
	uam::AppState app;
	for (const char* id : {"selected-chat", "background-chat"})
	{
		ChatSession chat;
		chat.id = id;
		chat.title = "Original";
		app.chats.push_back(std::move(chat));
	}
	app.selected_chat_index = 0;
	uam::PushStateUpdate(nullptr, app);
	app.chats[1].title = "First stream update";
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	clock.Advance(std::chrono::milliseconds(50));
	app.chats[1].title = "Second stream update";
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(uam::HasDeferredStatePush());
	// A quiet tick inside the window must not rebuild the fingerprint.
	UAM_ASSERT(!uam::DeferredStatePushDue());
	// The poll scheduler wakes exactly when the window ends.
	UAM_ASSERT_EQ(uam::MillisecondsUntilDeferredStatePushDue(), 50);
	clock.Advance(std::chrono::milliseconds(49));
	UAM_ASSERT(!uam::DeferredStatePushDue());
	UAM_ASSERT_EQ(uam::MillisecondsUntilDeferredStatePushDue(), 1);
	clock.Advance(std::chrono::milliseconds(1));
	UAM_ASSERT(uam::DeferredStatePushDue());
	UAM_ASSERT_EQ(uam::MillisecondsUntilDeferredStatePushDue(), 1);
	// The deferred final state is published once due; nothing is lost or repeated.
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(!uam::HasDeferredStatePush());
	UAM_ASSERT(!uam::DeferredStatePushDue());
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
}

UAM_TEST(CustomPngDataUrlIsReusedUntilTheIconFileChanges)
{
	TempDir temp("uam-custom-icon-cache");
	const CustomIcon icon{"png", "11111111-2222-4333-8444-555555555555.png"};
	const fs::path file = temp.root / "icons" / icon.value;
	fs::create_directories(file.parent_path());
	std::string first = FixturePng(4096);
	UAM_ASSERT(uam::io::WriteBinaryFile(file, first));
	const std::string original = uam::icons::PngDataUrl(temp.root, icon);
	UAM_ASSERT(original.starts_with("data:image/png;base64,"));
	const fs::file_time_type stamp = fs::last_write_time(file);
	// Same size and timestamp: served from the cache without rereading the file.
	std::string same_shape = first;
	same_shape.back() = 'q';
	UAM_ASSERT(uam::io::WriteBinaryFile(file, same_shape));
	fs::last_write_time(file, stamp);
	UAM_ASSERT_EQ(uam::icons::PngDataUrl(temp.root, icon), original);
	// A newer timestamp or different size re-encodes the replacement.
	fs::last_write_time(file, stamp + std::chrono::seconds(2));
	const std::string touched = uam::icons::PngDataUrl(temp.root, icon);
	UAM_ASSERT(touched != original);
	std::string encoded_replacement;
	UAM_ASSERT(uam::base64::Decode(touched.substr(std::string_view("data:image/png;base64,").size()), encoded_replacement));
	UAM_ASSERT_EQ(encoded_replacement, same_shape);
	UAM_ASSERT(uam::io::WriteBinaryFile(file, FixturePng(8192)));
	UAM_ASSERT(uam::icons::PngDataUrl(temp.root, icon).size() > touched.size());
	// Validation still runs before the cache: a removed asset yields no data URL.
	fs::remove(file);
	UAM_ASSERT(uam::icons::PngDataUrl(temp.root, icon).empty());
}

namespace
{
	/// <summary>Representative large catalog: six providers, 20 folders, idle unloaded chats and a few retained transcripts.</summary>
	void BuildLargeCatalogFixture(uam::AppState& app, const fs::path& root, int chat_count)
	{
		app.data_root = root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
		app.provider_model_catalog->Initialize(app.data_root);
		const std::vector<std::string> providers = {"claude-cli", "codex-cli", "gemini-cli", "opencode-cli", "copilot-cli", "antigravity-cli"};
		for (int index = 0; index < 20; ++index)
		{
			ChatFolder folder;
			folder.id = "folder-" + std::to_string(index);
			folder.title = folder.id;
			folder.directory = (root / folder.id).generic_string();
			app.folders.push_back(std::move(folder));
		}
		for (int index = 0; index < chat_count; ++index)
		{
			ChatSession chat;
			chat.id = "catalog-chat-" + std::to_string(index);
			chat.title = "Catalog chat " + std::to_string(index);
			chat.provider_id = providers[index % providers.size()];
			chat.model_id = index % 3 == 0 ? "" : "model-" + std::to_string(index % 5);
			chat.folder_id = "folder-" + std::to_string(index % 20);
			chat.created_at = "2026-10-01T10:00:00Z";
			chat.updated_at = "2026-10-25T10:00:00Z";
			chat.last_opened_at = "2026-10-25T10:00:00Z";
			if (index < 8)
			{
				for (int message = 0; message < 200; ++message)
					chat.messages.push_back(Message{message % 2 == 0 ? MessageRole::User : MessageRole::Assistant,
					    "Retained " + std::to_string(message) + ": " + std::string(1024, 'r')});
			}
			else
			{
				chat.messages_loaded = false;
				chat.persisted_message_count = 120;
				chat.persisted_messages_digest = "persisted-" + std::to_string(index);
			}
			app.chats.push_back(std::move(chat));
		}
		app.selected_chat_index = 0;
	}

	double Percentile95(std::vector<double> values)
	{
		std::sort(values.begin(), values.end());
		return values.empty() ? 0.0 : values[std::min(values.size() - 1, (values.size() * 95) / 100)];
	}
}

namespace
{
	std::string g_observed_state_patch;

	/// <summary>Captures built statePatch messages and stops observing even when an assertion throws.</summary>
	struct StatePatchObserver
	{
		StatePatchObserver() { uam::SetStatePatchObserverForTests([](const std::string& message) { g_observed_state_patch = message; }); }
		~StatePatchObserver() { uam::SetStatePatchObserverForTests(nullptr); }
	};

	struct SummaryMutation
	{
		std::string label;
		std::function<std::vector<std::size_t>()> mutate;
		std::function<void(const nlohmann::json& chat)> check = {};
	};
}

UAM_TEST(StatePushPublishesEverySummaryInputChange)
{
	// Poll ticks compare per-chat summary digests instead of summary JSON; each listed input a summary shows
	// must publish exactly the affected chats once, and an unchanged tick must publish nothing.
	FakeStatePushClock clock;
	StatePatchObserver observer;
	TempDir temp("uam-summary-digest-coverage");
	uam::AppState app;
	BuildLargeCatalogFixture(app, temp.root, 40);
	// These two chats follow the global memory default, which a mutation below changes.
	app.chats[1].memory_level = uam::memory_levels::kDefault;
	app.chats[2].memory_level = uam::memory_levels::kDefault;
	uam::PushStateUpdate(nullptr, app);
	// The full update is the digest baseline, so an unchanged first tick publishes nothing.
	clock.Advance(std::chrono::milliseconds(200));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

	const CustomIcon icon{"png", "00000000-0000-4000-8000-0000000000aa.png"};
	fs::create_directories(temp.root / "icons");
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "icons" / icon.value, FixturePng(4096)));
	const auto chat = [&app](std::size_t index) -> ChatSession& { return app.chats[index]; };
	const auto one = [](std::size_t index) { return std::vector<std::size_t>{index}; };
	const std::vector<SummaryMutation> mutations = {
	    {"title", [&] { chat(12).title = "Renamed"; return one(12); }},
	    {"folder", [&] { chat(13).folder_id = "folder-3"; return one(13); }},
	    {"pinned", [&] { chat(14).pinned = true; return one(14); }},
	    {"model", [&] { chat(15).model_id = "model-new"; return one(15); }},
	    {"provider", [&] { chat(16).provider_id = "gemini-cli"; return one(16); }},
	    {"updated at", [&] { chat(17).updated_at = "2026-10-26T10:00:00Z"; return one(17); }},
	    {"persisted digest", [&] { chat(18).persisted_messages_digest = "persisted-changed"; return one(18); }},
	    {"persisted count", [&] { chat(18).persisted_message_count = 121; return one(18); }},
	    {"approval mode", [&] { chat(19).approval_mode = "plan"; return one(19); }},
	    {"reasoning effort", [&] { chat(20).reasoning_effort = "high"; return one(20); }},
	    {"workspace", [&] { chat(21).workspace_directory = (temp.root / "elsewhere").generic_string(); return one(21); }},
	    {"memory level", [&] { chat(22).memory_level = "off"; return one(22); }},
	    {"pending branch", [&] { chat(23).pending_git_branch_name = "feature/x"; return one(23); }},
	    {"active goal", [&] { chat(24).active_goal_id = "goal-1"; return one(24); }},
	    {"goal", [&] { chat(24).goals.push_back({}); chat(24).goals.back().id = "goal-1"; return one(24); }},
	    {"goal progress", [&] { chat(24).goals.back().tokens_used = 42; return one(24); }},
	    {"stop reason", [&] { chat(25).last_stop_reason = "end_turn"; return one(25); }},
	    {"retained transcript append", [&] { chat(3).messages.push_back(Message{MessageRole::User, "new"}); return one(3); }},
	    {"retained transcript edit", [&] { chat(4).messages[10].content = "edited"; return one(4); }},
	    {"selected transcript append", [&] { chat(0).messages.push_back(Message{MessageRole::User, "next"}); return one(0); }},
	    {"discarded prompt", [&] { chat(5).messages[10].acp_prompt_discarded = true; return one(5); },
	        [](const nlohmann::json& summary) { UAM_ASSERT_EQ(summary["acpSession"]["unsentMessageIndices"], nlohmann::json::array({10})); }},
	    {"unseen update", [&] { chat(26).last_opened_at.clear(); chat(26).attention_revision.clear(); app.chats_with_unseen_updates.insert(chat(26).id); return one(26); }},
	    {"routing wait", [&] { app.execution_routing_waits[chat(27).id] = "host busy"; return one(27); }},
	    {"restored unresolved reservation", [&] { app.execution_routing_restored_unresolved_chat_ids.insert(chat(37).id); return one(37); },
	        [](const nlohmann::json& summary) { UAM_ASSERT(summary["routingUnresolvedReservation"].get<bool>()); }},
	    {"native session", [&] { app.resolved_native_sessions_by_chat_id[chat(28).id] = "native-28"; return one(28); }},
	    {"managed run", [&] { AgentRun run; run.id = "run-29"; run.root_chat_id = chat(29).id; run.status = "running"; app.agent_runs.push_back(run); return one(29); },
	        [](const nlohmann::json& summary)
	        {
	            // Catalog hydration rebuilds the summary; the scan's count must survive it.
	            UAM_ASSERT(summary["acpSession"].contains("availableModels"));
	            UAM_ASSERT_EQ(summary["managedSubagentCount"].get<std::size_t>(), std::size_t{1});
	            UAM_ASSERT_EQ(summary["acpSession"]["backgroundTaskCount"].get<std::size_t>(), std::size_t{1});
	        }},
	    {"folder directory", [&] { app.folders[10].directory = (temp.root / "moved").generic_string(); return std::vector<std::size_t>{10, 30}; }},
	    {"computer use runtime", [&] { app.computer_use_by_chat_id[chat(32).id].state = "running"; return one(32); },
	        [](const nlohmann::json& summary) { UAM_ASSERT_EQ(summary["computerUse"]["state"], "running"); }},
	    {"cli terminal", [&]
	        {
	            auto terminal = std::make_unique<uam::CliTerminalState>();
	            terminal->terminal_id = "terminal-33";
	            terminal->frontend_chat_id = chat(33).id;
	            terminal->attached_chat_id = chat(33).id;
	            terminal->running = true;
	            terminal->lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	            app.cli_terminals.push_back(std::move(terminal));
	            return one(33);
	        },
	        [](const nlohmann::json& summary) { UAM_ASSERT_EQ(summary["cliTerminal"]["terminalId"], "terminal-33"); UAM_ASSERT(summary["cliTerminal"]["running"].get<bool>()); }},
	    {"cli terminal stop", [&] { app.cli_terminals.clear(); return one(33); }},
	    {"queued prompt", [&] { chat(34).acp_queued_prompts.push_back({}); chat(34).acp_queued_prompts.back().text = "later"; return one(34); },
	        [](const nlohmann::json& summary) { UAM_ASSERT_EQ(summary["acpSession"]["queuedPrompts"].size(), std::size_t{1}); }},
	    {"direct prompt", [&] { chat(35).acp_queued_prompts.push_back({}); chat(35).acp_queued_prompts.back().text = "now"; chat(35).acp_queued_prompts.back().direct_admission = true; return one(35); }},
	    {"quota continuation", [&] { chat(36).claude_quota_continuation.state = "waiting"; return one(36); },
	        [](const nlohmann::json& summary) { UAM_ASSERT_EQ(summary["acpSession"]["lifecycleState"], "waiting_quota"); }},
	    {"catalog refresh", [&]
	        {
	            const nlohmann::json models = nlohmann::json::array({{{"id", "fixture/refreshed"}, {"name", "Refreshed"}}});
	            UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels("claude-cli", models, (temp.root / "folder-6").generic_string()));
	            return one(6);
	        },
	        [](const nlohmann::json& summary) { UAM_ASSERT_EQ(summary["acpSession"]["availableModels"].size(), std::size_t{1}); }},
	    {"global memory default", [&]
	        {
	            std::vector<bool> before;
	            for (const ChatSession& entry : app.chats) before.push_back(uam::memory_levels::IsEnabled(uam::memory_policy::Resolve(app.settings, entry)));
	            app.settings.memory_level_default = "off";
	            std::vector<std::size_t> changed;
	            for (std::size_t index = 0; index < app.chats.size(); ++index)
	                if (uam::memory_levels::IsEnabled(uam::memory_policy::Resolve(app.settings, app.chats[index])) != before[index]) changed.push_back(index);
	            UAM_ASSERT_EQ(changed, (std::vector<std::size_t>{1, 2}));
	            return changed;
	        }},
	    {"png icon", [&] { chat(38).custom_icon = icon; return one(38); },
	        [](const nlohmann::json& summary) { UAM_ASSERT(summary["customIcon"].value("dataUrl", "").starts_with("data:image/png;base64,")); }},
	    {"live session start", [&] { app.acp_sessions.push_back(StreamingSession(chat(31), 1, 16)); return one(31); }},
	    {"live turn text", [&] { app.acp_sessions.back()->turn_events.back().text += " more"; return one(31); }},
	    {"live session stop", [&] { app.acp_sessions.clear(); return one(31); }},
	};
	const auto digests = [&app]
	{
		std::vector<std::uint64_t> values;
		for (const uam::StateSerializer::ChatSummaryDigest& entry : uam::StateSerializer::SerializeChatFingerprintDigests(app))
			values.push_back(entry.digest);
		return values;
	};
	const auto publish = [&](const std::string& label) -> nlohmann::json
	{
		clock.Advance(std::chrono::milliseconds(200));
		if (!uam::PushStateUpdateIfChanged(nullptr, app)) throw TestFailure(label + " change did not publish");
		const nlohmann::json patch = nlohmann::json::parse(g_observed_state_patch)["data"];
		clock.Advance(std::chrono::milliseconds(200));
		if (uam::PushStateUpdateIfChanged(nullptr, app)) throw TestFailure(label + " published again without a change");
		return patch;
	};
	for (const SummaryMutation& mutation : mutations)
	{
		const std::vector<std::uint64_t> before = digests();
		const std::vector<std::size_t> changed = mutation.mutate();
		const std::vector<std::uint64_t> after = digests();
		UAM_ASSERT_EQ(after.size(), before.size());
		for (std::size_t other = 0; other < after.size(); ++other)
		{
			// Exactly the affected chats' digests move; a digest that ignored the input would hide the change.
			if ((after[other] != before[other]) != (std::ranges::find(changed, other) != changed.end()))
				throw TestFailure(mutation.label + ": digest of chat " + std::to_string(other) + " did not track the change");
		}
		const nlohmann::json patch = publish(mutation.label);
		std::vector<std::string> expected_ids;
		for (const std::size_t index : changed) expected_ids.push_back(app.chats[index].id);
		std::vector<std::string> patched_ids;
		for (const nlohmann::json& summary : patch.value("chats", nlohmann::json::array())) patched_ids.push_back(summary.value("id", ""));
		std::ranges::sort(expected_ids);
		std::ranges::sort(patched_ids);
		if (patched_ids != expected_ids) throw TestFailure(mutation.label + ": patch carried " + std::to_string(patched_ids.size()) + " chats, expected " + std::to_string(expected_ids.size()));
		if (mutation.check)
			for (const nlohmann::json& summary : patch["chats"]) mutation.check(summary);
	}

	ChatSession added;
	added.id = "catalog-chat-added";
	added.title = added.id;
	added.provider_id = "codex-cli";
	added.messages_loaded = false;
	app.chats.push_back(added);
	nlohmann::json patch = publish("added chat");
	UAM_ASSERT_EQ(patch["chats"].size(), std::size_t{1});
	UAM_ASSERT_EQ(patch["chats"][0]["id"], added.id);
	app.chats.pop_back();
	patch = publish("removed chat");
	UAM_ASSERT(!patch.contains("chats"));
	UAM_ASSERT_EQ(patch["removedChatIds"], nlohmann::json::array({added.id}));
}

UAM_TEST(PerformanceLargeCatalogStateFingerprint)
{
	const WithoutStateCacheVerification unverified;
	for (const int chat_count : {1000, 5000})
	{
		TempDir temp("uam-performance-large-catalog");
		uam::AppState app;
		BuildLargeCatalogFixture(app, temp.root, chat_count);
		uam::PushStateUpdate(nullptr, app);
		std::vector<double> fingerprints;
		std::vector<double> ticks;
		for (int iteration = 0; iteration < 21; ++iteration)
		{
			std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
			const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app, true);
			const double fingerprint_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			UAM_ASSERT_EQ(fingerprint["chats"].size(), static_cast<std::size_t>(chat_count));
			// The poll-tick path: fingerprint plus per-chat diff against the pushed baselines.
			started = std::chrono::steady_clock::now();
			UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
			const double tick_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			if (iteration == 0) continue;
			fingerprints.push_back(fingerprint_ms);
			ticks.push_back(tick_ms);
		}
		std::cout << "[TIMING] large catalog " << chat_count << " chats: fingerprint p50=" << Median(fingerprints)
		          << " ms p95=" << Percentile95(fingerprints) << " ms; unchanged push tick p50=" << Median(ticks)
		          << " ms p95=" << Percentile95(ticks) << " ms\n";
		// The printed figures are the regression signal; the shared interaction budget only catches gross regressions.
		if (Median(ticks) > static_cast<double>(kInteractionBudget.count()))
			throw TestFailure(std::to_string(chat_count) + "-chat unchanged push tick p50 exceeded " + std::to_string(kInteractionBudget.count()) + " ms");
	}
}

namespace
{
	/// <summary>Shape of a long-lived install: mostly unloaded chats, PNG folder icons, live sessions, managed runs and retained transcripts.</summary>
	void BuildIdleInstallFixture(uam::AppState& app, const fs::path& root, int chat_count)
	{
		BuildLargeCatalogFixture(app, root, chat_count);
		fs::create_directories(root / "icons");
		nlohmann::json models = nlohmann::json::array();
		for (int index = 0; index < 256; ++index)
			models.push_back({{"id", "fixture/model-" + std::to_string(index)}, {"name", "Fixture model " + std::to_string(index)}});
		for (std::size_t index = 0; index < app.folders.size(); ++index)
		{
			const CustomIcon icon{"png", "00000000-0000-4000-8000-0000000000" + std::to_string(10 + index) + ".png"};
			UAM_ASSERT(uam::icons::IsValid(icon));
			UAM_ASSERT(uam::io::WriteBinaryFile(root / "icons" / icon.value, FixturePng(100 * 1024)));
			app.folders[index].custom_icon = icon;
			fs::create_directories(app.folders[index].directory);
			for (const char* provider : {"claude-cli", "codex-cli"})
				UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(provider, models, app.folders[index].directory));
		}
		// Chats 1..3 stream in the background; chat 4 keeps a transcript of about 60 MB of tool output in memory.
		for (int index = 1; index <= 3; ++index)
		{
			app.chats[index].messages.push_back(StreamingTurn(5, 50 * 1024));
			app.acp_sessions.push_back(StreamingSession(app.chats[index], 5, 50 * 1024));
		}
		for (int message = 0; message < 40; ++message) app.chats[4].messages.push_back(StreamingTurn(30, 50 * 1024));
		for (int index = 0; index < 24; ++index)
		{
			AgentRun run;
			run.id = "run-" + std::to_string(index);
			run.root_chat_id = app.chats[20 + index % 6].id;
			run.status = index % 2 == 0 ? "running" : "completed";
			app.agent_runs.push_back(std::move(run));
		}
	}

	template <class Work>
	double MedianMs(int iterations, Work&& work)
	{
		std::vector<double> samples;
		for (int iteration = 0; iteration < iterations; ++iteration)
		{
			const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
			work();
			samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
		}
		return Median(samples);
	}
}

namespace
{
	/// <summary>Runs the path-probe memo on the stepped push clock and restores the real clock even when an assertion throws.</summary>
	struct FakePathProbeClock
	{
		FakePathProbeClock() { uam::paths::SetRecentPathProbeClockForTests([] { return g_fake_push_now; }); }
		~FakePathProbeClock() { uam::paths::SetRecentPathProbeClockForTests(nullptr); }
	};
}

// Guarded by assertions that fail without the change: the per-push path memo on both display readers (probe
// counts), and hashing no transcript on an unchanged push and only the changed one otherwise (hashed message counts).
// Not guarded here: hashing the catalogue token from the JSON tree instead of a dump, which only the printed
// timings show, and the 250 ms budget, which the code before these changes also met.
UAM_TEST(PerformanceIdleInstallPushTick)
{
	const WithoutStateCacheVerification unverified;
	FakeStatePushClock clock;
	FakePathProbeClock probe_clock;
	StatePatchObserver observer;
	for (const int chat_count : {500, 2000})
	{
		TempDir temp("uam-performance-idle-install");
		uam::AppState app;
		BuildIdleInstallFixture(app, temp.root, chat_count);
		// Expire every memo entry, then require each display reader to probe through the memo exactly once per path.
		// A reader that goes back to probing the disk directly leaves its own count where it was.
		using uam::paths::RecentPathProbeCount;
		using uam::paths::RecentPathProbeKind;
		const auto probes = [] { return RecentPathProbeCount(RecentPathProbeKind::CatalogWorkspace) + RecentPathProbeCount(RecentPathProbeKind::WorkspaceOwnership); };
		clock.Advance(std::chrono::seconds(10));
		std::size_t catalog_probes = RecentPathProbeCount(RecentPathProbeKind::CatalogWorkspace);
		(void)app.provider_model_catalog->GetCatalogSnapshot("claude-cli", app.folders[0].directory, "local");
		UAM_ASSERT_EQ(RecentPathProbeCount(RecentPathProbeKind::CatalogWorkspace).load(), catalog_probes + 1);
		(void)app.provider_model_catalog->GetCatalogSnapshot("codex-cli", app.folders[0].directory, "local");
		UAM_ASSERT_EQ(RecentPathProbeCount(RecentPathProbeKind::CatalogWorkspace).load(), catalog_probes + 1);
		clock.Advance(std::chrono::seconds(10));
		catalog_probes = RecentPathProbeCount(RecentPathProbeKind::CatalogWorkspace);
		UAM_ASSERT(!app.provider_model_catalog->GetCatalogScopes(true).empty());
		UAM_ASSERT(RecentPathProbeCount(RecentPathProbeKind::CatalogWorkspace).load() > catalog_probes);
		clock.Advance(std::chrono::seconds(10));
		const std::size_t ownership_probes = RecentPathProbeCount(RecentPathProbeKind::WorkspaceOwnership);
		uam::PushStateUpdate(nullptr, app);
		UAM_ASSERT(RecentPathProbeCount(RecentPathProbeKind::WorkspaceOwnership).load() - ownership_probes >= app.folders.size());
		UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

		// Repeated pushes inside the memo window (the stepped clock does not move) must not probe again,
		// and must hash each loaded transcript once.
		const std::size_t probes_before = probes();
		std::size_t hashed_before = uam::StateSerializer::HashedTranscriptMessageCountForTests();
		const std::size_t trusted_before = uam::StateSerializer::TrustedCacheReadCount();
		for (int repeat = 0; repeat < 3; ++repeat) UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
		UAM_ASSERT_EQ(probes(), probes_before);
		// Unchanged transcripts are vouched for by their revision stamps: nothing is hashed, whatever their size.
		UAM_ASSERT_EQ(uam::StateSerializer::HashedTranscriptMessageCountForTests(), hashed_before);
		UAM_ASSERT(uam::StateSerializer::TrustedCacheReadCount() > trusted_before);
		// Trusting the stamps owes one verifying push when the reconciliation interval ends, and only one.
		UAM_ASSERT(!uam::StateReconciliationDue());
		clock.Advance(std::chrono::seconds(3));
		UAM_ASSERT(uam::StateReconciliationDue());
		UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
		UAM_ASSERT(!uam::StateReconciliationDue());
		clock.Advance(std::chrono::seconds(3));
		UAM_ASSERT(!uam::StateReconciliationDue());
		UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

		clock.Advance(std::chrono::milliseconds(120));
		app.chats[4].messages.back().content += " first token";
		hashed_before = uam::StateSerializer::HashedTranscriptMessageCountForTests();
		UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
		// Only the changed transcript is hashed, and once: its summary build and catalog hydration reuse the new digest.
		UAM_ASSERT_EQ(uam::StateSerializer::HashedTranscriptMessageCountForTests() - hashed_before, app.chats[4].messages.size());

		const double slices = MedianMs(11, [&] { (void)uam::StateSerializer::SerializeFingerprint(app, true, false); });
		const double digests = MedianMs(11, [&] { (void)uam::StateSerializer::SerializeChatFingerprintDigests(app); });
		// (a) nothing changed.
		const double unchanged = MedianMs(21, [&] { UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app)); });
		// (b) one unloaded chat changed on every tick; each change must be published as exactly that chat.
		int revision = 0;
		const double one_changed = MedianMs(21, [&]
		{
			clock.Advance(std::chrono::milliseconds(120));
			app.chats[chat_count - 1].title = "Renamed " + std::to_string(++revision);
			g_observed_state_patch.clear();
			UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
			const nlohmann::json patch = nlohmann::json::parse(g_observed_state_patch)["data"];
			UAM_ASSERT_EQ(patch["chats"].size(), std::size_t{1});
			UAM_ASSERT_EQ(patch["chats"][0]["title"], app.chats[chat_count - 1].title);
		});
		// (c) the chat holding the very large loaded transcript changed (a streamed token on its last message).
		const double large_changed = MedianMs(21, [&]
		{
			clock.Advance(std::chrono::milliseconds(120));
			app.chats[4].messages.back().content += " token";
			g_observed_state_patch.clear();
			UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
			const nlohmann::json patch = nlohmann::json::parse(g_observed_state_patch)["data"];
			UAM_ASSERT_EQ(patch["chats"].size(), std::size_t{1});
			UAM_ASSERT_EQ(patch["chats"][0]["id"], app.chats[4].id);
		});
		UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
		std::cout << "[TIMING] idle install " << chat_count << " chats: slices=" << slices << " ms digests=" << digests
		          << " ms; push tick p50 unchanged=" << unchanged << " ms, one chat changed=" << one_changed
		          << " ms, large loaded chat changed=" << large_changed << " ms\n";
		// The printed figures are the regression signal; the shared interaction budget only catches gross regressions.
		if (std::max({unchanged, one_changed, large_changed}) > static_cast<double>(kInteractionBudget.count()))
			throw TestFailure(std::to_string(chat_count) + "-chat idle install push tick p50 exceeded " + std::to_string(kInteractionBudget.count()) + " ms");
	}
}

UAM_TEST(StatePushReconciliationRepairsAWriteThatBypassedTheTranscriptStamp)
{
	const WithoutStateCacheVerification unverified;
	FakeStatePushClock clock;
	StatePatchObserver observer;
	uam::AppState app;
	for (const char* id : {"selected", "background"})
	{
		ChatSession chat;
		chat.id = id;
		chat.provider_id = "claude-cli";
		chat.messages.push_back(Message{MessageRole::User, "Hello"});
		chat.messages.push_back(Message{MessageRole::Assistant, "First answer"});
		app.chats.push_back(std::move(chat));
	}
	app.selected_chat_index = 0;
	// Start a fresh reconciliation interval: this push verifies, the ones in the next three seconds trust the stamps.
	clock.Advance(std::chrono::seconds(3));
	uam::PushStateUpdate(nullptr, app);

	// The supported way to write: any non-const access restamps the transcript, so the next push publishes it.
	clock.Advance(std::chrono::milliseconds(120));
	app.chats[1].messages.back().content = "Second answer";
	g_observed_state_patch.clear();
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(nlohmann::json::parse(g_observed_state_patch)["data"]["chats"][0]["id"], "background");

	// The one write the stamp cannot see: through a reference kept from before the digest was read.
	Message& kept = app.chats[1].messages.back();
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	kept.content = "Third answer";
	clock.Advance(std::chrono::milliseconds(120));
	const std::size_t divergences_before = uam::StateSerializer::CacheDivergenceCount();
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::CacheDivergenceCount(), divergences_before);

	// It is not lost: the poll loop owes a verifying push when the interval ends, which finds, repairs and publishes it.
	UAM_ASSERT(!uam::StateReconciliationDue());
	clock.Advance(std::chrono::seconds(3));
	UAM_ASSERT(uam::StateReconciliationDue());
	g_observed_state_patch.clear();
	++uam_test::g_expected_state_cache_divergences;
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::CacheDivergenceCount(), divergences_before + 1);
	UAM_ASSERT_EQ(nlohmann::json::parse(g_observed_state_patch)["data"]["chats"][0]["id"], "background");
	UAM_ASSERT(!uam::StateReconciliationDue());
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
}

// The frontend's gap repair relies on this contract: every published patch, including a corrective one, is exactly
// the last revision + 1, a push that publishes nothing consumes no revision, and a getInitialState snapshot at revision
// R already holds every change up to R, even one a trusting push missed.
UAM_TEST(StatePushRevisionsStepByOneAndAnExactSnapshotHoldsWhatATrustingPushMissed)
{
	const WithoutStateCacheVerification unverified;
	FakeStatePushClock clock;
	StatePatchObserver observer;
	uam::AppState app;
	for (const char* id : {"selected", "background"})
	{
		ChatSession chat;
		chat.id = id;
		chat.provider_id = "claude-cli";
		chat.messages.push_back(Message{MessageRole::User, "Hello"});
		chat.messages.push_back(Message{MessageRole::Assistant, "First answer"});
		app.chats.push_back(std::move(chat));
	}
	app.selected_chat_index = 0;
	clock.Advance(std::chrono::seconds(3));
	uam::PushStateUpdate(nullptr, app);
	std::uint64_t revision = app.state_revision;
	const auto published = [] { return nlohmann::json::parse(g_observed_state_patch)["data"]; };
	const auto background_digest = [](const nlohmann::json& chats)
	{
		for (const nlohmann::json& chat : chats)
			if (chat.value("id", "") == "background") return chat.value("messagesDigest", std::string{});
		return std::string{};
	};

	clock.Advance(std::chrono::milliseconds(120));
	app.chats[1].messages.back().content = "Second answer";
	g_observed_state_patch.clear();
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(published()["stateRevision"].get<std::uint64_t>(), revision + 1);
	UAM_ASSERT_EQ(app.state_revision, revision + 1);
	revision = app.state_revision;

	// A write through a reference kept from before the digest was read is missed by the trusting push.
	Message& kept = app.chats[1].messages.back();
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	kept.content = "Third answer";
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(app.state_revision, revision);

	// getInitialState serializes exactly, so a snapshot at the same revision already holds the missed write.
	const nlohmann::json snapshot = uam::StateSerializer::Serialize(app, true);
	UAM_ASSERT_EQ(snapshot["stateRevision"].get<std::uint64_t>(), revision);

	// The verifying push repairs it with an ordinary patch at revision + 1 that agrees with the snapshot.
	clock.Advance(std::chrono::seconds(3));
	g_observed_state_patch.clear();
	++uam_test::g_expected_state_cache_divergences;
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	const nlohmann::json corrective = published();
	UAM_ASSERT_EQ(corrective["stateRevision"].get<std::uint64_t>(), revision + 1);
	UAM_ASSERT_EQ(app.state_revision, revision + 1);
	UAM_ASSERT(!background_digest(snapshot["chats"]).empty());
	UAM_ASSERT_EQ(background_digest(snapshot["chats"]), background_digest(corrective["chats"]));
}

UAM_TEST(PerformancePushDoesNotScanEveryManagedRunForEveryChat)
{
	const WithoutStateCacheVerification unverified;
	TempDir temp("uam-performance-managed-runs");
	uam::AppState app;
	BuildLargeCatalogFixture(app, temp.root, 2000);
	uam::PushStateUpdate(nullptr, app);
	const double without_runs = MedianMs(11, [&] { UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app)); });

	// A long-lived install keeps finished managed runs; each chat's summary needs only its own active count.
	for (int index = 0; index < 50000; ++index)
	{
		AgentRun run;
		run.id = "run-" + std::to_string(index);
		run.root_chat_id = app.chats[1000 + index % 1000].id;
		run.status = index == 0 ? "running" : "completed";
		app.agent_runs.push_back(std::move(run));
	}
	StatePatchObserver observer;
	g_observed_state_patch.clear();
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	const nlohmann::json patch = nlohmann::json::parse(g_observed_state_patch)["data"];
	// Only the chat that gained an active run changes, with that run counted.
	const nlohmann::json& changed = patch["chats"];
	UAM_ASSERT(std::ranges::any_of(changed, [&](const nlohmann::json& chat)
	{ return chat["id"] == app.chats[1000].id && chat["acpSession"]["backgroundTaskCount"] == 1; }));
	const double with_runs = MedianMs(11, [&] { UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app)); });
	std::cout << "[TIMING] 2000 chats unchanged push: no managed runs=" << without_runs << " ms, 50000 managed runs=" << with_runs << " ms\n";
	// Scanning every run for every chat adds 100 million comparisons, several hundred milliseconds; the index adds one pass over the runs.
	if (with_runs - without_runs > 50.0)
		throw TestFailure("50000 managed runs added " + std::to_string(with_runs - without_runs) + " ms to an unchanged push; limit 50 ms");
}

UAM_TEST(StatePushStampsAreNotPinnedByOtherReadersAndTheirCostIsVisible)
{
	const WithoutStateCacheVerification unverified;
	FakeStatePushClock clock;
	StatePatchObserver observer;
	uam::AppState app;
	for (const char* id : {"selected", "background"})
	{
		ChatSession chat;
		chat.id = id;
		chat.provider_id = "claude-cli";
		chat.messages.push_back(Message{MessageRole::User, "Hello"});
		chat.messages.push_back(Message{MessageRole::Assistant, "First answer"});
		app.chats.push_back(std::move(chat));
	}
	app.selected_chat_index = 0;
	clock.Advance(std::chrono::seconds(3));
	uam::PushStateUpdate(nullptr, app);
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

	// A reader outside a push (a message page, a handler's reply) computes a digest between taking a reference and
	// writing through it. It must not leave that digest behind for the next push to trust.
	Message& kept = app.chats[1].messages.back();
	const std::string before = uam::StateSerializer::MessageDigest(app.chats[1]);
	kept.content = "Second answer";
	UAM_ASSERT(uam::StateSerializer::MessageDigest(app.chats[1]) != before);
	clock.Advance(std::chrono::milliseconds(120));
	g_observed_state_patch.clear();
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(nlohmann::json::parse(g_observed_state_patch)["data"]["chats"][0]["id"], "background");

	// A non-const read restamps without changing anything: the push rehashes that transcript, publishes nothing, and counts it.
	const std::size_t restamped_before = uam::StateSerializer::RestampedUnchangedTranscriptCount();
	const std::size_t hashed_before = uam::StateSerializer::HashedTranscriptMessageCountForTests();
	(void)app.chats[1].messages.back();
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::RestampedUnchangedTranscriptCount(), restamped_before + 1);
	UAM_ASSERT_EQ(uam::StateSerializer::HashedTranscriptMessageCountForTests(), hashed_before + app.chats[1].messages.size());

	// The poll loop wakes for the owed reconciliation instead of sleeping through its idle second.
	// The reconciling push's hashing shows in the hash count.
	UAM_ASSERT_EQ(uam::CapPollDelayForDeferredStatePush(1000), 1000);
	clock.Advance(std::chrono::milliseconds(2140));
	UAM_ASSERT(!uam::StateReconciliationDue());
	UAM_ASSERT_EQ(uam::CapPollDelayForDeferredStatePush(1000), 500);
	clock.Advance(std::chrono::milliseconds(500));
	UAM_ASSERT(uam::StateReconciliationDue());
	UAM_ASSERT_EQ(uam::CapPollDelayForDeferredStatePush(1000), 1);
	const std::size_t verify_hashed_before = uam::StateSerializer::HashedTranscriptMessageCountForTests();
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(uam::StateSerializer::HashedTranscriptMessageCountForTests() > verify_hashed_before);
	UAM_ASSERT(!uam::StateReconciliationDue());
	UAM_ASSERT_EQ(uam::CapPollDelayForDeferredStatePush(1000), 1000);
}

UAM_TEST(StatePushRebuildsModelCataloguesOnlyWhenTheirInputsChange)
{
	const WithoutStateCacheVerification unverified;
	FakeStatePushClock clock;
	StatePatchObserver observer;
	TempDir temp("uam-catalogue-generation");
	uam::AppState app;
	BuildIdleInstallFixture(app, temp.root, 60);
	clock.Advance(std::chrono::seconds(3));
	uam::PushStateUpdate(nullptr, app);
	clock.Advance(std::chrono::milliseconds(120));
	// The first push after the baselines were replaced compares the slice once more.
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

	// Unchanged inputs: the slice (every provider and folder's model list) is not built at all.
	std::size_t builds = uam::StateSerializer::CatalogSliceBuildCountForTests();
	for (int repeat = 0; repeat < 3; ++repeat)
	{
		clock.Advance(std::chrono::milliseconds(120));
		UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	}
	UAM_ASSERT_EQ(uam::StateSerializer::CatalogSliceBuildCountForTests(), builds);

	const auto pushed_catalogs = [&]
	{
		clock.Advance(std::chrono::milliseconds(120));
		g_observed_state_patch.clear();
		UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
		const nlohmann::json patch = nlohmann::json::parse(g_observed_state_patch)["data"];
		UAM_ASSERT(patch.contains("providerModelCatalogs"));
		return patch["providerModelCatalogs"];
	};

	// Every write to the service takes its write lock, which advances the generation: the next push publishes it.
	const std::uint64_t generation = app.provider_model_catalog->Generation();
	nlohmann::json models = nlohmann::json::array({{{"id", "fixture/only-model"}, {"name", "Only model"}}});
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels("claude-cli", models, app.folders[0].directory));
	UAM_ASSERT(app.provider_model_catalog->Generation() != generation);
	nlohmann::json catalogs = pushed_catalogs();
	UAM_ASSERT(std::ranges::any_of(catalogs, [](const nlohmann::json& catalog)
	{ return catalog["providerId"] == "claude-cli" && catalog["availableModels"].size() == 1 && catalog["availableModels"][0]["id"] == "fixture/only-model"; }));

	app.provider_model_catalog->RememberRefreshFailure("codex-cli", "Fixture refresh failed", app.folders[1].directory);
	catalogs = pushed_catalogs();
	UAM_ASSERT(std::ranges::any_of(catalogs, [](const nlohmann::json& catalog) { return catalog["modelRefreshError"] == "Fixture refresh failed"; }));

	// Inputs outside the service are compared directly: a folder moved to another directory rebuilds the slice.
	const fs::path moved = temp.root / "moved-folder";
	fs::create_directories(moved);
	builds = uam::StateSerializer::CatalogSliceBuildCountForTests();
	app.folders[2].directory = moved.generic_string();
	clock.Advance(std::chrono::milliseconds(120));
	(void)uam::PushStateUpdateIfChanged(nullptr, app);
	UAM_ASSERT_EQ(uam::StateSerializer::CatalogSliceBuildCountForTests(), builds + 1);

	// And a checked CLI version, which decides the launch options shown for each model.
	builds = uam::StateSerializer::CatalogSliceBuildCountForTests();
	uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("claude-cli", "local")];
	version.checked = true;
	version.supported = true;
	version.installed_version = "9.9.9";
	clock.Advance(std::chrono::milliseconds(120));
	(void)uam::PushStateUpdateIfChanged(nullptr, app);
	UAM_ASSERT_EQ(uam::StateSerializer::CatalogSliceBuildCountForTests(), builds + 1);

	// Settled again: no further builds until the three-second reconciliation push, which builds and compares it.
	builds = uam::StateSerializer::CatalogSliceBuildCountForTests();
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::CatalogSliceBuildCountForTests(), builds);
	clock.Advance(std::chrono::seconds(3));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::CatalogSliceBuildCountForTests(), builds + 1);
}

UAM_TEST(StatePushSerializesLiveToolCallsOnlyWhenTheyChange)
{
	const WithoutStateCacheVerification unverified;
	FakeStatePushClock clock;
	StatePatchObserver observer;
	TempDir temp("uam-live-tool-calls");
	uam::AppState app;
	BuildStreamingPushFixture(app, temp.root);
	clock.Advance(std::chrono::seconds(3));
	uam::PushStateUpdate(nullptr, app);
	clock.Advance(std::chrono::milliseconds(120));
	(void)uam::PushStateUpdateIfChanged(nullptr, app);

	// Streaming text changes the turn events of the three live chats, not their tool calls (40 calls of 50 KB each).
	std::size_t serialized = uam::StateSerializer::LiveToolCallSerializationCountForTests();
	for (int token = 0; token < 5; ++token)
	{
		AppendStreamToken(app, token);
		clock.Advance(std::chrono::milliseconds(120));
		g_observed_state_patch.clear();
		UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
		const nlohmann::json patch = nlohmann::json::parse(g_observed_state_patch)["data"];
		UAM_ASSERT_EQ(patch["chats"].size(), std::size_t{3});
		// The published summary still carries every tool call.
		UAM_ASSERT_EQ(patch["chats"][0]["acpSession"]["toolCalls"].size(), app.acp_sessions[0]->tool_calls.size());
	}
	UAM_ASSERT_EQ(uam::StateSerializer::LiveToolCallSerializationCountForTests(), serialized);

	// A tool call that changes is serialized again for its session only, and published.
	app.acp_sessions[1]->tool_calls.back().status = "failed";
	clock.Advance(std::chrono::milliseconds(120));
	g_observed_state_patch.clear();
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	const nlohmann::json patch = nlohmann::json::parse(g_observed_state_patch)["data"];
	UAM_ASSERT_EQ(patch["chats"].size(), std::size_t{1});
	UAM_ASSERT_EQ(patch["chats"][0]["id"], app.chats[1].id);
	UAM_ASSERT_EQ(patch["chats"][0]["acpSession"]["toolCalls"].back()["status"], "failed");
	UAM_ASSERT(uam::StateSerializer::LiveToolCallSerializationCountForTests() > serialized);
	UAM_ASSERT(uam::StateSerializer::LiveToolCallSerializationCountForTests() <= serialized + 3);
}

UAM_TEST(StatePushReconciliationRepairsABypassedToolCallStampAndCatalogueMemo)
{
	const WithoutStateCacheVerification unverified;
	FakeStatePushClock clock;
	FakePathProbeClock probe_clock;
	StatePatchObserver observer;
	TempDir temp("uam-reconcile-tool-calls");
	uam::AppState app;
	BuildStreamingPushFixture(app, temp.root);
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);
	clock.Advance(std::chrono::seconds(10));
	uam::PushStateUpdate(nullptr, app);
	clock.Advance(std::chrono::milliseconds(120));
	(void)uam::PushStateUpdateIfChanged(nullptr, app);

	// A tool call written through a reference kept from before the push read the stamp.
	uam::AcpToolCallState& kept = app.acp_sessions[1]->tool_calls.back();
	clock.Advance(std::chrono::milliseconds(120));
	(void)uam::PushStateUpdateIfChanged(nullptr, app);
	kept.status = "failed";
	clock.Advance(std::chrono::milliseconds(120));
	std::size_t divergences = uam::StateSerializer::CacheDivergenceCount();
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::CacheDivergenceCount(), divergences);
	clock.Advance(std::chrono::seconds(3));
	UAM_ASSERT(uam::StateReconciliationDue());
	g_observed_state_patch.clear();
	++uam_test::g_expected_state_cache_divergences;
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::CacheDivergenceCount(), divergences + 1);
	nlohmann::json patch = nlohmann::json::parse(g_observed_state_patch)["data"];
	UAM_ASSERT_EQ(patch["chats"].size(), std::size_t{1});
	UAM_ASSERT_EQ(patch["chats"][0]["id"], app.chats[1].id);
	UAM_ASSERT_EQ(patch["chats"][0]["acpSession"]["toolCalls"].back()["status"], "failed");

	// A non-const read of the tool calls restamps them: rebuilt, unchanged, counted.
	const std::size_t restamped = uam::StateSerializer::RestampedUnchangedToolCallCount();
	(void)app.acp_sessions[2]->tool_calls.back();
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::RestampedUnchangedToolCallCount(), restamped + 1);

#if !defined(_WIN32)
	// The catalogue memo's one input outside its key is how a workspace path resolves on disk. Retarget a folder's
	// symlink to a directory with a different model list: trusting pushes keep the old list, the verifying push
	// (after the path memo has expired too) rebuilds it, reports the difference and publishes the new one.
	const fs::path first = temp.root / "catalogue-first";
	const fs::path second = temp.root / "catalogue-second";
	const fs::path link = temp.root / "catalogue-link";
	fs::create_directories(first);
	fs::create_directories(second);
	fs::create_directory_symlink(first, link);
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "fixture/first"}}}), first.generic_string()));
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "fixture/second"}}}), second.generic_string()));
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.folders[0].directory = link.generic_string();
	clock.Advance(std::chrono::seconds(10));
	(void)uam::PushStateUpdateIfChanged(nullptr, app);
	clock.Advance(std::chrono::milliseconds(120));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

	fs::remove(link);
	fs::create_directory_symlink(second, link);
	clock.Advance(std::chrono::milliseconds(120));
	divergences = uam::StateSerializer::CacheDivergenceCount();
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(uam::StateSerializer::CacheDivergenceCount(), divergences);
	clock.Advance(std::chrono::seconds(10));
	g_observed_state_patch.clear();
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	const std::size_t found = uam::StateSerializer::CacheDivergenceCount() - divergences;
	UAM_ASSERT(found >= 1);
	uam_test::g_expected_state_cache_divergences += found;
	patch = nlohmann::json::parse(g_observed_state_patch)["data"];
	UAM_ASSERT(patch.contains("providerModelCatalogs"));
	UAM_ASSERT(std::ranges::any_of(patch["providerModelCatalogs"], [&](const nlohmann::json& catalog)
	{
		return catalog["providerId"] == "claude-cli" && catalog["workspaceDirectory"] == link.generic_string() &&
		    catalog["availableModels"].size() == 1 && catalog["availableModels"][0]["id"] == "fixture/second";
	}));
#endif
}
