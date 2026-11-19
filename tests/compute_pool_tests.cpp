#include "test_harness.h"
#include "cef/state_serializer.h"
#include "common/config/compute_pools.h"
#include "common/config/settings_store.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/compute_pool_gate.h"

using namespace uam_test;

namespace
{
	constexpr const char* kModel = "qwen3.8";

	nlohmann::json AcpJson(const nlohmann::json& state, const std::string& chat_id)
	{
		for (const nlohmann::json& chat : state.at("chats"))
			if (chat.value("id", std::string{}) == chat_id) return chat.at("acpSession");
		return nlohmann::json::object();
	}

	struct PoolFixture
	{
		TempDir temp{"uam-compute-pools"};
		uam::AppState app;

		PoolFixture()
		{
			app.data_root = temp.root;
			app.chats.reserve(16);
			app.settings.compute_pools = {{"pool", "InferDeck", 2, {{uam::provider_ids::kGeminiCli, kModel}}}};
		}

		~PoolFixture()
		{
			for (const std::unique_ptr<uam::AcpSessionState>& session : app.acp_sessions)
			{
				if (!session->running) continue;
				PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
				PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*session);
			}
		}

		uam::AcpSessionState& AddTurn(const std::string& id, const std::string& model = kModel)
		{
			ChatSession chat;
			chat.id = id;
			chat.provider_id = uam::provider_ids::kGeminiCli;
			chat.native_session_id = "native-" + id;
			chat.model_id = model;
			chat.messages.push_back({.role = MessageRole::User, .content = "Hello", .acp_prompt_not_sent = true});
			app.chats.push_back(chat);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = id;
			session->provider_id = chat.provider_id;
			session->session_id = chat.native_session_id;
			session->running = true;
			session->session_ready = true;
			session->processing = true;
			session->queued_prompt = "Hello";
			session->current_model_id = model;
			session->turn_serial = 1;
			session->turn_first_user_message_index = 0;
			session->turn_user_message_index = 0;
			uam::AcpSessionState* raw = session.get();
			app.acp_sessions.push_back(std::move(session));
#if defined(_WIN32)
			const std::vector<std::string> sink = {"cmd", "/C", "more > NUL"};
#else
			const std::vector<std::string> sink = {"/bin/sh", "-c", "cat > /dev/null"};
#endif
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw, temp.root, sink, &error));
			return *raw;
		}

		ChatSession& Chat(const std::string& id) { return *ChatDomainService().FindChatById(app, id); }
		void Tick(uam::AcpSessionState& session) { (void)uam::acp_detail::SendQueuedPromptIfReady(app, session, Chat(session.chat_id)); }
		static bool Delivered(const uam::AcpSessionState& session) { return session.prompt_request_id != 0 && session.queued_prompt.empty(); }
	};
}

UAM_TEST(ComputePoolHoldsThirdTurnWithVisibleStatusAndStartsItWhenASlotFrees)
{
	PoolFixture fixture;
	uam::AcpSessionState& first = fixture.AddTurn("one");
	uam::AcpSessionState& second = fixture.AddTurn("two");
	uam::AcpSessionState& third = fixture.AddTurn("three");
	fixture.Tick(first);
	fixture.Tick(second);
	fixture.Tick(third);
	UAM_ASSERT(PoolFixture::Delivered(first));
	UAM_ASSERT(PoolFixture::Delivered(second));
	UAM_ASSERT(!PoolFixture::Delivered(third));
	UAM_ASSERT(third.processing);
	UAM_ASSERT_EQ(third.queued_prompt, std::string("Hello"));
	const nlohmann::json state = uam::StateSerializer::Serialize(fixture.app);
	const nlohmann::json held = AcpJson(state, "three");
	UAM_ASSERT_EQ(held.at("computePoolWait").at("poolName").get<std::string>(), std::string("InferDeck"));
	UAM_ASSERT_EQ(held.at("computePoolWait").at("inUse").get<int>(), 2);
	UAM_ASSERT_EQ(held.at("computePoolWait").at("limit").get<int>(), 2);
	UAM_ASSERT_EQ(state.at("computePoolUsage").at("pool").at("inUse").get<int>(), 2);
	UAM_ASSERT_EQ(state.at("computePoolUsage").at("pool").at("waiting").get<int>(), 1);

	uam::acp_detail::CompletePromptTurn(first, "ready");
	(void)uam::PollAllAcpSessions(fixture.app); // The ordinary runtime poll starts the held turn; nothing else re-sends it.
	UAM_ASSERT(PoolFixture::Delivered(third));
	UAM_ASSERT(!AcpJson(uam::StateSerializer::Serialize(fixture.app), "three").contains("computePoolWait"));
}

UAM_TEST(ComputePoolReleasesOnCompleteFailStopDeathAndDeletion)
{
	for (const std::string ending : {"complete", "fail", "cancel", "death", "delete"})
	{
		PoolFixture fixture;
		fixture.app.settings.compute_pools[0].limit = 1;
		uam::AcpSessionState& running = fixture.AddTurn("running");
		uam::AcpSessionState& waiting = fixture.AddTurn("waiting");
		fixture.Tick(running);
		fixture.Tick(waiting);
		UAM_ASSERT(PoolFixture::Delivered(running));
		UAM_ASSERT(!PoolFixture::Delivered(waiting));
		if (ending == "complete") uam::acp_detail::CompletePromptTurn(running, "ready");
		else if (ending == "fail") uam::acp_detail::CompletePromptTurn(running, "error");
		else if (ending == "cancel")
		{
			std::string error;
			UAM_ASSERT(uam::CancelAcpTurn(fixture.app, "running", &error, false, false, false));
			uam::acp_detail::CompletePromptTurn(running, "ready");
		}
		else if (ending == "death")
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(running, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(running);
			running.running = false;
		}
		else
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(running, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(running);
			std::erase_if(fixture.app.acp_sessions, [&](const std::unique_ptr<uam::AcpSessionState>& session) { return session.get() == &running; });
			std::erase_if(fixture.app.chats, [](const ChatSession& chat) { return chat.id == "running"; });
		}
		fixture.Tick(waiting);
		UAM_ASSERT(PoolFixture::Delivered(waiting));
	}
}

UAM_TEST(ComputePoolStoppingAHeldTurnRemovesItFromTheLine)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& stopped = fixture.AddTurn("stopped");
	uam::AcpSessionState& behind = fixture.AddTurn("behind");
	fixture.Tick(running);
	fixture.Tick(stopped);
	fixture.Tick(behind);
	std::string error;
	UAM_ASSERT(uam::CancelAcpTurn(fixture.app, "stopped", &error, false, false, false));
	UAM_ASSERT(!stopped.processing);
	UAM_ASSERT(stopped.queued_prompt.empty());
	UAM_ASSERT_EQ(stopped.prompt_request_id, 0);
	// Nothing reached the provider, so no cancel is outstanding and the 5-second kill cannot fire on this session.
	UAM_ASSERT(!stopped.cancel_requested);
	UAM_ASSERT_EQ(stopped.cancel_request_id, 0);
	UAM_ASSERT(stopped.running);
	uam::acp_detail::CompletePromptTurn(running, "ready");
	fixture.Tick(behind);
	UAM_ASSERT(PoolFixture::Delivered(behind));
}

UAM_TEST(ComputePoolAdmitsWaitersInArrivalOrderRegardlessOfPollOrder)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& earlier = fixture.AddTurn("earlier");
	uam::AcpSessionState& later = fixture.AddTurn("later");
	fixture.Tick(running);
	fixture.Tick(earlier);
	fixture.Tick(later);
	uam::acp_detail::CompletePromptTurn(running, "ready");
	fixture.Tick(later);
	UAM_ASSERT(!PoolFixture::Delivered(later));
	fixture.Tick(earlier);
	UAM_ASSERT(PoolFixture::Delivered(earlier));
	fixture.Tick(later);
	UAM_ASSERT(!PoolFixture::Delivered(later));
}

UAM_TEST(ComputePoolWaiterThatStopsPollingDoesNotBlockTheLine)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& abandoned = fixture.AddTurn("abandoned");
	uam::AcpSessionState& later = fixture.AddTurn("later");
	fixture.Tick(running);
	fixture.Tick(abandoned);
	fixture.Tick(later);
	uam::acp_detail::CompletePromptTurn(running, "ready");
	abandoned.compute_pool_ticket_seen_s = uam::GetAppTimeSeconds() - 60.0;
	fixture.Tick(later);
	UAM_ASSERT(PoolFixture::Delivered(later));
}

UAM_TEST(ComputePoolTurnOutsideEveryPoolAndAppWithoutPoolsAreUnchanged)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& other_model = fixture.AddTurn("other", "other-model");
	fixture.Tick(running);
	fixture.Tick(other_model);
	UAM_ASSERT(PoolFixture::Delivered(other_model));

	PoolFixture off;
	off.app.settings.compute_pools.clear();
	uam::AcpSessionState& one = off.AddTurn("one");
	uam::AcpSessionState& two = off.AddTurn("two");
	off.Tick(one);
	off.Tick(two);
	UAM_ASSERT(PoolFixture::Delivered(one));
	UAM_ASSERT(PoolFixture::Delivered(two));
	UAM_ASSERT_EQ(two.compute_pool_ticket, std::uint64_t(0));
	UAM_ASSERT_EQ(two.compute_pool_admitted_turn_serial, -1);
}

UAM_TEST(ComputePoolRunsOnlyWhenEveryMatchingPoolHasRoom)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools = {{"wide", "Provider", 5, {{uam::provider_ids::kGeminiCli, ""}}}, {"narrow", "Model", 1, {{uam::provider_ids::kGeminiCli, kModel}}}};
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& held = fixture.AddTurn("held");
	fixture.Tick(running);
	fixture.Tick(held);
	UAM_ASSERT(!PoolFixture::Delivered(held));
	UAM_ASSERT_EQ(held.compute_pool_wait_name, std::string("Model"));
}

UAM_TEST(ComputePoolParentInAgentWaitLeavesRoomForItsChild)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& parent = fixture.AddTurn("parent");
	uam::AcpSessionState& child = fixture.AddTurn("child");
	fixture.Tick(parent);
	fixture.Tick(child);
	UAM_ASSERT(!PoolFixture::Delivered(child));
	uam::UamControlCapability capability;
	capability.session_chat_id = "parent";
	// The gate only tests for a pending wait, so an opaque non-null handle stands in for the service-private type.
	static int wait_handle = 0;
	capability.pending_agent_wait = std::shared_ptr<uam::UamAgentWait>(std::make_shared<int>(0), reinterpret_cast<uam::UamAgentWait*>(&wait_handle));
	fixture.app.uam_control_capabilities.push_back(std::move(capability));
	fixture.Tick(child);
	UAM_ASSERT(PoolFixture::Delivered(child));
}

UAM_TEST(ComputePoolSteerOnHeldTurnIsRefusedAndKeepsThePrompt)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& held = fixture.AddTurn("held");
	fixture.Tick(running);
	fixture.Tick(held);
	std::string error;
	UAM_ASSERT(!uam::SteerAcpPrompt(fixture.app, "held", "Actually do this", {}, {}, false, &error));
	UAM_ASSERT_EQ(error, std::string("Wait for a compute pool slot before steering."));
	UAM_ASSERT_EQ(held.queued_prompt, std::string("Hello"));
	UAM_ASSERT(held.queued_user_prompts.empty());
	UAM_ASSERT(held.processing);
}

UAM_TEST(ComputePoolSettingsRoundTripAndDropInvalidPools)
{
	std::vector<ComputePool> pools = {
	    {" a ", " InferDeck ", 2, {{"gemini-cli", " qwen "}, {"gemini-cli", "qwen"}, {"codex-cli", ""}}},
	    {"a", "Repeated id", 1, {{"gemini-cli", ""}}},
	    {"b", "", 1, {{"gemini-cli", ""}}},
	    {"c", "No members", 1, {}},
	    {"d", "Zero limit", 0, {{"gemini-cli", ""}}}};
	uam::compute_pools::Normalize(pools);
	UAM_ASSERT_EQ(pools.size(), std::size_t(1));
	UAM_ASSERT_EQ(pools[0].id, std::string("a"));
	UAM_ASSERT_EQ(pools[0].name, std::string("InferDeck"));
	UAM_ASSERT_EQ(pools[0].members.size(), std::size_t(2));

	TempDir temp("uam-compute-pool-settings");
	AppSettings saved;
	saved.compute_pools = pools;
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", saved));
	AppSettings loaded;
	(void)SettingsStore::Load(temp.root / "settings.txt", loaded);
	UAM_ASSERT_EQ(uam::compute_pools::Serialize(loaded.compute_pools).dump(), uam::compute_pools::Serialize(pools).dump());
}

UAM_TEST(ComputePoolHeldTimeDoesNotCountAgainstTurnOrRunLimits)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& held = fixture.AddTurn("held");
	held.managed_agent_run_id = "run-held";
	fixture.Tick(running);
	fixture.Tick(held);
	const double long_ago = uam::GetAppTimeSeconds() - 3600.0;
	held.turn_started_time_s = held.last_runtime_activity_time_s = long_ago;
	fixture.app.agent_run_deadline_steady_ms["run-held"] = 1000;
	held.compute_pool_ticket_seen_s = uam::GetAppTimeSeconds() - 2.0;
	fixture.Tick(held);
	UAM_ASSERT(fixture.app.agent_run_deadline_steady_ms["run-held"] >= 2900);
	UAM_ASSERT_EQ(held.turn_started_time_s, long_ago);
	uam::acp_detail::CompletePromptTurn(running, "ready");
	fixture.Tick(held);
	UAM_ASSERT(PoolFixture::Delivered(held));
	UAM_ASSERT(held.turn_started_time_s > long_ago + 3000.0);
	UAM_ASSERT(held.last_runtime_activity_time_s > long_ago + 3000.0);
}

UAM_TEST(ComputePoolDefaultModelChatMatchesTheModelItsProviderReports)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& default_model = fixture.AddTurn("default", "");
	default_model.current_model_id = kModel;
	fixture.Tick(running);
	fixture.Tick(default_model);
	UAM_ASSERT(!PoolFixture::Delivered(default_model));
}

UAM_TEST(ComputePoolOverlappingPoolsKeepStrictArrivalOrder)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools = {
	    {"model", "Model", 1, {{uam::provider_ids::kGeminiCli, kModel}}},
	    {"provider", "Provider", 2, {{uam::provider_ids::kGeminiCli, ""}}}};
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& oldest = fixture.AddTurn("oldest");
	uam::AcpSessionState& newer = fixture.AddTurn("newer", "other-model");
	fixture.Tick(running);
	fixture.Tick(oldest);
	UAM_ASSERT(!PoolFixture::Delivered(oldest));
	fixture.Tick(newer);
	UAM_ASSERT(!PoolFixture::Delivered(newer));
	UAM_ASSERT_EQ(newer.compute_pool_wait_name, std::string("Provider"));
	UAM_ASSERT_EQ(newer.compute_pool_wait_in_use, 2);
	uam::acp_detail::CompletePromptTurn(running, "ready");
	fixture.Tick(oldest);
	UAM_ASSERT(PoolFixture::Delivered(oldest));
}

UAM_TEST(ComputePoolRuntimeResetForgetsAnEarlierAdmission)
{
	PoolFixture fixture;
	uam::AcpSessionState& session = fixture.AddTurn("reset");
	fixture.Tick(session);
	UAM_ASSERT_EQ(session.compute_pool_admitted_turn_serial, 1);
	uam::acp_detail::ResetAcpRuntimeState(fixture.app, session, fixture.Chat("reset"));
	UAM_ASSERT_EQ(session.compute_pool_admitted_turn_serial, -1);
	UAM_ASSERT_EQ(session.compute_pool_ticket, std::uint64_t(0));
}

UAM_TEST(ComputePoolRemovedWhileHeldStartsTheTurnWithFreshInactivityClocks)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& held = fixture.AddTurn("held");
	fixture.Tick(running);
	fixture.Tick(held);
	const double long_ago = uam::GetAppTimeSeconds() - 3600.0;
	held.turn_started_time_s = held.last_runtime_activity_time_s = long_ago;
	fixture.app.settings.compute_pools.clear();
	fixture.Tick(held);
	UAM_ASSERT(PoolFixture::Delivered(held));
	UAM_ASSERT(held.last_runtime_activity_time_s > long_ago + 3000.0);
	UAM_ASSERT(held.compute_pool_wait_name.empty());
}

UAM_TEST(ComputePoolNewHoldIsReportedEvenWithAnIdenticalEarlierStatus)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& held = fixture.AddTurn("held");
	fixture.Tick(running);
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(fixture.app, held, fixture.Chat("held")));
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(fixture.app, held, fixture.Chat("held")));
	// A later turn of the same chat inherits the old status text but has not been pushed yet.
	held.turn_serial += 1;
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(fixture.app, held, fixture.Chat("held")));
}

UAM_TEST(ComputePoolRemoteTurnBeingReconnectedKeepsItsSlot)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& remote = fixture.AddTurn("remote");
	uam::AcpSessionState& waiting = fixture.AddTurn("waiting");
	fixture.Tick(remote);
	fixture.Tick(waiting);
	remote.running = false;
	remote.recovering_remote_turn = true;
	fixture.Tick(waiting);
	UAM_ASSERT(!PoolFixture::Delivered(waiting));
	// Once the user has stopped it, an unreachable host no longer holds the slot.
	remote.remote_stop_unconfirmed = true;
	fixture.Tick(waiting);
	UAM_ASSERT(PoolFixture::Delivered(waiting));
	remote.running = true; // The fixture owns a real sink process to stop.
	remote.recovering_remote_turn = false;
	remote.remote_stop_unconfirmed = false;
}

UAM_TEST(ComputePoolHeldOpenCodeTurnIsNotTreatedAsUnconfirmedInput)
{
	PoolFixture fixture;
	fixture.app.settings.compute_pools[0].limit = 1;
	uam::AcpSessionState& running = fixture.AddTurn("running");
	uam::AcpSessionState& held = fixture.AddTurn("held");
	fixture.Tick(running);
	fixture.Tick(held);
	UAM_ASSERT(uam::compute_pools::HeldBeforeDelivery(held));
	held.opencode_v2_native = true;
	UAM_ASSERT(!ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kOpenCodeCli).OnAcpHoldUnconfirmedInput(fixture.app, held, fixture.Chat("held"), "transport lost"));
	UAM_ASSERT(!held.queue_held);
	uam::acp_detail::CompletePromptTurn(running, "ready");
	fixture.Tick(held);
	UAM_ASSERT(!uam::compute_pools::HeldBeforeDelivery(held));
}
