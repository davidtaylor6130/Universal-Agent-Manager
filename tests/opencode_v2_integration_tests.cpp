#include "test_harness.h"
#include "app/opencode_import_stream.h"
#include "common/provider/opencode_v2_session.h"
#include "common/runtime/acp/acp_session_internal.h"
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
namespace
{
	using Json = nlohmann::json;

	Json NativeExport(const ChatSession& chat)
	{
		return {{"info", {{"id", chat.native_session_id}, {"location", {{"directory", chat.workspace_directory}}}}},
		    {"messages", Json::array({
		        {{"id", "msg_user"}, {"type", "user"}, {"text", "Keep the exact authored request"}, {"time", {{"created", 1792780000000LL}}}},
		        {{"id", "msg_assistant"}, {"type", "assistant"}, {"agent", "build"}, {"model", {{"providerID", "mock"}, {"id", "fixture"}}},
		            {"content", Json::array({{{"type", "text"}, {"text", "Retained response"}}})},
		            {"time", {{"created", 1792780000010LL}, {"completed", 1792780000020LL}}}}
		    })}};
	}

	std::vector<Message> ImportNormalizedExport(const fs::path& file, const ChatSession& chat, const Json& native)
	{
		UAM_ASSERT(uam::io::WriteTextFile(file, uam::opencode_v2::NormalizeExport(native).dump()));
		std::vector<Message> messages;
		std::string error;
		UAM_ASSERT(uam::opencode_import::Read(file, chat, [&](const Message& message)
		{
			messages.push_back(message);
			return true;
		}, {}, error));
		UAM_ASSERT(error.empty());
		return messages;
	}

	struct HeldV2Turn
	{
		TempDir temp{"uam-v2-shared-hold"};
		uam::AppState app;

		HeldV2Turn()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultOpenCodeProfile()};
			// There is no process or input sink. An accidental admission cannot run a CLI.
			app.provider_profiles.front().interactive_command.clear();
			ChatSession chat;
			chat.id = "owned-v2-chat"; chat.provider_id = "opencode-cli";
			chat.native_session_id = "ses_v2_owned"; chat.opencode_v2_session_id = chat.native_session_id;
			chat.workspace_directory = temp.root.string(); chat.memory_enabled = false;
			chat.created_at = chat.updated_at = "2026-10-24T08:00:00.000Z";
			Message user; user.role = MessageRole::User; user.content = "Already delivered"; user.created_at = chat.created_at;
			Message assistant; assistant.role = MessageRole::Assistant; assistant.content = "Partial response"; assistant.created_at = chat.created_at;
			chat.messages = {user, assistant};
			for (const std::string id : {"queued-one", "queued-two"})
			{
				uam::AcpQueuedUserPromptState prompt; prompt.id = id; prompt.text = "Follow-up " + id;
				prompt.markdown_store_prompt_blocks = {"CAPTURED_SKILL"};
				chat.acp_queued_prompts.push_back(prompt);
			}
			app.chats.push_back(chat);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = chat.id; session->provider_id = chat.provider_id;
			session->process_execution_host_id = "local"; session->session_id = chat.native_session_id;
			session->opencode_v2_native = true; session->running = session->initialized = session->session_ready = session->processing = true;
			session->prompt_request_id = 7; session->pending_request_methods[7] = "session/prompt";
			session->turn_user_message_index = session->turn_first_user_message_index = 0;
			session->current_assistant_message_index = session->turn_assistant_message_index = 1;
			session->queued_user_prompts.assign(chat.acp_queued_prompts.begin(), chat.acp_queued_prompts.end());
			app.acp_sessions.push_back(std::move(session));
		}

		ChatSession& Chat() { return app.chats.front(); }
		uam::AcpSessionState& Session() { return *app.acp_sessions.front(); }

		void AssertHeld(const std::vector<uam::AcpQueuedUserPromptState>& expected)
		{
			const auto& session = Session(); const auto& chat = Chat();
			UAM_ASSERT(!session.running && !session.session_ready && !session.processing);
			UAM_ASSERT(!session.reconnect_pending && !session.cancel_requested && session.prompt_request_id == 0);
			UAM_ASSERT(session.queue_held && chat.acp_queue_held);
			UAM_ASSERT_EQ(session.lifecycle_state, std::string("error"));
			UAM_ASSERT(session.last_error.find("held") != std::string::npos);
			UAM_ASSERT(session.last_error.find("replayed automatically") != std::string::npos);
			UAM_ASSERT_EQ(chat.native_session_id, std::string("ses_v2_owned"));
			UAM_ASSERT_EQ(chat.acp_queued_prompts, expected);
			UAM_ASSERT_EQ(std::vector<uam::AcpQueuedUserPromptState>(session.queued_user_prompts.begin(), session.queued_user_prompts.end()), expected);
			UAM_ASSERT_EQ(chat.messages[0].content, std::string("Already delivered"));
			UAM_ASSERT(!chat.messages[0].acp_prompt_not_sent && chat.messages[1].interrupted);
			UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(app, Session(), Chat()));
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
			UAM_ASSERT(saved && saved->acp_queue_held && saved->acp_queued_prompts == expected);
			UAM_ASSERT(saved->opencode_v2_session_id == chat.opencode_v2_session_id);
		}
	};
}

UAM_TEST(OpenCodeV2CompletedHistoryImportsNativeIdleOutcomes)
{
	TempDir temp("uam-v2-history-idle");
	ChatSession chat; chat.id = "imported-v2"; chat.provider_id = "opencode-cli";
	chat.native_session_id = "ses_import"; chat.workspace_directory = temp.root.string();
	for (const std::string outcome : {"succeeded", "failed", "interrupted"})
	{
		auto native = NativeExport(chat);
		Json idle{{"id", "msg_idle"}, {"type", "idle"}, {"outcome", outcome}, {"time", {{"created", 1792780000030LL}}}};
		if (outcome == "failed") idle["error"] = {{"type", "api"}, {"message", "Native failure"}, {"status", 503}};
		if (outcome == "interrupted") native["messages"][1]["error"] = {{"type", "aborted"}, {"message", "Cancelled"}};
		native["messages"].push_back(idle);
		const auto messages = ImportNormalizedExport(temp.root / "export.json", chat, native);
		UAM_ASSERT_EQ(messages.size(), std::size_t{2});
		UAM_ASSERT_EQ(messages[0].content, std::string("Keep the exact authored request"));
		UAM_ASSERT_EQ(messages[1].content, std::string("Retained response"));
		UAM_ASSERT_EQ(messages[1].interrupted, outcome == "interrupted");
		UAM_ASSERT(chat.acp_queued_prompts.empty() && !chat.remote_turn_reconnect_pending);
	}
}

UAM_TEST(OpenCodeV2FailedToolHistoryImportsStructuredErrors)
{
	TempDir temp("uam-v2-history-errors");
	ChatSession chat; chat.id = "imported-tool"; chat.provider_id = "opencode-cli";
	chat.native_session_id = "ses_tool_import"; chat.workspace_directory = temp.root.string();
	for (const bool content : {false, true})
	{
		auto native = NativeExport(chat);
		const Json error{{"type", "tool"}, {"message", "Fixture command failed"}, {"status", 500}};
		Json state{{"status", "error"}, {"input", {{"command", "synthetic fixture"}}}, {"error", error}};
		if (content) state["content"] = Json::array({{{"type", "text"}, {"text", "Partial tool output"}}});
		native["messages"][1]["content"].push_back({{"type", "tool"}, {"id", "tool_one"}, {"name", "bash"}, {"state", state}, {"time", {{"created", 1792780000010LL}, {"completed", 1792780000020LL}}}});
		const auto normalized = uam::opencode_v2::NormalizeExport(native);
		UAM_ASSERT_EQ(normalized["messages"][1]["parts"][1]["state"]["errorDetails"], error);
		const auto messages = ImportNormalizedExport(temp.root / "export.json", chat, native);
		UAM_ASSERT_EQ(messages.size(), std::size_t{2});
		UAM_ASSERT_EQ(messages[1].tool_calls.size(), std::size_t{1});
		const auto& tool = messages[1].tool_calls.front();
		UAM_ASSERT_EQ(tool.status, std::string("error"));
		UAM_ASSERT_EQ(tool.id, uam::opencode_v2::ToolKey("msg_assistant", "tool_one"));
		UAM_ASSERT_EQ(tool.result_text, std::string("Fixture command failed"));
	}
}

UAM_TEST(OpenCodeV2HistoryOverlayPreservesOriginOnlyForSameNativeOwner)
{
	TempDir temp("uam-v2-history-origin");
	const auto& runtime = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	const auto profile = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession local; local.id = "v2-merge"; local.provider_id = "opencode-cli";
	local.native_session_id = "ses_owned"; local.workspace_directory = temp.root.string();
	local.created_at = local.updated_at = "2026-10-24T08:00:00.000Z";
	for (const std::string hint : {"ses_owned", ""})
	{
		local.opencode_v2_session_id = hint;
		auto incoming = local; incoming.opencode_v2_session_id.reset();
		ChatHistorySyncService().OverlayPreparedLocalHistory(local, incoming);
		UAM_ASSERT(incoming.opencode_v2_session_id == local.opencode_v2_session_id);
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, incoming));
		const auto saved = ChatRepository::LoadLocalChat(temp.root, incoming.id);
		UAM_ASSERT(saved && saved->opencode_v2_session_id == local.opencode_v2_session_id);
		UAM_ASSERT(!runtime.StructuredCliVersionError(profile, "1.19.4", false, &*saved).empty());
		uam::AppState app; uam::AcpSessionState session; session.agent_version = "1.19.4";
		std::string error;
		UAM_ASSERT(runtime.OnAcpPrepareSetup(app, session, *saved, local.workspace_directory, error) == AcpRequestPreflight::Rejected);
	}
	local.opencode_v2_session_id = "ses_owned";
	for (int mismatch = 0; mismatch < 5; ++mismatch)
	{
		auto incoming = local; incoming.opencode_v2_session_id.reset();
		if (mismatch == 0) incoming.native_session_id = "ses_other";
		if (mismatch == 1) incoming.execution_host_id = "remote-other";
		if (mismatch == 2) incoming.workspace_directory = (temp.root / "other").string();
		if (mismatch == 3) incoming.provider_id = "codex-cli";
		if (mismatch == 4) incoming.native_session_id.clear();
		ChatHistorySyncService().OverlayPreparedLocalHistory(local, incoming);
		UAM_ASSERT(!incoming.opencode_v2_session_id.has_value());
	}
	auto incoming = local; local.opencode_v2_session_id.reset();
	ChatHistorySyncService().OverlayPreparedLocalHistory(local, incoming);
	UAM_ASSERT(incoming.opencode_v2_session_id == std::optional<std::string>{"ses_owned"});
}

UAM_TEST(OpenCodeV2CancelledPromptErrorsHoldQueueThroughSharedDispatch)
{
	HeldV2Turn fixture; auto& session = fixture.Session();
	const auto expected = fixture.Chat().acp_queued_prompts;
	session.cancel_requested = true; session.cancel_requested_time_s = 10.0;
	// Even an older bridge without the new machine-readable error data must hold
	// an owned prompt error during cancellation, rather than report cancel success.
	uam::acp_detail::HandleAcpResponse(fixture.app, session, fixture.Chat(),
	    {{"jsonrpc", "2.0"}, {"id", 7}, {"error", {{"code", -32603}, {"message", "Owned cancellation is ambiguous"}}}});
	fixture.AssertHeld(expected);
}

UAM_TEST(OpenCodeV2DisconnectedPromptHoldsQueueWithoutCancel)
{
	HeldV2Turn fixture;
	const auto expected = fixture.Chat().acp_queued_prompts;
	uam::acp_detail::HandleAcpResponse(fixture.app, fixture.Session(), fixture.Chat(),
	    {{"jsonrpc", "2.0"}, {"id", 7}, {"error", {{"code", -32603}, {"message", "Owned event transport failed"},
	        {"data", {{"uamInputHeld", true}, {"noReplay", true}}}}}});
	fixture.AssertHeld(expected);
}

UAM_TEST(OpenCodeV2SharedCancelDeadlineHoldsBeforeInactivityRecovery)
{
	HeldV2Turn fixture; auto& session = fixture.Session();
	const auto expected = fixture.Chat().acp_queued_prompts;
	session.cancel_requested = true; session.cancel_requested_time_s = 100.0;
	session.inactivity_timeout_pending = true;
	UAM_ASSERT_EQ(uam::acp_detail::kAcpCancelTimeoutSeconds, 5.0);
	UAM_ASSERT(!uam::acp_detail::HoldAcpUnconfirmedCancellationIfTimedOut(fixture.app, session, fixture.Chat(), 104.999));
	UAM_ASSERT(session.running && session.processing && !session.queue_held);
	UAM_ASSERT(uam::acp_detail::HoldAcpUnconfirmedCancellationIfTimedOut(fixture.app, session, fixture.Chat(), 105.0));
	fixture.AssertHeld(expected);
}

UAM_TEST(OpenCodeV2TransportLossHoldsQueueThroughSharedPolling)
{
	for (int path = 0; path < 3; ++path)
	{
		HeldV2Turn fixture;
		const auto expected = fixture.Chat().acp_queued_prompts;
		if (path == 0) uam::acp_detail::InvalidateAcpTransport(fixture.app, fixture.Session(), fixture.Chat(), "Owned frame was invalid");
		if (path == 1) uam::acp_detail::RecoverDisconnectedRemoteAcpTransport(fixture.app, fixture.Session(), fixture.Chat(), "Owned input transport failed");
		if (path == 2)
		{
			// Private empty files give the real poller clean EOF, so closed-handle
			// read errors cannot accidentally satisfy the observed-exit regression.
			// No process, input sink or provider is launched.
			const fs::path empty = fixture.temp.root / "empty-output";
			UAM_ASSERT(uam::io::WriteTextFile(empty, ""));
#if defined(_WIN32)
			fixture.Session().stdout_read = CreateFileW(empty.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			fixture.Session().stderr_read = CreateFileW(empty.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			UAM_ASSERT(fixture.Session().stdout_read != INVALID_HANDLE_VALUE && fixture.Session().stderr_read != INVALID_HANDLE_VALUE);
#else
			fixture.Session().stdout_read_fd = ::open(empty.c_str(), O_RDONLY);
			fixture.Session().stderr_read_fd = ::open(empty.c_str(), O_RDONLY);
			UAM_ASSERT(fixture.Session().stdout_read_fd >= 0 && fixture.Session().stderr_read_fd >= 0);
#endif
			fixture.Session().process_exit_observed = true;
			fixture.Session().observed_process_exit_code = 1;
			fixture.Session().last_runtime_activity_time_s = uam::GetAppTimeSeconds();
			(void)uam::PollAllAcpSessions(fixture.app);
			UAM_ASSERT(fixture.Session().last_error.find("owned provider process exited") != std::string::npos);
		}
		fixture.AssertHeld(expected);
	}
}

UAM_TEST(OpenCodeV1CancellationShortcutAndDeadlineKeepLegacyBehavior)
{
	HeldV2Turn fixture; auto& session = fixture.Session(); auto& chat = fixture.Chat();
	session.opencode_v2_native = false; chat.acp_queued_prompts.clear(); session.queued_user_prompts.clear();
	session.cancel_requested = true; session.cancel_requested_time_s = 100.0;
	UAM_ASSERT(!uam::acp_detail::HoldAcpUnconfirmedCancellationIfTimedOut(fixture.app, session, chat, 105.0));
	uam::acp_detail::HandleAcpResponse(fixture.app, session, chat,
	    {{"jsonrpc", "2.0"}, {"id", 7}, {"error", {{"code", -32603}, {"message", "Legacy cancellation response"}}}});
	UAM_ASSERT(session.running && session.session_ready && !session.cancel_requested);
	UAM_ASSERT(!session.queue_held && !chat.acp_queue_held && session.prompt_request_id == 0);
}

UAM_TEST(OpenCodeV2ObservedExitRetainsNativeOwnerValidation)
{
	const auto& runtime = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	for (int mismatch = 0; mismatch < 3; ++mismatch)
	{
		HeldV2Turn fixture;
		const auto messages = fixture.Chat().messages;
		const auto queue = fixture.Chat().acp_queued_prompts;
		if (mismatch == 0) fixture.Chat().native_session_id = "ses_changed";
		if (mismatch == 1) fixture.Session().provider_id = "codex-cli";
		if (mismatch == 2) fixture.Session().process_execution_host_id = "foreign-host";
		UAM_ASSERT(runtime.OnAcpHoldUnconfirmedInput(fixture.app, fixture.Session(), fixture.Chat(), "Observed process exit", 1));
		UAM_ASSERT(!fixture.Session().transport_ownership_error.empty());
		UAM_ASSERT_EQ(fixture.Chat().messages, messages);
		UAM_ASSERT_EQ(fixture.Chat().acp_queued_prompts, queue);
		UAM_ASSERT(!fixture.Chat().acp_queue_held && fixture.Session().prompt_request_id == 7);
		UAM_ASSERT(fixture.app.acp_process_stop_tasks.empty());
	}
}

UAM_TEST(OpenCodeV2ConfirmedCancellationKeepsReadySession)
{
	HeldV2Turn fixture; auto& session = fixture.Session(); auto& chat = fixture.Chat();
	chat.acp_queued_prompts.clear(); session.queued_user_prompts.clear();
	session.cancel_requested = true; session.cancel_requested_time_s = 100.0;
	uam::acp_detail::HandleAcpResponse(fixture.app, session, chat,
	    {{"jsonrpc", "2.0"}, {"id", 7}, {"result", {{"stopReason", "cancelled"}}}});
	UAM_ASSERT(session.running && session.session_ready && !session.cancel_requested);
	UAM_ASSERT(!session.queue_held && !chat.acp_queue_held && session.prompt_request_id == 0);
}

UAM_TEST(OpenCodeV2AmbiguousAdmissionHoldsThroughProductionEngineAndSharedDispatch)
{
	for (int failure = 0; failure < 4; ++failure)
	{
		HeldV2Turn fixture;
		const auto expected = fixture.Chat().acp_queued_prompts;
		const auto owned = fixture.Chat().native_session_id;
		const auto cwd = fixture.Chat().workspace_directory;
		Json info{{"id", owned}, {"agent", "build"}, {"location", {{"directory", cwd}}}};
		std::vector<Json> replies;
		int admissions = 0;
		uam::opencode_v2::Session native([&](const std::string& method, const std::string& route, const Json& body) -> Json
		{
			if (route.starts_with("/api/location?")) return {{"data", {{"directory", cwd}}}};
			if (route.starts_with("/api/plugin?")) return {{"data", Json::array()}};
			if (route.starts_with("/api/model/default?")) return {{"data", {{"providerID", "mock"}, {"id", "fixture"}}}};
			if (route.starts_with("/api/model?")) return {{"data", Json::array({{{"providerID", "mock"}, {"id", "fixture"}}})}};
			if (method == "GET" && route == "/api/session/" + owned) return {{"data", info}};
			if (method == "PATCH" && route == "/api/session/" + owned)
			{
				info["permissions"] = body.at("permissions");
				return Json::object();
			}
			if (method == "GET" && route.ends_with("/inbox")) return {{"data", Json::array()}};
			if (method == "POST" && route.ends_with("/prompt"))
			{
				++admissions; // Native side received the POST even if its reply is unusable.
				if (failure == 0) throw std::runtime_error("Injected lost admission reply");
				if (failure == 1) return {{"data", Json::object()}};
				if (failure == 2) return {{"data", {{"id", 42}, {"sessionID", owned}}}};
				return {{"data", {{"id", "msg_admitted"}, {"sessionID", "ses_other"}}}};
			}
			throw std::runtime_error("Unexpected mocked native route");
		}, [&](const std::string& id, const std::string& directory, bool)
		{
			UAM_ASSERT(id.empty() || id == owned);
			UAM_ASSERT_EQ(directory, cwd);
		}, [&](const Json& reply) { replies.push_back(reply); });
		native.Handle({{"id", 1}, {"method", "session/load"}, {"params", {{"cwd", cwd}, {"sessionId", owned}, {"_meta", {{"uamPolicy", {{"readOnly", false}}}}}}}});
		UAM_ASSERT(replies.back().contains("result"));
		native.Handle({{"id", 7}, {"method", "session/prompt"}, {"params", {{"sessionId", owned},
		    {"prompt", Json::array({{{"type", "text"}, {"text", "Already delivered"}}})}, {"_meta", {{"uamPolicy", {{"readOnly", false}}}}}}}});
		const Json error = replies.back();
		UAM_ASSERT_EQ(error.at("id"), Json(7));
		UAM_ASSERT(error.at("error").at("data").value("uamInputHeld", false));
		UAM_ASSERT(error.at("error").at("data").value("noReplay", false));
		UAM_ASSERT(native.Broken() && !native.HasTurn() && admissions == 1);
		uam::acp_detail::HandleAcpResponse(fixture.app, fixture.Session(), fixture.Chat(), error);
		fixture.AssertHeld(expected);
		native.Handle({{"id", 8}, {"method", "session/prompt"}, {"params", {{"sessionId", owned}}}});
		UAM_ASSERT_EQ(admissions, 1);
	}
}

UAM_TEST(OpenCodeV2CancelledUsageIsObservedOnceWithoutAdmittingInput)
{
	HeldV2Turn fixture; auto& session = fixture.Session(); auto& chat = fixture.Chat();
	chat.acp_queued_prompts.clear(); session.queued_user_prompts.clear();
	session.cancel_requested = true;
	const Json response{{"jsonrpc", "2.0"}, {"id", 7}, {"result", {{"stopReason", "cancelled"},
	    {"usage", {{"inputTokens", 10}, {"cachedReadTokens", 3}, {"cachedWriteTokens", 2},
	        {"outputTokens", 4}, {"thoughtTokens", 1}, {"totalTokens", 20}}}}}};
	uam::acp_detail::HandleAcpResponse(fixture.app, session, chat, response);
	const auto& usage = session.provider_usage.token_usage;
	UAM_ASSERT(usage.available && usage.last_available);
	UAM_ASSERT_EQ(usage.session_id, chat.native_session_id);
	UAM_ASSERT_EQ(usage.total.input_tokens, 15LL);
	UAM_ASSERT_EQ(usage.total.output_tokens, 5LL);
	UAM_ASSERT_EQ(usage.total.cached_input_tokens, 3LL);
	UAM_ASSERT_EQ(usage.total.cache_write_input_tokens, 2LL);
	UAM_ASSERT_EQ(usage.total.reasoning_output_tokens, 1LL);
	UAM_ASSERT_EQ(usage.total.total_tokens, 20LL);
	UAM_ASSERT(session.running && session.session_ready && !session.cancel_requested && session.prompt_request_id == 0);
	UAM_ASSERT(!session.queue_held && !chat.acp_queue_held && session.pending_request_methods.empty());
	uam::acp_detail::HandleAcpResponse(fixture.app, session, chat, response);
	UAM_ASSERT_EQ(usage.total.total_tokens, 20LL);
	UAM_ASSERT(fixture.app.acp_process_stop_tasks.empty());
}

UAM_TEST(OpenCodeV2CancelledUsageRejectsMalformedCountsAndPreservesV1Shortcut)
{
	for (int variant = 0; variant < 4; ++variant)
	{
		HeldV2Turn fixture; auto& session = fixture.Session(); auto& chat = fixture.Chat();
		chat.acp_queued_prompts.clear(); session.queued_user_prompts.clear(); session.cancel_requested = true;
		Json usage{{"inputTokens", 10}, {"outputTokens", 2}, {"totalTokens", 12}};
		if (variant == 0) usage["inputTokens"] = -1;
		if (variant == 1) usage["totalTokens"] = 13;
		if (variant == 2) usage["inputTokens"] = 18446744073709551615ULL;
		if (variant == 3) session.opencode_v2_native = false;
		uam::acp_detail::HandleAcpResponse(fixture.app, session, chat,
		    {{"id", 7}, {"result", {{"stopReason", "cancelled"}, {"usage", usage}}}});
		UAM_ASSERT(!session.provider_usage.token_usage.available);
		UAM_ASSERT_EQ(session.provider_usage.token_usage.total.total_tokens, 0LL);
		UAM_ASSERT(session.running && session.session_ready && !session.cancel_requested);
	}
}

#if defined(_WIN32)
UAM_TEST(OpenCodeV2WindowsFiniteInputReadsPreloadedFileAndRetainsPipeRules)
{
	using Status = uam::platform::StandardInputReadStatus;
	TempDir temp("uam-v2-private-stdin");
	auto& service = PlatformServicesFactory::Instance().process_service;
	struct Input
	{
		HANDLE original = GetStdHandle(STD_INPUT_HANDLE), read = INVALID_HANDLE_VALUE, write = INVALID_HANDLE_VALUE;
		~Input()
		{
			SetStdHandle(STD_INPUT_HANDLE, original);
			if (read != INVALID_HANDLE_VALUE) CloseHandle(read);
			if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
		}
	};
	char bytes[3]{};
	{
		Input input;
		const fs::path file = temp.root / "preloaded";
		input.read = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_DELETE,
		    nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		UAM_ASSERT(input.read != INVALID_HANDLE_VALUE);
		UAM_ASSERT(DeleteFileW(file.c_str())); // Same private finite-file semantics as worker launcher.
		DWORD count = 0;
		UAM_ASSERT(WriteFile(input.read, "hello", 5, &count, nullptr) && count == 5);
		LARGE_INTEGER start{}; UAM_ASSERT(SetFilePointerEx(input.read, start, nullptr, FILE_BEGIN));
		UAM_ASSERT(SetStdHandle(STD_INPUT_HANDLE, input.read));
		UAM_ASSERT(service.ReadStandardInput(bytes, sizeof(bytes)).status == Status::Error);
		UAM_ASSERT(service.ReadFiniteStandardInput(bytes, sizeof(bytes), 4).status == Status::Error);
		auto first = service.ReadFiniteStandardInput(bytes, sizeof(bytes), 5);
		UAM_ASSERT(first.status == Status::Data && std::string(bytes, first.size) == "hel");
		auto last = service.ReadFiniteStandardInput(bytes, sizeof(bytes), 5);
		UAM_ASSERT(last.status == Status::Data && std::string(bytes, last.size) == "lo");
		UAM_ASSERT(service.ReadFiniteStandardInput(bytes, sizeof(bytes), 5).status == Status::EndOfStream);
		UAM_ASSERT(GetFileType(input.read) == FILE_TYPE_DISK); // Reader never closes input.
	}
	{
		Input input;
		UAM_ASSERT(CreatePipe(&input.read, &input.write, nullptr, 0));
		UAM_ASSERT(SetStdHandle(STD_INPUT_HANDLE, input.read));
		UAM_ASSERT(service.ReadFiniteStandardInput(bytes, sizeof(bytes), 5).status == Status::Pending);
		DWORD count = 0; UAM_ASSERT(WriteFile(input.write, "ok", 2, &count, nullptr) && count == 2);
		auto read = service.ReadFiniteStandardInput(bytes, sizeof(bytes), 5);
		UAM_ASSERT(read.status == Status::Data && std::string(bytes, read.size) == "ok");
		CloseHandle(input.write); input.write = INVALID_HANDLE_VALUE;
		UAM_ASSERT(service.ReadFiniteStandardInput(bytes, sizeof(bytes), 5).status == Status::EndOfStream);
	}
	{
		Input input;
		input.read = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		UAM_ASSERT(input.read != INVALID_HANDLE_VALUE && SetStdHandle(STD_INPUT_HANDLE, input.read));
		UAM_ASSERT(service.ReadFiniteStandardInput(bytes, sizeof(bytes), 5).status == Status::Error);
	}
}
#endif
#endif
