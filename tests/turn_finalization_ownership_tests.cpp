#include "test_harness.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"
#include <future>
#include <cstdio>
#include "common/platform/async_byte_writer.h"
#if defined(_WIN32)
#include <io.h>
#endif

using namespace uam_test;

UAM_TEST(CheckpointCompletionCannotFinalizeANewerTurnOrActiveCompaction)
{
	for (const std::string activity : {"new-turn", "compaction", "unchanged"})
	{
		TempDir temp("uam-checkpoint-finalization-owner");
		uam::AppState app;
		app.data_root = temp.root / "data";
		ChatSession chat;
		chat.id = "checkpoint-owner";
		chat.provider_id = "codex-cli";
		chat.messages.push_back({MessageRole::Assistant, "Completed earlier turn.", "earlier-turn"});
		app.chats.push_back(chat);
		std::string goal_id;
		UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Complete the provider goal.", 0, &goal_id, "provider", "/goal"));
		UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
		std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat.id;
		session->provider_id = chat.provider_id;
		session->running = true;
		session->session_ready = true;
		session->turn_serial = 1;
		session->goal_command_revision = app.chats.front().goal_command_revision;
		session->turn_checkpoint_commit_pending = true;
		app.acp_sessions.push_back(std::move(session));
		uam::AsyncTurnCheckpointTask task;
		task.kind = uam::AsyncTurnCheckpointTaskKind::Commit;
		task.chat_id = chat.id;
		task.turn_serial = 1;
		task.assistant_message_index = 0;
		task.expected_message_created_at = "earlier-turn";
		task.state = std::make_shared<uam::AsyncTurnCheckpointState>();
		task.state->ok = true;
		task.state->changed = true;
		task.state->checkpoint_sha = "previous-turn-checkpoint";
		std::promise<void> finish_checkpoint;
		const std::shared_ptr<uam::AsyncTurnCheckpointState> completion_state = task.state;
		task.worker = std::make_unique<std::jthread>([completion_state, ready = finish_checkpoint.get_future()]() mutable
		{
			ready.get();
			completion_state->finished.store(true, std::memory_order_release);
		});
		app.turn_checkpoint_tasks.push_back(std::move(task));
		app.worktree_operation_chat_ids.insert(chat.id);
		app.acp_sessions.front()->turn_serial = activity == "new-turn" ? 2 : 1;
		app.acp_sessions.front()->processing = activity != "unchanged";
		app.acp_sessions.front()->manual_compaction_pending = activity == "compaction";
		finish_checkpoint.set_value();
		app.turn_checkpoint_tasks.front().worker->join();
		UAM_ASSERT(uam::acp_detail::PollTurnCheckpointTasks(app, nullptr));
		UAM_ASSERT(app.turn_checkpoint_tasks.empty());
		UAM_ASSERT_EQ(app.chats.front().messages.front().checkpoint_sha, std::string("previous-turn-checkpoint"));
		UAM_ASSERT_EQ(app.chats.front().goals.front().status, activity == "unchanged" ? GoalStatus::Complete : GoalStatus::Active);
		UAM_ASSERT_EQ(app.acp_sessions.front()->turn_checkpoint_commit_pending, activity != "unchanged");
		UAM_ASSERT_EQ(app.acp_sessions.front()->processing, activity != "unchanged");
		UAM_ASSERT_EQ(app.acp_sessions.front()->manual_compaction_pending, activity == "compaction");
	}
}

UAM_TEST(AsyncCheckpointKeepsNewQueuedIntentDurableAndRetriesOwnedMetadataBeforeDispatch)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-checkpoint-outbox-owner");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "file.txt", "before\n"));
	UAM_ASSERT(RunGitForTest(repo, "add -A"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
	app.runtime_cli_versions_by_provider_id["codex-cli"].checked = true;
	app.runtime_cli_versions_by_provider_id["codex-cli"].supported = true;
	app.runtime_cli_versions_by_provider_id["codex-cli"].installed_version = "0.160.0";
	app.folders.push_back({"folder", "Workspace", repo.string(), false});
	ChatSession chat;
	chat.id = "checkpoint-queued-intent";
	chat.provider_id = "codex-cli";
	chat.folder_id = "folder";
	chat.workspace_directory = repo.string();
	chat.memory_enabled = false;
	chat.messages = {{MessageRole::User, "Change file.", "user-before"}, {MessageRole::Assistant, "Changed.", "assistant-before"}};
	app.chats.push_back(chat);
	UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, app.chats.front()).ok);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	uam::AppState snapshot;
	snapshot.data_root = app.data_root;
	snapshot.folders = app.folders;
	snapshot.provider_profiles = app.provider_profiles;
	snapshot.chats.push_back(app.chats.front());
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = "codex-cli";
	session->running = true;
	session->session_ready = true;
	session->session_id = "fixture-thread";
	session->codex_thread_id = "fixture-thread";
	session->turn_serial = 1;
	session->turn_checkpoint_commit_pending = true;
	app.acp_sessions.push_back(std::move(session));
	uam::AcpSessionState& current = *app.acp_sessions.front();
	std::string captured;
	std::FILE* transport = std::tmpfile();
	UAM_ASSERT(transport != nullptr);
	struct TransportCleanup
	{
		uam::AcpSessionState& session;
		std::FILE* file;
		~TransportCleanup()
		{
			session.stdin_writer.reset();
#if defined(_WIN32)
			session.stdin_write = INVALID_HANDLE_VALUE;
#else
			session.stdin_write_fd = -1;
#endif
			std::fclose(file);
		}
	} cleanup{current, transport};
#if defined(_WIN32)
	current.stdin_write = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(transport)));
#else
	current.stdin_write_fd = fileno(transport);
#endif
	current.stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
	    [&captured](const char* bytes, std::size_t count, std::string&) -> std::ptrdiff_t
	    { captured.append(bytes, count); return static_cast<std::ptrdiff_t>(count); }, [] {});
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "Next intended turn.", &error));
	UAM_ASSERT_EQ(current.turn_serial, 1);
	UAM_ASSERT(!current.processing);
	UAM_ASSERT_EQ(current.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(current.stdin_writer->Flush(std::chrono::seconds(1)));
	UAM_ASSERT(captured.empty());
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(app, current, app.chats.front()));
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(uam::io::WriteTextFile(fs::path(snapshot.chats.front().workspace_worktree_directory) / "file.txt", "after\n"));
	const uam::GitTurnCheckpointResult result = uam::GitWorktreeService().CreateTurnCheckpoint(snapshot, snapshot.chats.front(), 1, {}, false);
	UAM_ASSERT(result.ok && result.changed);
	std::vector<ChatSession> durable = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(durable.front().acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(durable.front().acp_queued_prompts.front().text, std::string("Next intended turn."));
	UAM_ASSERT(durable.front().messages[1].checkpoint_sha.empty());
	uam::AsyncTurnCheckpointTask task;
	task.kind = uam::AsyncTurnCheckpointTaskKind::Commit;
	task.chat_id = chat.id;
	task.turn_serial = 1;
	task.assistant_message_index = 1;
	task.expected_message_created_at = "assistant-before";
	task.state = std::make_shared<uam::AsyncTurnCheckpointState>();
	task.state->ok = true;
	task.state->changed = true;
	task.state->checkpoint_sha = result.checkpoint_sha;
	task.state->parent_sha = result.parent_sha;
	task.state->finished.store(true);
	app.turn_checkpoint_tasks.push_back(std::move(task));
	app.worktree_operation_chat_ids.insert(chat.id);
	const fs::path durable_root = app.data_root;
	app.data_root = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "blocked"));
	UAM_ASSERT(uam::acp_detail::PollTurnCheckpointTasks(app, nullptr));
	UAM_ASSERT_EQ(app.turn_checkpoint_tasks.size(), std::size_t{1});
	UAM_ASSERT(current.turn_checkpoint_commit_pending);
	UAM_ASSERT(!current.processing && captured.empty());
	UAM_ASSERT(app.chats.front().messages[1].checkpoint_sha.empty());
	app.data_root = durable_root;
	UAM_ASSERT(uam::acp_detail::PollTurnCheckpointTasks(app, nullptr));
	UAM_ASSERT_EQ(app.turn_checkpoint_tasks.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.turn_checkpoint_tasks.front().kind, uam::AsyncTurnCheckpointTaskKind::Preflight);
	UAM_ASSERT_EQ(app.turn_checkpoint_tasks.front().turn_serial, 2);
	UAM_ASSERT(app.turn_checkpoint_tasks.front().worker != nullptr);
	app.turn_checkpoint_tasks.front().worker->join();
	UAM_ASSERT(uam::acp_detail::PollTurnCheckpointTasks(app, nullptr));
	UAM_ASSERT(app.turn_checkpoint_tasks.empty());
	UAM_ASSERT(!current.turn_checkpoint_commit_pending);
	UAM_ASSERT(current.processing);
	UAM_ASSERT_EQ(current.turn_serial, 2);
	UAM_ASSERT(current.queued_user_prompts.empty());
	const std::chrono::steady_clock::time_point delivery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (current.prompt_request_id == 0 && current.processing && std::chrono::steady_clock::now() < delivery_deadline)
	{
		uam::FlushPendingChatSaves(app);
		(void)uam::acp_detail::SendQueuedPromptIfReady(app, current, app.chats.front());
		std::this_thread::yield();
	}
	UAM_ASSERT(current.prompt_request_id != 0);
	UAM_ASSERT(current.stdin_writer->Flush(std::chrono::seconds(1)));
	UAM_ASSERT(captured.find("turn/start") != std::string::npos);
	durable = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(durable.front().messages[1].checkpoint_sha, result.checkpoint_sha);
	UAM_ASSERT_EQ(durable.front().messages.back().content, std::string("Next intended turn."));
	const std::string once = captured;
	UAM_ASSERT(!uam::acp_detail::PollTurnCheckpointTasks(app, nullptr));
	UAM_ASSERT_EQ(captured, once);
#endif
}
