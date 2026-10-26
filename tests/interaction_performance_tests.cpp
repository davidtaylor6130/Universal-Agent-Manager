#include "test_harness.h"
#include "app/transcript_retention_service.h"
#include "common/config/custom_icon.h"

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
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	const std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(summaries.size(), std::size_t{1000});
	for (const ChatSession& chat : summaries)
	{
		UAM_ASSERT(chat.messages.empty());
		UAM_ASSERT_EQ(chat.persisted_message_count, std::size_t{2});
	}
	RequireBudget("cold 1000-chat summary catalogue with cache regeneration", started, kHistoryPageBudget);
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
	const int windows = static_cast<int>((kTicks * kTick) / std::chrono::milliseconds(100)) + 1;
	double per_second = 0.0;
	std::vector<double> calls;
	int pushes = 0;
	for (int run = 0; run < 3; ++run)
	{
		uam::PushStateUpdate(nullptr, app);
		std::vector<double> run_calls;
		int run_pushes = 0;
		for (int tick = 0; tick < kTicks; ++tick)
		{
			const bool streamed = tick % 2 == 0;
			if (streamed) AppendStreamToken(app, run * kTicks + tick);
			if (streamed || uam::DeferredStatePushDue())
			{
				const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
				run_pushes += uam::PushStateUpdateIfChanged(nullptr, app) ? 1 : 0;
				run_calls.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
			}
			clock.Advance(kTick);
		}

		// Three streaming chats share one 100 ms window each: at most one push per window per chat (plus the first),
		// and quiet ticks rebuild only when a deferred summary is due.
		UAM_ASSERT(run_pushes <= windows + 1);
		UAM_ASSERT(static_cast<int>(run_calls.size()) <= kTicks / 2 + windows);

		// Final streamed state must reach the frontend: flush deferred work, then nothing may remain unsent.
		for (int tick = 0; tick < 20 && uam::HasDeferredStatePush(); ++tick)
		{
			clock.Advance(kTick);
			if (uam::DeferredStatePushDue()) uam::PushStateUpdateIfChanged(nullptr, app);
		}
		UAM_ASSERT(!uam::HasDeferredStatePush());
		clock.Advance(std::chrono::milliseconds(120));
		UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

		const double run_per_second = std::accumulate(run_calls.begin(), run_calls.end(), 0.0) / window_seconds;
		if (run == 0 || run_per_second < per_second)
		{
			per_second = run_per_second;
			calls = std::move(run_calls);
			pushes = run_pushes;
		}
	}

	std::cout << "[TIMING] streaming push fixture: fingerprint p50=" << Median(fingerprints) << " ms; ticks=62 rebuilds="
	          << calls.size() << " pushes=" << pushes << " per-rebuild p50=" << Median(calls) << " ms; UI-thread push cost="
	          << per_second << " ms/s, cheapest of 3 windows\n";
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
