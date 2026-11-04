#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/platform/async_byte_writer.h"
#include "common/provider/provider_runtime.h"

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace uam_test;

namespace
{
	// Consumer/retirement fixture only: synthetic runtime correlation, no provider/child process.
	// The actual AsyncByteWriter callback writes bytes to this fixture's private OS pipe.
	struct LocalInputWriteFixture
	{
		TempDir temp{"uam-local-input-fact"};
		uam::AppState app;
		std::shared_ptr<std::atomic<bool>> release = std::make_shared<std::atomic<bool>>(false);
#if defined(_WIN32)
		HANDLE read_handle = INVALID_HANDLE_VALUE;
#else
		int read_fd = -1;
#endif
		int serial = 0;
		std::string last_wire;
		explicit LocalInputWriteFixture(const std::string& provider_id = "codex-cli")
		{
			app.data_root = temp.root / "data";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			ProviderProfile provider;
			provider.id = provider_id;
			provider.interactive_command = "private-fixture-never-executed";
			provider.supports_structured = true;
			provider.structured_protocol = provider_id == "claude-cli" ? "claude-code-stream-json" : "codex-app-server";
			app.provider_profiles = {provider};
			ChatSession chat = ChatDomainService().CreateNewChat({}, provider.id, false);
			chat.workspace_directory = temp.root.string();
			chat.execution_host_id = "local";
			chat.native_session_id = provider_id == "claude-cli" ? "" : "fixture-native";
			app.chats.push_back(std::move(chat));
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = Chat().id;
			session->provider_id = provider.id;
			session->protocol_kind = provider.structured_protocol;
			session->session_id = Chat().native_session_id;
			session->process_execution_host_id = "local";
			session->running = true;
			session->last_process_id = "unit-controlled-pipe-only";
			// No PID/process HANDLE is seeded: the label is explicitly synthetic correlation.
#if defined(_WIN32)
			UAM_ASSERT(CreatePipe(&read_handle, &session->stdin_write, nullptr, 0));
			const HANDLE write_handle = session->stdin_write;
			const auto write = [gate = release, write_handle](const char* bytes, std::size_t count, std::string& error) -> std::ptrdiff_t
			{
				if (!gate->load()) return 0;
				DWORD written = 0;
				if (!WriteFile(write_handle, bytes, static_cast<DWORD>(count), &written, nullptr)) { error = "private pipe write failed"; return -1; }
				return static_cast<std::ptrdiff_t>(written);
			};
#else
			int pipe_fds[2];
			UAM_ASSERT(pipe(pipe_fds) == 0);
			read_fd = pipe_fds[0];
			session->stdin_write_fd = pipe_fds[1];
			UAM_ASSERT(fcntl(read_fd, F_SETFL, fcntl(read_fd, F_GETFL, 0) | O_NONBLOCK) == 0);
			const int write_fd = session->stdin_write_fd;
			const auto write = [gate = release, write_fd](const char* bytes, std::size_t count, std::string& error) -> std::ptrdiff_t
			{
				if (!gate->load()) return 0;
				const ssize_t written = ::write(write_fd, bytes, count);
				if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
				if (written < 0) error = "private pipe write failed";
				return written;
			};
#endif
			session->stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(write, [] {});
			app.acp_sessions.push_back(std::move(session));
			NewAuthoredRow();
		}
		~LocalInputWriteFixture()
		{
			release->store(true);
			if (Session().stdin_writer) Session().stdin_writer->Stop();
			Session().stdin_writer.reset();
#if defined(_WIN32)
			if (Session().stdin_write != INVALID_HANDLE_VALUE) CloseHandle(Session().stdin_write);
			Session().stdin_write = INVALID_HANDLE_VALUE;
			if (read_handle != INVALID_HANDLE_VALUE) CloseHandle(read_handle);
#else
			if (Session().stdin_write_fd >= 0) close(Session().stdin_write_fd);
			Session().stdin_write_fd = -1;
			if (read_fd >= 0) close(read_fd);
#endif
			Session().running = false; // No Stop/process termination API is invoked.
		}
		ChatSession& Chat() { return app.chats.front(); }
		uam::AcpSessionState& Session() { return *app.acp_sessions.front(); }
		void NewAuthoredRow(bool append = false)
		{
			++serial;
			if (!append) Chat().messages.clear();
			Message row;
			row.role = MessageRole::User;
			row.content = "owned private arithmetic " + std::to_string(serial);
			row.created_at = Chat().created_at;
			row.native_input_id = uam::AgentRunLedger::NewRunId();
			row.origin_request_id = "private-request-" + std::to_string(serial);
			Chat().messages.push_back(std::move(row));
			Session().turn_serial = serial;
			Session().prompt_request_id = serial;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, Chat()));
		}
		void Begin(std::size_t row_index = 0)
		{
			release->store(false);
			const std::size_t before = app.local_input_write_observations.size();
			nlohmann::json message;
			std::string method;
			if (Session().provider_id == "claude-cli")
			{
				message = {{"type", "user"}, {"session_id", Session().session_id}};
				message["message"] = {{"role", "user"}};
				message["message"]["content"] = nlohmann::json::array({{{"type", "text"}, {"text", Chat().messages.at(row_index).content}}});
			}
			else
			{
				method = "turn/start";
				message = {{"jsonrpc", "2.0"}, {"method", method}, {"id", serial}};
				message["params"] = {{"threadId", Session().session_id}};
				message["params"]["input"] = nlohmann::json::array({{{"type", "text"}, {"text", Chat().messages.at(row_index).content}}});
			}
			last_wire = uam::acp_detail::SerializeStructuredMessage(Session(), message);
			UAM_ASSERT(uam::acp_detail::WriteLocalAcpPromptWithObservation(app, Session(), Chat(), message, method, serial, {row_index}));
			UAM_ASSERT_EQ(app.local_input_write_observations.size(), before + 1);
		}
		void CompleteAndReadExactPipeBytes()
		{
			release->store(true);
			UAM_ASSERT(Session().stdin_writer->Flush(std::chrono::seconds(5)));
			std::string actual;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (actual.size() < last_wire.size() && std::chrono::steady_clock::now() < deadline)
			{
				char buffer[1024];
#if defined(_WIN32)
				DWORD available = 0, count = 0;
				UAM_ASSERT(PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr));
				if (available) { UAM_ASSERT(ReadFile(read_handle, buffer, static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), available)), &count, nullptr)); actual.append(buffer, count); }
#else
				const ssize_t count = read(read_fd, buffer, sizeof(buffer));
				if (count > 0) actual.append(buffer, static_cast<std::size_t>(count));
				else UAM_ASSERT(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#endif
				if (actual.size() < last_wire.size()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT_EQ(actual, last_wire);
		}
	};
}

UAM_TEST(LocalInputWriteFactNeedsCompleteRealControlledSink)
{
	LocalInputWriteFixture fixture;
	fixture.Begin();
	UAM_ASSERT(!uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(!fixture.Chat().messages.front().input_write_completed);
	fixture.CompleteAndReadExactPipeBytes();
	UAM_ASSERT(uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(fixture.Chat().messages.front().input_write_completed);
	UAM_ASSERT(fixture.app.local_input_write_observations.empty());
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.Chat().id));
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id, true, nullptr, false);
	UAM_ASSERT(saved && saved->messages.front().input_write_completed);
}

UAM_TEST(LocalInputWriteMoreThanEightOrphansDoNotExhaustFutureObservation)
{
	LocalInputWriteFixture fixture;
	for (int attempt = 0; attempt < 12; ++attempt)
	{
		if (attempt) fixture.NewAuthoredRow();
		fixture.Begin();
		fixture.Chat().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		UAM_ASSERT(!uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
		UAM_ASSERT(fixture.app.local_input_write_observations.empty());
		UAM_ASSERT(!fixture.Chat().messages.front().input_write_completed);
		UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.Chat().id));
		fixture.CompleteAndReadExactPipeBytes(); // Original sink may complete; retired observation cannot project it.
	}
	fixture.NewAuthoredRow();
	fixture.Begin();
	fixture.CompleteAndReadExactPipeBytes();
	UAM_ASSERT(uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(fixture.Chat().messages.front().input_write_completed);
}

UAM_TEST(LocalInputWriteRestoringOrphanFieldsCannotReviveRetiredObservation)
{
	LocalInputWriteFixture fixture;
	const auto original_incarnation = fixture.Chat().metadata_incarnation;
	fixture.Begin();
	fixture.Chat().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
	UAM_ASSERT(!uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(fixture.app.local_input_write_observations.empty());
	fixture.Chat().metadata_incarnation = original_incarnation;
	fixture.CompleteAndReadExactPipeBytes();
	UAM_ASSERT(!uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(!fixture.Chat().messages.front().input_write_completed);
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.Chat().id));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id, true, nullptr, false);
	UAM_ASSERT(saved && !saved->messages.front().input_write_completed);
}

UAM_TEST(LocalInputWriteEditedRowCannotReceiveOldCompleteFact)
{
	LocalInputWriteFixture fixture;
	fixture.Begin();
	fixture.CompleteAndReadExactPipeBytes();
	fixture.Chat().messages.front().content += " edited after handoff";
	UAM_ASSERT(!uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(fixture.app.local_input_write_observations.empty());
	UAM_ASSERT(!fixture.Chat().messages.front().input_write_completed);
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.Chat().id));
}

UAM_TEST(LocalInputWriteCompleteSurvivesActualClaudeInitTransition)
{
	LocalInputWriteFixture fixture("claude-cli");
	const auto original_incarnation = fixture.Chat().metadata_incarnation;
	UAM_ASSERT(fixture.Session().session_id.empty());
	fixture.Begin();
	fixture.CompleteAndReadExactPipeBytes(); // Actual original observed entry completes before init is consumed.
	const std::string native = uam::AgentRunLedger::NewRunId();
	const nlohmann::json init = {{"type", "system"}, {"subtype", "init"}, {"session_id", native},
	    {"model", "claude-unit-init-model"}, {"permissionMode", "acceptEdits"}};
	UAM_ASSERT(ProviderRuntimeRegistry::ResolveById(fixture.Session().provider_id).OnAcpHandleMessage(fixture.app, fixture.Session(), fixture.Chat(), init, nullptr));
	UAM_ASSERT_EQ(fixture.Session().session_id, native);
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, native);
	UAM_ASSERT_EQ(fixture.Session().current_model_id, std::string("claude-unit-init-model"));
	UAM_ASSERT_EQ(fixture.Session().current_mode_id, std::string("acceptEdits"));
	UAM_ASSERT_EQ(fixture.Chat().metadata_incarnation, original_incarnation);
	UAM_ASSERT(uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(fixture.Chat().messages.front().input_write_completed);
	UAM_ASSERT(fixture.app.local_input_write_observations.empty());
}

UAM_TEST(LocalInputWriteCompleteSurvivesSourceShapedFifoTurnAdvanceOnlyMarksA)
{
	LocalInputWriteFixture fixture;
	fixture.Begin();
	const auto id_a = fixture.Chat().messages.front().native_input_id;
	fixture.CompleteAndReadExactPipeBytes();
	// SOURCE-SHAPED queue advancement: same turn-serial increment and row append as B admission.
	// Deliberately not a full DrainNextQueuedAcpUserPrompt/workspace/admission integration proof.
	const int turn_a = fixture.Session().turn_serial;
	fixture.NewAuthoredRow(true);
	UAM_ASSERT_EQ(fixture.Session().turn_serial, turn_a + 1);
	fixture.Begin(1); // B really reaches the SAME production handoff but its real sink stays Pending.
	UAM_ASSERT_EQ(fixture.app.local_input_write_observations.size(), std::size_t{2});
	UAM_ASSERT(uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT_EQ(fixture.Chat().messages.front().native_input_id, id_a);
	UAM_ASSERT(fixture.Chat().messages.front().input_write_completed);
	UAM_ASSERT(!fixture.Chat().messages.at(1).input_write_completed);
	UAM_ASSERT_EQ(fixture.app.local_input_write_observations.size(), std::size_t{1});
	fixture.CompleteAndReadExactPipeBytes();
	UAM_ASSERT(uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(fixture.Chat().messages.at(1).input_write_completed);
}

UAM_TEST(LocalInputWriteOldCompleteSurvivesStopAndWriterReplacementForSameRawRow)
{
	LocalInputWriteFixture fixture;
	fixture.Begin();
	fixture.CompleteAndReadExactPipeBytes();
	const auto id = fixture.Chat().messages.front().native_input_id;
	fixture.Session().stdin_writer->Stop();
	fixture.Session().stdin_writer.reset();
	fixture.Session().running = false;
	fixture.Session().local_stop_pending = true;
	fixture.Session().permission_review_incarnation = std::make_shared<const char>(0);
	fixture.Session().last_process_id = "unit-replacement-not-authority";
	fixture.Chat().messages.front().acp_prompt_not_sent = true; // Stop/held projection cannot erase historical physical write.
	UAM_ASSERT(uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT_EQ(fixture.Chat().messages.front().native_input_id, id);
	UAM_ASSERT(fixture.Chat().messages.front().input_write_completed);
	UAM_ASSERT(fixture.Chat().messages.front().acp_prompt_not_sent); // No admission/retry-state promotion.
	UAM_ASSERT(fixture.Session().local_stop_pending); // No cleanup authority consumed.
}

UAM_TEST(LocalInputWriteIncompleteSinkNeverPromotesHistoricalFact)
{
	LocalInputWriteFixture fixture;
	fixture.Begin(); // Gate stays closed; real writer has only queued input.
	fixture.Session().stdin_writer->Stop(); // Pending receipt terminates Incomplete, never fabricated Complete.
	UAM_ASSERT(!uam::acp_detail::PollLocalInputWriteObservations(fixture.app));
	UAM_ASSERT(fixture.app.local_input_write_observations.empty());
	UAM_ASSERT(!fixture.Chat().messages.front().input_write_completed);
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.Chat().id));
}
