#include "test_harness.h"
#include "injected_acp_input.h"
#include "common/config/compute_pools.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/compute_pool_gate.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

using namespace uam_test;

// Manual compaction (maintenance on an idle session) and native mid-turn steering never wait for a compute pool
// slot but count towards use. A turn held at the gate across an app restart is not restored as a wait: its message
// stays in the chat for Retry and is checked at the gate again as a new turn.
namespace
{
	constexpr const char* kModel = "qwen3.8";

	/// One slot shared by a Gemini model and every Codex model.
	std::vector<ComputePool> OneSlotPool()
	{
		return {{"pool", "InferDeck", 1, {{uam::provider_ids::kGeminiCli, kModel}, {uam::provider_ids::kCodexCli, ""}}}};
	}

	int InUse(const uam::AppState& app)
	{
		return uam::compute_pools::UsageForFrontend(app).at("pool").at("inUse").get<int>();
	}

	int Waiting(const uam::AppState& app)
	{
		return uam::compute_pools::UsageForFrontend(app).at("pool").at("waiting").get<int>();
	}

	std::size_t Occurrences(const std::string& haystack, const std::string& needle)
	{
		std::size_t count = 0;
		for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) ++count;
		return count;
	}

	bool NotWaiting(const uam::AcpSessionState& session)
	{
		return session.compute_pool_ticket == 0 && session.compute_pool_wait_name.empty() &&
		    !uam::compute_pools::IsWaiting(session, uam::GetAppTimeSeconds());
	}

	void AwaitSave(uam::AppState& app)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	}

	std::string Written(uam::AcpSessionState& session, const InjectedAcpInput& input)
	{
		UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
		return input.Bytes();
	}

	struct PoolContinuationFixture
	{
		TempDir temp{"uam-compute-pool-continuation"};
		uam::AppState app;

		PoolContinuationFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.chats.reserve(16);
			app.settings.compute_pools = OneSlotPool();
		}

		ChatSession& Chat(const std::string& id) { return *ChatDomainService().FindChatById(app, id); }

		ChatSession& AddChat(const std::string& id, const std::string& provider_id, const std::string& model_id)
		{
			ChatSession chat;
			chat.id = id;
			chat.provider_id = provider_id;
			chat.model_id = model_id;
			chat.native_session_id = "native-" + id;
			chat.workspace_directory = temp.root.string();
			chat.approval_mode.clear();
			chat.memory_enabled = false;
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.back()));
			return app.chats.back();
		}

		/// A ready provider session whose input is captured instead of reaching a process.
		uam::AcpSessionState& AddSession(const ChatSession& chat, InjectedAcpInput& input)
		{
			uam::AcpSessionState& session = uam::acp_detail::EnsureAcpSessionForChat(app, chat);
			session.provider_id = chat.provider_id;
			session.session_id = chat.native_session_id;
			session.current_model_id = chat.model_id;
			session.current_mode_id = "default";
			input.Attach(session);
			return session;
		}

		/// A turn already admitted and running at its provider: it holds one slot.
		uam::AcpSessionState& Occupy(const std::string& id)
		{
			(void)AddChat(id, uam::provider_ids::kGeminiCli, kModel);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = id;
			session->provider_id = uam::provider_ids::kGeminiCli;
			session->current_model_id = kModel;
			session->running = session->processing = true;
			session->turn_serial = 1;
			session->prompt_request_id = 1;
			session->compute_pool_admitted_turn_serial = 1;
			app.acp_sessions.push_back(std::move(session));
			return *app.acp_sessions.back();
		}

		/// A new turn that reaches the gate while the pool is full and waits there.
		uam::AcpSessionState& HoldTurn(const std::string& id, InjectedAcpInput& input)
		{
			ChatSession& chat = AddChat(id, uam::provider_ids::kGeminiCli, kModel);
			chat.messages.push_back({.role = MessageRole::User, .content = "Hello", .acp_prompt_not_sent = true});
			uam::AcpSessionState& session = AddSession(chat, input);
			session.processing = true;
			session.queued_prompt = "Hello";
			session.turn_serial = 1;
			session.turn_first_user_message_index = session.turn_user_message_index = 0;
			TickUntilGateDecides(session);
			UAM_ASSERT(uam::compute_pools::IsWaiting(session, uam::GetAppTimeSeconds()));
			UAM_ASSERT_EQ(session.prompt_request_id, 0);
			return session;
		}

		void Tick(uam::AcpSessionState& session) { (void)uam::acp_detail::SendQueuedPromptIfReady(app, session, Chat(session.chat_id)); }

		/// Ticks, flushing pending saves, until the turn waits at the gate or has been sent.
		void TickUntilGateDecides(uam::AcpSessionState& session)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do
			{
				uam::FlushPendingChatSaves(app);
				Tick(session);
				if (uam::compute_pools::IsWaiting(session, uam::GetAppTimeSeconds()) || session.prompt_request_id != 0) return;
				std::this_thread::yield();
			} while (std::chrono::steady_clock::now() < deadline);
		}
	};
}

#if UAM_ENABLE_RUNTIME_CODEX_CLI
UAM_TEST(ComputePoolManualCompactionStartsOnAFullPoolAndCountsTowardUse)
{
	PoolContinuationFixture fixture;
	uam::AcpSessionState& busy = fixture.Occupy("busy");
	InjectedAcpInput waiting_input;
	uam::AcpSessionState& waiting = fixture.HoldTurn("waiting", waiting_input);
	UAM_ASSERT_EQ(InUse(fixture.app), 1);
	UAM_ASSERT_EQ(Waiting(fixture.app), 1);

	ChatSession& chat = fixture.AddChat("compact", uam::provider_ids::kCodexCli, "");
	InjectedAcpInput input;
	uam::AcpSessionState& session = fixture.AddSession(chat, input);
	session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
	session.codex_thread_id = session.session_id;
	std::string error;
	// The pool is full and an older turn is waiting, yet compaction is written at once and takes no place in line.
	UAM_ASSERT(uam::CompactAcpSession(fixture.app, chat.id, &error));
	UAM_ASSERT(session.manual_compaction_pending && session.processing);
	UAM_ASSERT(Written(session, input).find("thread/compact/start") != std::string::npos);
	UAM_ASSERT(NotWaiting(session));
	UAM_ASSERT_EQ(InUse(fixture.app), 2); // Over the limit of 1: it counts, it does not wait.
	UAM_ASSERT_EQ(Waiting(fixture.app), 1);

	// The running compaction holds its slot: the waiter stays held after the other turn ends, and starts after compaction.
	uam::acp_detail::CompletePromptTurn(busy, uam::acp_detail::kAcpLifecycleReady);
	fixture.Tick(waiting);
	UAM_ASSERT(uam::compute_pools::IsWaiting(waiting, uam::GetAppTimeSeconds()));
	UAM_ASSERT_EQ(waiting.prompt_request_id, 0);
	uam::acp_detail::CompletePromptTurn(session, uam::acp_detail::kAcpLifecycleReady);
	fixture.Tick(waiting);
	UAM_ASSERT(!uam::compute_pools::IsWaiting(waiting, uam::GetAppTimeSeconds()));
	UAM_ASSERT(waiting.compute_pool_wait_name.empty());
}

UAM_TEST(ComputePoolNativeSteerOnAFullPoolIsWrittenAtOnceAndKeepsItsSlot)
{
	PoolContinuationFixture fixture;
	ChatSession& chat = fixture.AddChat("steer", uam::provider_ids::kCodexCli, "");
	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-524524524524";
	chat.messages.push_back({.role = MessageRole::User, .content = "Start the task"});
	chat.messages.push_back({.role = MessageRole::Assistant, .content = "Working on it"});
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	InjectedAcpInput input;
	uam::AcpSessionState& session = fixture.AddSession(chat, input);
	session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
	session.codex_thread_id = session.session_id;
	session.codex_turn_id = "active-turn";
	session.processing = true;
	session.prompt_request_id = 17;
	session.turn_serial = 7;
	session.turn_user_message_index = session.turn_first_user_message_index = 0;
	session.current_assistant_message_index = session.turn_assistant_message_index = 1;
	session.compute_pool_admitted_turn_serial = 7; // Admitted by the gate when it started.

	// The steered turn fills the one slot, so a newer turn waits behind it.
	InjectedAcpInput waiting_input;
	uam::AcpSessionState& waiting = fixture.HoldTurn("waiting", waiting_input);
	UAM_ASSERT_EQ(InUse(fixture.app), 1);
	UAM_ASSERT_EQ(Waiting(fixture.app), 1);

	std::string error;
	UAM_ASSERT(uam::SteerAcpPrompt(fixture.app, chat.id, "Also cover the tests", {}, {}, false, &error));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::yield();
	}
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);

	// The steer joins the running turn natively: same turn, same slot, no ticket and no wait.
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.pending_request_methods.begin()->second, std::string("turn/steer"));
	UAM_ASSERT(session.processing && session.prompt_request_id == 17 && session.turn_serial == 7);
	const std::string written = Written(session, input);
	UAM_ASSERT(written.find("turn/steer") != std::string::npos);
	UAM_ASSERT(written.find("Also cover the tests") != std::string::npos);
	UAM_ASSERT(NotWaiting(session));
	// Re-check the waiter first: the save loop above may outlast the gate's stale-waiter window.
	fixture.Tick(waiting);
	UAM_ASSERT_EQ(InUse(fixture.app), 1);
	UAM_ASSERT_EQ(Waiting(fixture.app), 1);
	UAM_ASSERT(uam::compute_pools::IsWaiting(waiting, uam::GetAppTimeSeconds()));
	UAM_ASSERT_EQ(waiting.prompt_request_id, 0);
}
#endif

UAM_TEST(ComputePoolTurnHeldAtRestartStaysInItsChatAndIsCheckedAgainOnRetry)
{
	const std::string text = "Held across a restart";
	const auto user_rows = [&text](const ChatSession& chat)
	{
		return static_cast<int>(std::ranges::count_if(chat.messages, [&](const Message& message)
		{
			return message.role == MessageRole::User && message.content == text;
		}));
	};
	PoolContinuationFixture before;
	(void)before.Occupy("busy");
	ChatSession& chat = before.AddChat("held", uam::provider_ids::kGeminiCli, kModel);
	const std::string chat_id = chat.id;
	InjectedAcpInput before_input;
	uam::AcpSessionState& held = before.AddSession(chat, before_input);
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(before.app, chat_id, text, &error));
	AwaitSave(before.app);
	before.TickUntilGateDecides(held);
	UAM_ASSERT(uam::compute_pools::IsWaiting(held, uam::GetAppTimeSeconds()));
	UAM_ASSERT_EQ(held.compute_pool_wait_name, std::string("InferDeck"));
	UAM_ASSERT_EQ(held.prompt_request_id, 0);
	AwaitSave(before.app);
	UAM_ASSERT_EQ(Occurrences(Written(held, before_input), text), std::size_t{0});

	// The app quits the way it always does, while the turn is still held.
	uam::FastStopAcpSessionsForExit(before.app);
	uam::FlushPendingChatSaves(before.app, true);
	UAM_ASSERT_EQ(Occurrences(before_input.Bytes(), text), std::size_t{0});
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(before.app.data_root, chat_id);
	UAM_ASSERT(saved.has_value() && !saved->messages.empty());
	UAM_ASSERT_EQ(saved->messages.back().content, text);
	UAM_ASSERT(saved->messages.back().acp_prompt_not_sent);

	// The app starts again with only what was saved, restores sessions and polls as usual.
	PoolContinuationFixture after;
	after.app.data_root = before.app.data_root;
	after.app.chats.push_back(*saved);
	(void)uam::RestoreRemoteAcpSessionsAfterRestart(after.app);
	for (int poll = 0; poll < 5; ++poll)
	{
		(void)uam::PollAllAcpSessions(after.app);
		uam::FlushPendingChatSaves(after.app);
	}
	ChatSession& reloaded = after.Chat(chat_id);
	// The wait is not restored: nothing sends the row by itself and nothing waits at the pool.
	const uam::AcpSessionState* restored = uam::FindAcpSessionForChat(std::as_const(after.app), chat_id);
	UAM_ASSERT(restored == nullptr || (!restored->processing && restored->queued_prompt.empty() && restored->queued_user_prompts.empty() &&
	    !restored->reconnect_pending && restored->compute_pool_ticket == 0 && restored->prompt_request_id == 0));
	UAM_ASSERT_EQ(Waiting(after.app), 0);
	UAM_ASSERT(uam::ChatHasUnsentAcpPrompt(after.app, reloaded));
	UAM_ASSERT_EQ(reloaded.messages.back().content, text);
	UAM_ASSERT(reloaded.messages.back().acp_prompt_not_sent);
	UAM_ASSERT_EQ(user_rows(reloaded), 1);

	// Retry with no provider running yet stages the same row for the provider's relaunch; it adds no new row.
	uam::AcpSessionState& busy = after.Occupy("busy-after-restart");
	const int user_index = static_cast<int>(reloaded.messages.size()) - 1;
	UAM_ASSERT(uam::RetryFailedAcpMessage(after.app, chat_id, user_index, &error));
	uam::AcpSessionState* relaunched = uam::FindAcpSessionForChat(after.app, chat_id);
	UAM_ASSERT(relaunched != nullptr);
	UAM_ASSERT(relaunched->reconnect_pending);
	UAM_ASSERT_EQ(relaunched->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(relaunched->queued_user_prompts.front().existing_user_message_index, user_index);
	UAM_ASSERT_EQ(user_rows(reloaded), 1);

	// The relaunch finishes session setup (its process is stood in for here), which starts the staged row as a new turn.
	InjectedAcpInput after_input;
	relaunched->provider_id = reloaded.provider_id;
	relaunched->session_id = reloaded.native_session_id;
	relaunched->current_model_id = kModel;
	relaunched->current_mode_id = "default";
	after_input.Attach(*relaunched);
	relaunched->reconnect_pending = false;
	relaunched->lifecycle_state = uam::acp_detail::kAcpLifecycleReady;
	const auto settle = [&](const std::function<bool()>& done)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(after.app);
			(void)uam::acp_detail::ResumeQueuedUserPromptsAfterSessionSetup(after.app, *relaunched, reloaded);
			std::this_thread::yield();
		}
	};
	// It is checked at the gate again and waits while the pool is full.
	settle([&] { return uam::compute_pools::IsWaiting(*relaunched, uam::GetAppTimeSeconds()) || relaunched->prompt_request_id != 0; });
	UAM_ASSERT(uam::compute_pools::IsWaiting(*relaunched, uam::GetAppTimeSeconds()));
	UAM_ASSERT_EQ(relaunched->compute_pool_wait_name, std::string("InferDeck"));
	UAM_ASSERT_EQ(relaunched->prompt_request_id, 0);
	UAM_ASSERT_EQ(Waiting(after.app), 1);

	// When the slot frees, the message is sent once, as the same row.
	uam::acp_detail::CompletePromptTurn(busy, uam::acp_detail::kAcpLifecycleReady);
	settle([&] { return relaunched->prompt_request_id != 0; });
	UAM_ASSERT(relaunched->prompt_request_id != 0);
	UAM_ASSERT(NotWaiting(*relaunched));
	UAM_ASSERT_EQ(Occurrences(Written(*relaunched, after_input), text), std::size_t{1});
	UAM_ASSERT_EQ(Occurrences(before_input.Bytes(), text), std::size_t{0});
	UAM_ASSERT_EQ(user_rows(after.Chat(chat_id)), 1);
}
