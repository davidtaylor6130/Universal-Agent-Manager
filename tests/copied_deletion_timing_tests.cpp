#include "test_harness.h"
#include "app/chat_lifecycle_service.h"

using namespace uam_test;

namespace
{
	using DeletionClock = std::chrono::steady_clock;

	double DeletionMilliseconds(DeletionClock::time_point started)
	{
		return std::chrono::duration<double, std::milli>(DeletionClock::now() - started).count();
	}

	void CheckCopiedDeletionTiming(bool delete_folder)
	{
		const std::shared_ptr<TempDir> temp = std::make_shared<TempDir>("uam-copied-deletion-timing");
		const std::shared_ptr<ScopedEnvVar> provider_home = std::make_shared<ScopedEnvVar>("CODEX_HOME", (temp->root / "provider-home").string());
		const fs::path generated = temp->root / "generated-workspace";
		const fs::path copied = temp->root / "copied-workspace";
		fs::create_directories(generated);
		const std::string payload(256 * 1024, 'W');
		for (int index = 0; index < 32; ++index)
			UAM_ASSERT(uam::io::WriteTextFile(generated / ("resource-" + std::to_string(index) + ".bin"), payload));
		const DeletionClock::time_point copy_started = DeletionClock::now();
		fs::copy(generated, copied, fs::copy_options::recursive);
		const double copy_ms = DeletionMilliseconds(copy_started);

		uam::AppState app;
		app.data_root = temp->root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		app.folders = {{"delete", "Delete", copied.string(), false}, {"keep", "Keep", copied.string(), false}};
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		ChatSession doomed;
		doomed.id = "copied-doomed"; doomed.branch_root_chat_id = doomed.id; doomed.folder_id = "delete";
		doomed.provider_id = uam::provider_ids::kCodexCli;
		doomed.workspace_directory = copied.string();
		const std::string message(4096, 'T');
		for (int index = 0; index < 2048; ++index) doomed.messages.push_back({MessageRole::User, message});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, doomed));
		ChatSession survivor;
		survivor.id = "copied-survivor"; survivor.branch_root_chat_id = survivor.id; survivor.folder_id = "keep";
		survivor.provider_id = uam::provider_ids::kCodexCli;
		survivor.workspace_directory = copied.string();
		survivor.messages.push_back({MessageRole::User, "untouched survivor transcript"});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, survivor));
		ChatSession second_survivor;
		second_survivor.id = "copied-survivor-two"; second_survivor.branch_root_chat_id = second_survivor.id; second_survivor.folder_id = "keep";
		second_survivor.provider_id = uam::provider_ids::kCodexCli;
		second_survivor.workspace_directory = copied.string();
		second_survivor.messages.push_back({MessageRole::User, "independent survivor transcript"});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, second_survivor));
		const fs::path survivor_path = AppPaths::UamChatFilePath(app.data_root, survivor.id);
		const std::string survivor_bytes = ReadFile(survivor_path);
		const fs::path second_survivor_path = AppPaths::UamChatFilePath(app.data_root, second_survivor.id);
		const std::string second_survivor_bytes = ReadFile(second_survivor_path);
		const fs::path foreign = app.data_root / "foreign-owner" / "keep.bin";
		UAM_ASSERT(uam::io::WriteTextFile(foreign, "foreign storage survivor"));
		std::optional<ChatSession> cold = ChatRepository::LoadLocalChat(app.data_root, doomed.id, false);
		UAM_ASSERT(cold && !cold->messages_loaded && cold->messages.empty());
		app.chats = {std::move(*cold), survivor, second_survivor};
		app.selected_chat_index = 0;
		const std::function<void()> check_survivors = [&]()
		{
			std::uintmax_t fixture_bytes = 0;
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(temp->root))
				if (entry.is_regular_file()) fixture_bytes += entry.file_size();
			UAM_ASSERT(fixture_bytes < 50ULL * 1024 * 1024);
			UAM_ASSERT_EQ(ReadFile(survivor_path), survivor_bytes);
			UAM_ASSERT_EQ(ReadFile(second_survivor_path), second_survivor_bytes);
			UAM_ASSERT_EQ(ReadFile(foreign), "foreign storage survivor");
			for (int index = 0; index < 32; ++index)
			{
				UAM_ASSERT_EQ(ReadFile(generated / ("resource-" + std::to_string(index) + ".bin")), payload);
				UAM_ASSERT_EQ(ReadFile(copied / ("resource-" + std::to_string(index) + ".bin")), payload);
			}
		};

		const std::shared_ptr<uam::WorkspaceDeletionTask> task = std::make_shared<uam::WorkspaceDeletionTask>();
		const DeletionClock::time_point prepare_started = DeletionClock::now();
		const bool prepared = delete_folder ? uam::PrepareWorkspaceDeletion(app, {"delete"}, *task) : uam::PrepareChatDeletion(app, {doomed.id}, *task);
		const double prepare_ms = DeletionMilliseconds(prepare_started);
		UAM_ASSERT(prepared && prepare_ms < 250.0);
		UAM_ASSERT(task->snapshot.chats.size() == 1 && !task->snapshot.chats.front().messages_loaded && task->snapshot.chats.front().messages.empty());
		check_survivors();

		double stage_ms = 0.0, cleanup_ms = 0.0, input_max_ms = 0.0;
		int input_calls = 0;
		const std::function<void()> select_survivor = [&]()
		{
			const DeletionClock::time_point input_started = DeletionClock::now();
			const std::string target = ChatDomainService().SelectedChatId(app) == survivor.id ? second_survivor.id : survivor.id;
			ChatDomainService().SelectChatById(app, target);
			const double input_ms = DeletionMilliseconds(input_started);
			input_max_ms = std::max(input_max_ms, input_ms); ++input_calls;
			UAM_ASSERT(input_ms < 100.0);
		};
		struct PhaseResult { bool ok; double milliseconds; };
		const std::function<std::future<PhaseResult>(bool)> start_worker = [task, temp, provider_home](bool cleanup)
		{
			std::packaged_task<PhaseResult()> work([task, temp, provider_home, cleanup]()
			{
				const DeletionClock::time_point started = DeletionClock::now();
				const bool ok = cleanup ? uam::CleanupWorkspaceDeletion(*task) : uam::StageWorkspaceDeletion(*task);
				return PhaseResult{ok, DeletionMilliseconds(started)};
			});
			std::future<PhaseResult> result = work.get_future();
			std::thread(std::move(work)).detach();
			return result;
		};
		const std::function<PhaseResult(std::future<PhaseResult>&)> wait_worker = [&](std::future<PhaseResult>& worker)
		{
			// Test-hang protection only; worker completion has no product latency acceptance ceiling.
			const DeletionClock::time_point deadline = DeletionClock::now() + std::chrono::seconds(30);
			select_survivor();
			while (worker.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
			{
				UAM_ASSERT(DeletionClock::now() < deadline);
				select_survivor();
			}
			return worker.get();
		};
		std::future<PhaseResult> stage = start_worker(false);
		const PhaseResult staged = wait_worker(stage);
		UAM_ASSERT(staged.ok); stage_ms = staged.milliseconds;
		check_survivors();

		const std::string expected_selected = ChatDomainService().SelectedChatId(app);
		const DeletionClock::time_point commit_started = DeletionClock::now();
		uam::CommitWorkspaceDeletion(app, *task);
		const double commit_ms = DeletionMilliseconds(commit_started);
		UAM_ASSERT(!task->commit_rejected && task->metadata_saved && commit_ms < 250.0);
		UAM_ASSERT(ChatDomainService().SelectedChatId(app) == expected_selected);
		UAM_ASSERT(ChatDomainService().FindChatById(app, doomed.id) == nullptr);
		check_survivors();

		std::future<PhaseResult> cleanup = start_worker(true);
		const PhaseResult cleaned = wait_worker(cleanup);
		UAM_ASSERT(cleaned.ok); cleanup_ms = cleaned.milliseconds;
		check_survivors();
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, doomed.id)));
		UAM_ASSERT(!fs::exists(app.data_root / "deletion-transaction.json"));
		UAM_ASSERT(!fs::exists(app.data_root / ".deletion-transaction"));
		if (delete_folder) UAM_ASSERT(app.folders.size() == 1 && app.folders.front().id == "keep");
		std::printf("[copied deletion %s] copy=%.3fms prepare_ui=%.3fms stage_worker=%.3fms commit_ui=%.3fms cleanup_worker=%.3fms select_input_max=%.3fms input_calls=%d\n",
		    delete_folder ? "workspace" : "chat", copy_ms, prepare_ms, stage_ms, commit_ms, cleanup_ms, input_max_ms, input_calls);
	}
}

UAM_TEST(CopiedChatDeletionKeepsUiResponsiveAndSurvivorBytes)
{
	CheckCopiedDeletionTiming(false);
}

UAM_TEST(CopiedWorkspaceDeletionKeepsUiResponsiveAndSurvivorBytes)
{
	CheckCopiedDeletionTiming(true);
}
