#include "test_harness.h"
#include "cef_handler_fixture.h"
#include "cef/state_serializer.h"
#include "common/provider/provider_runtime.h"
#include "common/platform/async_byte_writer.h"
#include <cstdio>
#if defined(_WIN32)
#include <io.h>
#endif

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_COPILOT_CLI

struct NativeModelOptionsHandlerTestAccess
{
	static void Invoke(UamQueryHandler& handler, const nlohmann::json& payload,
	    CefRefPtr<UamQueryHandler::Callback> cb, bool refresh = false)
	{
		if (refresh) handler.HandleRefreshNativeModelOptions(nullptr, payload, cb);
		else handler.HandleSetNativeModelOption(nullptr, payload, cb);
	}
};

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
namespace
{
	class NativeOptionCallback final : public UamQueryHandler::Callback
	{
	public:
		int successes = 0, failures = 0, status = 0;
		nlohmann::json body;
		void Success(const CefString& value) override { ++successes; body = nlohmann::json::parse(value.ToString()); }
		void Success(const void*, size_t) override { ++successes; }
		void Failure(int code, const CefString&) override { ++failures; status = code; }
	private:
		IMPLEMENT_REFCOUNTING(NativeOptionCallback);
	};

	struct NativeOptionFixture
	{
		TempDir temp{"uam-native-model-option-handler"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		std::unique_ptr<UamQueryHandler> handler;
		std::FILE* file = std::tmpfile();
		std::shared_ptr<std::string> bytes = std::make_shared<std::string>();
		NativeOptionFixture()
		{
			UAM_ASSERT(file != nullptr);
			app.data_root = temp.root;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat;
			chat.id = "owned-native-model-options";
			chat.created_at = "2026-10-17T00:00:00Z";
			chat.provider_id = "copilot-cli";
			chat.native_session_id = "550e8400-e29b-41d4-a716-446655440000";
			chat.model_id = "auto";
			app.chats.push_back(chat);
			auto owned = std::make_unique<uam::AcpSessionState>();
			owned->chat_id = chat.id; owned->provider_id = chat.provider_id;
			owned->session_id = chat.native_session_id; owned->process_execution_host_id = chat.execution_host_id;
			owned->protocol_kind = "copilot-sdk";
			owned->running = owned->initialized = owned->session_ready = true;
			owned->lifecycle_state = "ready"; owned->current_model_id = "auto";
			owned->copilot_native_state["autoRoutingPublicLifetime"] = "public-handler-lifetime";
			owned->copilot_native_state["autoRouting"] = {
			    {"owner", {{"chatId", chat.id}, {"sessionId", chat.native_session_id}, {"executionHostId", chat.execution_host_id}, {"lifetime", "private-handler-lifetime"}}},
			    {"revision", 0}, {"catalogStatus", "ready"}, {"stateStatus", "ready"},
			    {"eligibleTiers", {{"present", true}, {"value", nlohmann::json::array({
			        {{"id", "vendor/Latency:Fast+V2"}, {"displayName", "Native latency preference"}, {"description", "Advertised tier"}, {"type", "auto"}, {"status", {{"enabled", true}}}}})}}},
			    {"providerDefault", {{"present", true}, {"value", "balance"}}},
			    {"currentModel", {{"present", true}, {"value", "auto"}}},
			    {"effective", {{"present", true}, {"value", "balance"}}},
			    {"pending", {{"present", false}}}, {"activating", {{"present", false}}},
			    {"requests", nlohmann::json::object()}, {"lastError", ""}};
#if defined(_WIN32)
			owned->stdin_write = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
#else
			owned->stdin_write_fd = fileno(file);
#endif
			owned->stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
			    [capture = bytes](const char* data, std::size_t count, std::string&) -> std::ptrdiff_t
			    { capture->append(data, count); return static_cast<std::ptrdiff_t>(count); }, [] {});
			session = owned.get(); app.acp_sessions.push_back(std::move(owned));
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
		}
		~NativeOptionFixture()
		{
			handler.reset(); session->stdin_writer.reset();
#if defined(_WIN32)
			session->stdin_write = INVALID_HANDLE_VALUE;
#else
			session->stdin_write_fd = -1;
#endif
			std::fclose(file);
		}
		nlohmann::json Payload() const
		{
			return {{"chatId", app.chats.front().id}, {"nativeSessionId", session->session_id},
			    {"executionHostId", "local"}, {"lifetime", "public-handler-lifetime"},
			    {"optionId", "autoRouting"}, {"value", nullptr}};
		}
		CefRefPtr<NativeOptionCallback> Invoke(const nlohmann::json& payload, bool refresh = false)
		{
			CefRefPtr<NativeOptionCallback> cb = new NativeOptionCallback;
			NativeModelOptionsHandlerTestAccess::Invoke(*handler, payload, cb, refresh);
			UAM_ASSERT(session->stdin_writer->Flush(std::chrono::seconds(2)));
			return cb;
		}
	};
}
#endif

UAM_TEST(NativeModelOptionHandlerRejectsMalformedScopeAndValue)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	NativeOptionFixture fixture;
	for (const char* key : {"chatId", "nativeSessionId", "executionHostId", "lifetime", "optionId", "value"})
	{
		auto payload = fixture.Payload(); payload.erase(key);
		const auto cb = fixture.Invoke(payload);
		UAM_ASSERT(cb->successes == 0 && cb->failures == 1 && cb->status == 400);
	}
	for (const nlohmann::json value : {nlohmann::json(true), nlohmann::json(2), nlohmann::json::object(), nlohmann::json(std::string(513, 'x'))})
	{
		auto payload = fixture.Payload(); payload["value"] = value;
		const auto cb = fixture.Invoke(payload);
		UAM_ASSERT(cb->successes == 0 && cb->failures == 1 && cb->status == 400);
	}
	UAM_ASSERT(fixture.bytes->empty() && fixture.app.chats.front().messages.empty());
#endif
}

UAM_TEST(NativeModelOptionHandlerRejectsStaleOwnershipAndReadOnlyChat)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	NativeOptionFixture fixture;
	for (const char* key : {"nativeSessionId", "executionHostId", "lifetime"})
	{
		auto payload = fixture.Payload(); payload[key] = "stale-owner";
		for (const bool refresh : {false, true})
		{
			const auto cb = fixture.Invoke(payload, refresh);
			UAM_ASSERT(cb->successes == 0 && cb->failures == 1 && cb->status == 409);
		}
	}
	fixture.app.chats.front().imported_read_only = true;
	const auto cb = fixture.Invoke(fixture.Payload());
	UAM_ASSERT(cb->successes == 0 && cb->failures == 1 && cb->status == 409);
	UAM_ASSERT(fixture.bytes->empty() && fixture.session->config_option_change_request_id == 0);
#endif
}

UAM_TEST(NativeModelOptionHandlerAcknowledgesPendingWithoutChangingEffective)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	for (const nlohmann::json value : {nlohmann::json(nullptr), nlohmann::json("vendor/Latency:Fast+V2")})
	{
		NativeOptionFixture fixture;
		auto payload = fixture.Payload(); payload["value"] = value;
		const auto cb = fixture.Invoke(payload);
		UAM_ASSERT(cb->failures == 0 && cb->successes == 1);
		UAM_ASSERT_EQ(cb->body, (nlohmann::json{{"pending", true}}));
		const auto boundary = fixture.bytes->find("\r\n\r\n"); UAM_ASSERT(boundary != std::string::npos);
		const auto wire = nlohmann::json::parse(fixture.bytes->substr(boundary + 4));
		UAM_ASSERT_EQ(wire.at("method"), "session.model.switchAutoTier");
		UAM_ASSERT_EQ(wire.at("params").at("autoTier"), value);
		const auto projection = uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"]["nativeModelOptions"]["autoRouting"];
		UAM_ASSERT_EQ(projection.at("effective"), (nlohmann::json{{"present", true}, {"value", "balance"}}));
		UAM_ASSERT(!projection.contains("requests") && !projection.contains("owner"));
		UAM_ASSERT(projection.dump().find("private-handler-lifetime") == std::string::npos);
		UAM_ASSERT(fixture.app.chats.front().messages.empty());
	}
#endif
}

UAM_TEST(NativeModelOptionHandlerRefreshesMetadataWithoutCreatingTurn)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	NativeOptionFixture fixture;
	const auto cb = fixture.Invoke(fixture.Payload(), true);
	UAM_ASSERT(cb->failures == 0 && cb->successes == 1);
	UAM_ASSERT_EQ(cb->body, (nlohmann::json{{"pending", true}}));
	UAM_ASSERT(fixture.bytes->find("session.model.list") != std::string::npos);
	UAM_ASSERT(fixture.bytes->find("session.model.getCurrent") != std::string::npos);
	UAM_ASSERT(fixture.bytes->find("session.send") == std::string::npos);
	UAM_ASSERT(fixture.session->prompt_request_id == 0 && fixture.app.chats.front().messages.empty());
	UAM_ASSERT(fixture.session->queued_user_prompts.empty() && fixture.app.chats.front().acp_queued_prompts.empty());
#endif
}


UAM_TEST(NativeModelOptionHandlerRejectsInternalMissingForeignAndBusySessions)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	for (const std::string gate : {"discovery", "goal", "managed", "provider", "missing", "processing", "compaction", "background"})
	{
		for (const bool refresh : {false, true})
		{
			NativeOptionFixture fixture;
			const auto payload = fixture.Payload();
			std::unique_ptr<uam::AcpSessionState> detached;
			if (gate == "discovery") fixture.session->model_discovery_only = true;
			if (gate == "goal") fixture.session->goal_internal_session = true;
			if (gate == "managed") fixture.session->managed_agent_run_id = "owned-managed-run";
			if (gate == "provider") fixture.session->provider_id = "codex-cli";
			if (gate == "processing") fixture.session->processing = true;
			if (gate == "compaction") fixture.session->context_compaction_in_progress = true;
			if (gate == "background") fixture.session->copilot_native_state["liveAgents"]["fixture-child"] = nlohmann::json::object();
			if (gate == "missing")
			{
				detached = std::move(fixture.app.acp_sessions.front());
				fixture.app.acp_sessions.clear();
			}
			const auto cb = fixture.Invoke(payload, refresh);
			if (detached) fixture.app.acp_sessions.push_back(std::move(detached));
			UAM_ASSERT(cb->successes == 0 && cb->failures == 1 && cb->status == 409);
			UAM_ASSERT(fixture.bytes->empty());
			UAM_ASSERT_EQ(fixture.session->config_option_change_request_id, 0);
			UAM_ASSERT(fixture.session->copilot_native_state.at("autoRouting").at("requests").empty());
			UAM_ASSERT(fixture.app.chats.front().messages.empty() && fixture.session->queued_user_prompts.empty());
		}
	}
#endif
}

#endif
