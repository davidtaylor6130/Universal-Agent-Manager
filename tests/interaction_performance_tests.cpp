#include "test_harness.h"
#include "app/transcript_retention_service.h"

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
