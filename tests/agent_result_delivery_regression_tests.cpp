#include "test_harness.h"
#include "common/platform/published_file_change_witness.h"

using namespace uam_test;

namespace
{
	struct AgentResultDeliveryFixture
	{
		TempDir temp{"uam-agent-result-delivery"};
		uam::AppState app;
		AgentRun run;

		/// `history` adds that many authored rows of about 1 KiB each before the parent is first saved.
		explicit AgentResultDeliveryFixture(std::size_t history = 0)
		{
			app.data_root = temp.root / "data";
			ChatSession parent;
			parent.id = "result-parent";
			parent.branch_root_chat_id = parent.id;
			parent.provider_id = "codex-cli";
			parent.created_at = "2026-10-21 10:00:00";
			parent.updated_at = parent.created_at;
			parent.managed_followup_incarnation = uam::AgentRunLedger::NewRunId();
			Message first;
			first.role = MessageRole::User;
			first.content = "Authored parent message A.";
			first.created_at = parent.created_at;
			parent.messages.push_back(first);
			for (std::size_t index = 0; index < history; ++index)
			{
				Message row;
				row.role = index % 2 == 0 ? MessageRole::Assistant : MessageRole::User;
				row.content = "History row " + std::to_string(index) + ": " + std::string(1000, static_cast<char>('a' + index % 26));
				row.created_at = parent.created_at;
				parent.messages.push_back(std::move(row));
			}
			UAM_ASSERT(parent.messages_loaded);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, parent));
			app.chats.push_back(parent);

			run.id = uam::AgentRunLedger::NewRunId();
			run.root_chat_id = parent.id;
			run.transcript_chat_id = "result-transcript-" + run.id;
			run.agent_id = "reviewer";
			run.definition_hash = "fixture-definition-identity";
			run.definition_snapshot = "---\nversion: 1\n---\nReview the requested change.";
			run.provider_id = parent.provider_id;
			run.execution_capability = "uam-prompt-injected";
			run.task = "Review the requested change.";
			run.effective_workspace_access = "read";
			run.status = "completed";
			run.depth = 1;
			run.expected_turn_serial = 1;
			run.started_at_epoch_ms = 1000;
			run.finished_at_epoch_ms = 1100;
			run.deadline_at_epoch_ms = 1801000;
			run.created_at = parent.created_at;
			run.started_at = parent.created_at;
			run.finished_at = "2026-10-21 10:00:01";
			run.updated_at = run.finished_at;
			run.result_delivery_id = run.id;
			run.deliver_result_to_root_chat = true;
			run.result_excerpt = "Original child result.\nRetain these exact bytes.";
			UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
			app.agent_runs.push_back(run);
		}

		ChatSession LoadParent() const
		{
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, run.root_chat_id, true, nullptr, false);
			UAM_ASSERT(saved.has_value());
			return *saved;
		}

		AgentRun LoadRun() const
		{
			const auto saved = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
			UAM_ASSERT(saved.errors.empty());
			UAM_ASSERT_EQ(saved.runs.size(), std::size_t{1});
			UAM_ASSERT_EQ(saved.runs.front().id, run.id);
			UAM_ASSERT_EQ(saved.runs.front().result_excerpt, run.result_excerpt);
			return saved.runs.front();
		}

		std::size_t ResultCount(const ChatSession& parent) const
		{
			std::size_t count = 0;
			for (const auto& message : parent.messages)
			{
				for (const auto& tool : message.tool_calls)
				{
					if (tool.name != "uam_agent_result") continue;
					const auto result = nlohmann::json::parse(tool.result_text);
					UAM_ASSERT_EQ(result.at("runId").get<std::string>(), run.id);
					UAM_ASSERT_EQ(result.at("result").get<std::string>(), run.result_excerpt);
					UAM_ASSERT_EQ(result.at("status").get<std::string>(), run.status);
					++count;
				}
			}
			return count;
		}

		void DiagnoseMissingResult() const
		{
			const auto& current = app.chats.front();
			auto saved = LoadParent();
			saved.persisted_message_count = current.persisted_message_count;
			saved.persisted_messages_digest = current.persisted_messages_digest;
			saved.acp_queue_full_save_revision = current.acp_queue_full_save_revision;
			const auto before = nlohmann::json::parse(ChatRepository::ChatMetadataFingerprint(app.data_root, saved));
			const auto after = nlohmann::json::parse(ChatRepository::ChatMetadataFingerprint(app.data_root, current));
			nlohmann::json paths = nlohmann::json::array();
			for (const auto& difference : nlohmann::json::diff(before, after))
				paths.push_back(difference.at("path"));
			std::cerr << "RESULT_DELIVERY_DIAGNOSTIC attempts=" << app.agent_runs.front().root_result_delivery_attempts
			    << " messages_equal=" << (saved.messages == current.messages)
			    << " metadata_changed_paths=" << paths.dump() << '\n';
		}
	};
}

UAM_TEST(AgentResultDeliveryAppendsOnceToCurrentLoadedParent)
{
	AgentResultDeliveryFixture fixture;
	const auto authored = fixture.app.chats.front().messages.front();
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200);
	const auto delivered = fixture.LoadParent();
	UAM_ASSERT_EQ(delivered.messages.front(), authored);
	if (fixture.ResultCount(delivered) == 0) fixture.DiagnoseMissingResult();
	UAM_ASSERT_EQ(fixture.ResultCount(delivered), std::size_t{1});
	UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
	UAM_ASSERT(fixture.LoadRun().root_result_delivered);

	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1300);
	UAM_ASSERT_EQ(fixture.LoadParent().messages, delivered.messages);
	UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
}

UAM_TEST(AgentResultDeliveryPreservesNewerSavedParentBeforeAcknowledging)
{
	AgentResultDeliveryFixture fixture;
	const auto stale_messages = fixture.app.chats.front().messages;
	ChatSession newer = fixture.app.chats.front();
	Message second;
	second.role = MessageRole::User;
	second.content = "Newer authored parent message B must survive.";
	second.created_at = "2026-10-21 10:00:02";
	newer.messages.push_back(second);
	newer.updated_at = second.created_at;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, newer));
	UAM_ASSERT_EQ(newer.managed_followup_incarnation, fixture.app.chats.front().managed_followup_incarnation);
	UAM_ASSERT_EQ(newer.metadata_incarnation, fixture.app.chats.front().metadata_incarnation);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages, stale_messages);

	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200);
	const auto after_stale_poll = fixture.LoadParent();
	UAM_ASSERT(after_stale_poll.messages.size() >= newer.messages.size());
	UAM_ASSERT_EQ(after_stale_poll.messages.at(0), newer.messages.at(0));
	UAM_ASSERT_EQ(after_stale_poll.messages.at(1), second);
	const auto result_count = fixture.ResultCount(after_stale_poll);
	UAM_ASSERT(result_count <= 1);
	// Reconciliation may publish safely or refuse; refusal must retain the result without a false ACK.
	if (result_count == 0)
	{
		UAM_ASSERT_EQ(after_stale_poll.messages, newer.messages);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, stale_messages);
		UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered);
		UAM_ASSERT(!fixture.LoadRun().root_result_delivered);
	}
	else
	{
		UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
		UAM_ASSERT(fixture.LoadRun().root_result_delivered);
	}
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().result_excerpt, fixture.run.result_excerpt);

	// Explicitly adopt authoritative history before retrying; this read grants no provider execution.
	const auto incarnation = fixture.app.chats.front().metadata_incarnation;
	fixture.app.chats.front() = fixture.LoadParent();
	fixture.app.chats.front().metadata_incarnation = incarnation;
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1300);
	const auto delivered = fixture.LoadParent();
	UAM_ASSERT_EQ(delivered.messages.at(0), newer.messages.at(0));
	UAM_ASSERT_EQ(delivered.messages.at(1), second);
	UAM_ASSERT_EQ(fixture.ResultCount(delivered), std::size_t{1});
	UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
	UAM_ASSERT(fixture.LoadRun().root_result_delivered);

	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1400);
	UAM_ASSERT_EQ(fixture.LoadParent().messages, delivered.messages);
	UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
}

UAM_TEST(AgentResultDeliverySurvivesPendingAuthoredParentSave)
{
	AgentResultDeliveryFixture fixture;
	Message second;
	second.role = MessageRole::User;
	second.content = "Pending authored parent message B must survive.";
	second.created_at = "2026-10-21 10:00:02";
	fixture.app.chats.front().messages.push_back(second);
	fixture.app.chats.front().updated_at = second.created_at;
	// A pending parent save may reconcile safely or defer, without exhausting delivery.
	for (int attempt = 0; attempt < 4; ++attempt)
		(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200 + attempt);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front()));
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1300);
	const auto delivered = fixture.LoadParent();
	UAM_ASSERT_EQ(delivered.messages.at(1), second);
	if (fixture.ResultCount(delivered) == 0) fixture.DiagnoseMissingResult();
	UAM_ASSERT_EQ(fixture.ResultCount(delivered), std::size_t{1});
	UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
	UAM_ASSERT(fixture.LoadRun().root_result_delivered);
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1400);
	UAM_ASSERT_EQ(fixture.LoadParent().messages, delivered.messages);
	UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
}

UAM_TEST(AgentResultDeliveryRepeatedStaleRefusalIsReadOnly)
{
	AgentResultDeliveryFixture fixture;
	const auto live_before = fixture.app.chats.front().messages;
	ChatSession newer = fixture.app.chats.front();
	ChatDomainService().AddMessage(newer, MessageRole::User, "Newer saved parent message B.");
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, newer));
	const auto saved_before = fixture.LoadParent().messages;
	const auto ledger = fixture.app.data_root / "agent-runs" / (fixture.run.id + ".json");
	const auto bytes = ReadFile(ledger);
	const auto attempts = fixture.app.agent_runs.front().root_result_delivery_attempts;
	const auto physical = uam::platform::PublishedFileChangeWitness::Capture(ledger);
	UAM_ASSERT(physical);
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		const bool changed = uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200 + attempt);
		// A retained physical object detects an equal-byte rewrite without clocks or sleeps.
		UAM_ASSERT(physical->Current());
		UAM_ASSERT(!changed);
		UAM_ASSERT_EQ(ReadFile(ledger), bytes);
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().root_result_delivery_attempts, attempts);
		UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered);
		UAM_ASSERT(!fixture.LoadRun().root_result_delivered);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, live_before);
		UAM_ASSERT_EQ(fixture.LoadParent().messages, saved_before);
		UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{0});
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		UAM_ASSERT(fixture.app.queued_agent_run_ids.empty());
	}
}

namespace
{
	/// One streamed chunk: the parent's unsaved tail grows and its updated_at moves, so the back-off never applies.
	void StreamChunk(ChatSession& parent, int chunk)
	{
		parent.messages.back().content += " chunk " + std::to_string(chunk);
		parent.updated_at = "2026-10-21 10:01:" + std::string(chunk < 10 ? "0" : "") + std::to_string(chunk % 60);
	}

	/// The state a fresh start rebuilds from disk: the saved parent and the saved run records, nothing else.
	void LoadSavedState(uam::AppState& app, const std::filesystem::path& data_root, const std::string& parent_id)
	{
		app.data_root = data_root;
		auto parent = ChatRepository::LoadLocalChat(data_root, parent_id, true, nullptr, false);
		UAM_ASSERT(parent.has_value());
		app.chats.push_back(std::move(*parent));
		auto saved = uam::AgentRunLedger::LoadAllReadOnly(data_root);
		UAM_ASSERT(saved.errors.empty());
		app.agent_runs = std::move(saved.runs);
	}
}

UAM_TEST(AgentResultDeliveryStreamingParentReusesOneSavedReadPerRevision)
{
	AgentResultDeliveryFixture fixture;
	AgentRun second = fixture.run;
	second.id = uam::AgentRunLedger::NewRunId();
	second.transcript_chat_id = "result-transcript-" + second.id;
	second.result_delivery_id = second.id;
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, second));
	fixture.app.agent_runs.push_back(second);
	ChatSession& parent = fixture.app.chats.front();
	const ChatSession live_before = parent;
	const auto saved_before = fixture.LoadParent();
	const auto retry = [&](const AgentRun& run) -> const auto& { return fixture.app.agent_result_retry_steady_ms.at(run.id); };
	const auto cards = [&](const ChatSession& chat, const std::string& run_id)
	{
		std::size_t count = 0;
		for (const auto& message : chat.messages)
			count += static_cast<std::size_t>(std::ranges::count(message.tool_calls, "uam-agent-result-" + run_id, &ToolCall::id));
		return count;
	};

	// The parent streams: memory stays ahead of its saved history, so both results wait without an attempt.
	ChatDomainService().AddMessage(parent, MessageRole::Assistant, "Streaming answer");
	std::shared_ptr<const ChatSession> read;
	for (int chunk = 1; chunk < 10; ++chunk)
	{
		StreamChunk(parent, chunk);
		(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200 + chunk);
		for (const AgentRun& run : fixture.app.agent_runs)
		{
			UAM_ASSERT(!run.root_result_delivered);
			UAM_ASSERT_EQ(run.root_result_delivery_attempts, 0);
		}
		if (chunk == 1) read = retry(fixture.run).saved_parent;
		// One read of the saved parent per storage revision, shared by both runs, for every streamed chunk.
		UAM_ASSERT(read && read->messages == saved_before.messages);
		UAM_ASSERT_EQ(retry(fixture.run).saved_parent, read);
		UAM_ASSERT_EQ(retry(second).saved_parent, read);
		UAM_ASSERT_EQ(retry(fixture.run).storage_revision, *ChatRepository::ChatStorageRevision(fixture.app.data_root, parent.id));
	}

	// A read older than the retry interval is read again even at the same revision, and shared again.
	StreamChunk(parent, 10);
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1201 + 2000);
	UAM_ASSERT(retry(fixture.run).saved_parent && retry(fixture.run).saved_parent != read);
	UAM_ASSERT_EQ(retry(second).saved_parent, retry(fixture.run).saved_parent);
	UAM_ASSERT_EQ(retry(fixture.run).saved_parent->messages, saved_before.messages);

	// Another writer saves newer history and memory falls back to exactly the earlier read. The earlier read must not
	// stand in for the new revision: delivering would overwrite the newer row.
	ChatSession foreign = live_before;
	ChatDomainService().AddMessage(foreign, MessageRole::User, "Newer saved row from another writer.");
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, foreign));
	parent = live_before;
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 3300);
	UAM_ASSERT(std::ranges::none_of(fixture.app.agent_runs, &AgentRun::root_result_delivered));
	UAM_ASSERT(retry(fixture.run).saved_parent != read);
	UAM_ASSERT_EQ(retry(fixture.run).saved_parent->messages, fixture.LoadParent().messages);
	UAM_ASSERT_EQ(fixture.LoadParent().messages.back().content, std::string("Newer saved row from another writer."));
	UAM_ASSERT_EQ(cards(fixture.LoadParent(), fixture.run.id) + cards(fixture.LoadParent(), second.id), std::size_t{0});

	// Adopting the saved history delivers each result exactly once and leaves no retry state behind.
	const auto incarnation = parent.metadata_incarnation;
	parent = fixture.LoadParent();
	parent.metadata_incarnation = incarnation;
	for (int poll = 0; poll < 3; ++poll)
		(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 3400 + poll);
	const auto delivered = fixture.LoadParent();
	UAM_ASSERT_EQ(delivered.messages.at(saved_before.messages.size()).content, std::string("Newer saved row from another writer."));
	UAM_ASSERT_EQ(cards(delivered, fixture.run.id), std::size_t{1});
	UAM_ASSERT_EQ(cards(delivered, second.id), std::size_t{1});
	UAM_ASSERT(std::ranges::all_of(fixture.app.agent_runs, &AgentRun::root_result_delivered));
	UAM_ASSERT(fixture.app.agent_result_retry_steady_ms.empty());
}

UAM_TEST(AgentResultDeliveryAfterRestartIsExactlyOnce)
{
	AgentResultDeliveryFixture fixture;
	const auto ledger = fixture.app.data_root / "agent-runs" / (fixture.run.id + ".json");
	const std::string undelivered_record = ReadFile(ledger);
	const std::size_t saved_rows = fixture.LoadParent().messages.size();

	// The app goes away while the parent streams; the waiting result and its saved read live only in memory.
	ChatDomainService().AddMessage(fixture.app.chats.front(), MessageRole::Assistant, "Streaming answer");
	StreamChunk(fixture.app.chats.front(), 1);
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200);
	UAM_ASSERT(fixture.app.agent_result_retry_steady_ms.contains(fixture.run.id));
	UAM_ASSERT(!fixture.LoadRun().root_result_delivered);
	UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{0});
	{
		uam::AppState restarted;
		LoadSavedState(restarted, fixture.app.data_root, fixture.run.root_chat_id);
		(void)uam::AgentRunScheduler::PollAtForTests(restarted, 1300);
		UAM_ASSERT(restarted.agent_runs.front().root_result_delivered);
		UAM_ASSERT(fixture.LoadRun().root_result_delivered);
		UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
		UAM_ASSERT_EQ(fixture.LoadParent().messages.size(), saved_rows + 1);
	}

	// A crash after the card was saved but before its receipt: the card is found, never written twice.
	UAM_ASSERT(uam::io::WriteTextFile(ledger, undelivered_record));
	UAM_ASSERT(!fixture.LoadRun().root_result_delivered);
	{
		uam::AppState crashed;
		LoadSavedState(crashed, fixture.app.data_root, fixture.run.root_chat_id);
		for (int poll = 0; poll < 3; ++poll)
			(void)uam::AgentRunScheduler::PollAtForTests(crashed, 1400 + poll);
		UAM_ASSERT(crashed.agent_runs.front().root_result_delivered);
		UAM_ASSERT(fixture.LoadRun().root_result_delivered);
		UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
		UAM_ASSERT_EQ(fixture.LoadParent().messages.size(), saved_rows + 1);
	}
}

// #948: while a large parent keeps streaming, a result waiting for it must not read and parse the parent's saved
// transcript on every poll. Per-poll budget per docs/engineering/testing.md: half the cheapest of three reads of the
// same saved parent on this machine, so a poll that still reads it fails. Listed in tests/ci_serial.txt.
UAM_TEST(AgentResultDeliveryStreamingLargeParentPollStaysBelowOneParentRead)
{
	using Clock = std::chrono::steady_clock;
	const auto milliseconds = [](Clock::duration elapsed) { return std::chrono::duration<double, std::milli>(elapsed).count(); };
	AgentResultDeliveryFixture fixture(2000);
	double read_ms = 0.0;
	for (int sample = 0; sample < 3; ++sample)
	{
		const auto started = Clock::now();
		(void)fixture.LoadParent();
		const double elapsed = milliseconds(Clock::now() - started);
		read_ms = sample == 0 ? elapsed : std::min(read_ms, elapsed);
	}
	ChatSession& parent = fixture.app.chats.front();
	ChatDomainService().AddMessage(parent, MessageRole::Assistant, "Streaming answer");
	std::vector<double> polls;
	for (int chunk = 1; chunk <= 30; ++chunk)
	{
		StreamChunk(parent, chunk);
		const auto started = Clock::now();
		(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200 + chunk);
		polls.push_back(milliseconds(Clock::now() - started));
		UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered);
	}
	const double first = polls.front();
	std::vector<double> streaming(polls.begin() + 1, polls.end());
	std::ranges::sort(streaming);
	const double median = streaming[streaming.size() / 2];
	std::cout << "[TIMING] streaming parent (2,001 rows) saved read: " << read_ms << " ms\n";
	std::cout << "[TIMING] streaming parent first scheduler poll: " << first << " ms\n";
	std::cout << "[TIMING] streaming parent scheduler poll median: " << median << " ms, max " << streaming.back() << " ms (limit " << read_ms / 2 << " ms)\n";
	UAM_ASSERT(median < read_ms / 2);

	// The parent's save lands: the card follows on the next poll, once.
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, parent));
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1300);
	UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1400);
	UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
}
