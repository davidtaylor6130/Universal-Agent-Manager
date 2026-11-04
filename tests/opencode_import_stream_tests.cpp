#include "test_harness.h"
#include "app/opencode_import_stream.h"
#include "app/startup_native_import_service.h"

using namespace uam_test;

namespace
{
	struct OpenCodeExportFixture
	{
		TempDir temp{"uam-opencode-stream"};
		fs::path workspace = temp.root / "private workspace";
		ProviderProfile profile = ProviderProfileStore::DefaultOpenCodeProfile();
		ChatSession chat;
		nlohmann::json document;
		std::string text = std::string(512 * 1024, 'x') + " final UTF8 \xc3\xa9 escaped \"\\{}[]";

		OpenCodeExportFixture()
		{
			fs::create_directories(workspace);
			chat.id = "private-import-fixture";
			chat.folder_id = "workspace";
			chat.title = "Private imported history";
			chat.created_at = "2026-08-28T21:14:26.000Z";
			chat.updated_at = chat.created_at;
			chat.provider_id = uam::provider_ids::kOpenCodeCli;
			chat.native_session_id = "ses_stream_fixture";
			chat.workspace_directory = uam::paths::Utf8PathString(workspace);
			SetExitCode(0);
			document = {{"info", {{"id", chat.native_session_id}, {"directory", chat.workspace_directory}}}, {"messages", nlohmann::json::array()}};
			for (int index = 0; index < 12; ++index)
			{
				document["messages"].push_back({
				    {"info", {{"role", "assistant"}, {"providerID", "openai"}, {"modelID", "fixture"}, {"time", {{"created", 1787951666685}, {"completed", 1787951668185}}}}},
				    {"parts", nlohmann::json::array({{{"type", "reasoning"}, {"text", "Private reasoning"}},
				        {{"type", "tool"}, {"callID", "call-" + std::to_string(index)}, {"tool", "bash"}, {"state", {{"status", "completed"}, {"input", {{"command", "fixture"}}}, {"output", "Private result"}}}},
				        {{"type", "text"}, {"text", text + std::to_string(index)}}})}});
			}
			Write();
		}

		void SetTaskChild(const std::string& child_id)
		{
			// OpenCode task exports carry their exact child session in state.metadata.
			auto& tool = document["messages"][0]["parts"][1];
			tool["tool"] = "task";
			tool["state"]["input"] = {{"description", "Inspect provider"}, {"prompt", "Inspect the provider."}, {"subagent_type", "general"}};
			tool["state"]["metadata"] = {{"sessionId", child_id}};
			Write();
		}

		void SetExitCode(int code)
		{
#if defined(_WIN32)
			const fs::path executable = temp.root / "fixture.cmd";
			const std::string script = "@echo off\r\ntype export.json\r\nexit /b " + std::to_string(code) + "\r\n";
#else
			const fs::path executable = temp.root / "fixture";
			const std::string script = "#!/bin/sh\n/bin/cat export.json\nexit " + std::to_string(code) + "\n";
#endif
			UAM_ASSERT(uam::io::WriteTextFile(executable, script));
			fs::permissions(executable, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
			profile.interactive_command = uam::shell::JoinEscapedArgs({uam::paths::Utf8PathString(executable)});
		}

		uam::opencode_import::Spool Export()
		{
			uam::ProviderWorkerInvocation invocation;
			invocation.direct_process = true;
			invocation.argv = ProviderRuntimeRegistry::Resolve(profile).BuildNativeExportArgv(profile, chat);
			std::string error;
			auto source = uam::opencode_import::Export(std::move(invocation), workspace, {}, error);
			UAM_ASSERT(source && error.empty());
			return source;
		}

		void Write()
		{
			UAM_ASSERT(uam::io::WriteTextFile(workspace / "export.json", document.dump()));
			UAM_ASSERT(fs::file_size(workspace / "export.json") > uam::platform::kCapturedCommandMaxOutputBytes);
		}
	};
}

UAM_TEST(LocalOpenCodeStreamingExportPreservesCompleteLargeTranscript)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	OpenCodeExportFixture fixture;
	const auto loaded = ChatHistorySyncService().LoadLocalOpenCodeTranscript(fixture.chat, fixture.profile);
	UAM_ASSERT(loaded.success);
	UAM_ASSERT_EQ(loaded.session_id, fixture.chat.native_session_id);
	UAM_ASSERT_EQ(loaded.directory, fixture.chat.workspace_directory);
	UAM_ASSERT_EQ(loaded.messages.size(), std::size_t{12});
	for (std::size_t index = 0; index < loaded.messages.size(); ++index)
	{
		const Message& message = loaded.messages[index];
		UAM_ASSERT_EQ(message.content, fixture.text + std::to_string(index));
		UAM_ASSERT_EQ(message.created_at, std::string("2026-08-28T21:14:26.000Z"));
		UAM_ASSERT_EQ(message.processing_time_ms, 1500);
		UAM_ASSERT_EQ(message.model_id, std::string("openai/fixture"));
		UAM_ASSERT_EQ(message.thoughts, std::string("Private reasoning"));
		UAM_ASSERT_EQ(message.tool_calls.size(), std::size_t{1});
		UAM_ASSERT_EQ(message.tool_calls[0].id, "call-" + std::to_string(index));
		UAM_ASSERT_EQ(message.tool_calls[0].args_json, std::string("{\"command\":\"fixture\"}"));
		UAM_ASSERT_EQ(message.tool_calls[0].result_text, std::string("Private result"));
		UAM_ASSERT_EQ(message.blocks.size(), std::size_t{3});
	}
#endif
}

UAM_TEST(LocalOpenCodeStreamingExportRejectsWholeFailedSession)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	OpenCodeExportFixture fixture;
	const auto reject = [&](std::stop_token stop = {})
	{
		const auto loaded = ChatHistorySyncService().LoadLocalOpenCodeTranscript(fixture.chat, fixture.profile, stop);
		UAM_ASSERT(!loaded.success);
		UAM_ASSERT(loaded.messages.empty());
		UAM_ASSERT(!loaded.error.empty());
	};
	fixture.document["messages"].back()["parts"].push_back({{"type", "text"}, {"text", 42}});
	fixture.Write();
	reject();
	fixture.document["messages"].back()["parts"].erase(fixture.document["messages"].back()["parts"].size() - 1);
	fixture.Write();
	fixture.SetExitCode(7);
	reject();
	fixture.SetExitCode(0);
	std::stop_source canceled;
	canceled.request_stop();
	reject(canceled.get_token());
	fixture.document["info"]["id"] = "ses_foreign";
	fixture.Write();
	reject();
	fixture.document["info"]["id"] = fixture.chat.native_session_id;
	fixture.document["info"]["directory"] = uam::paths::Utf8PathString(fixture.temp.root / "foreign");
	fixture.Write();
	reject();
#endif
}

UAM_TEST(OpenCodeStreamSpoolOwnershipAndStagingFailure)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	OpenCodeExportFixture fixture;
	auto source = fixture.Export();
	const fs::path path = *source;
	std::string error;
	std::stop_source stopped;
	std::size_t emitted = 0;
	UAM_ASSERT(!uam::opencode_import::Read(path, fixture.chat, [&](const Message&)
	{
		++emitted;
		stopped.request_stop();
		return true;
	}, stopped.get_token(), error));
	UAM_ASSERT_EQ(emitted, std::size_t{1});
	UAM_ASSERT(!error.empty());
	fixture.document["messages"].back()["parts"].push_back({{"type", "text"}, {"text", 42}});
	fixture.Write();
	auto malformed = fixture.Export();
	ChatSession metadata = fixture.chat;
	metadata.messages_loaded = false;
	const fs::path data = fixture.temp.root / "private data";
	const auto prepared = ChatRepository::PrepareImportedChatSave(data, metadata, [&](const std::function<bool(const Message&)>& emit, ChatSession& final)
	{
		return uam::opencode_import::Read(*malformed, final, emit, {}, error);
	});
	UAM_ASSERT(!prepared);
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(data, metadata.id)));
	const fs::path blocked = fixture.temp.root / "blocked-data";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "storage is a file"));
	UAM_ASSERT(!ChatRepository::PrepareImportedChatSave(blocked, metadata, [&](const std::function<bool(const Message&)>& emit, ChatSession& final)
	{
		return uam::opencode_import::Read(path, final, emit, {}, error);
	}));
	source.reset();
	UAM_ASSERT(!fs::exists(path.parent_path()));
#endif
}

UAM_TEST(OpenCodeStreamWorkerRejectsFailedSinkAndPreservesCaptureLimit)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	OpenCodeExportFixture fixture;
	uam::ProviderWorkerInvocation invocation;
	invocation.direct_process = true;
	invocation.argv = ProviderRuntimeRegistry::Resolve(fixture.profile).BuildNativeExportArgv(fixture.profile, fixture.chat);
	std::size_t writes = 0;
	invocation.output_sink = [&](std::string_view) { ++writes; return false; };
	const ProcessExecutionResult refused = uam::ExecuteProviderWorkerInvocation(invocation, fixture.workspace, 30000);
	UAM_ASSERT(!refused.ok && !refused.error.empty());
	UAM_ASSERT_EQ(writes, std::size_t{1});
	invocation.output_sink = {};
	const ProcessExecutionResult captured = uam::ExecuteProviderWorkerInvocation(invocation, fixture.workspace, 30000);
	UAM_ASSERT(!captured.ok && captured.output_truncated);
	UAM_ASSERT_EQ(captured.output.size(), uam::platform::kCapturedCommandMaxOutputBytes);
#endif
}

UAM_TEST(OpenCodeStreamBatchPreservesExistingRefreshAndCompleteStorage)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	OpenCodeExportFixture fixture;
	const std::string child_id = "ses_stream_fixture_child";
	fixture.SetTaskChild(child_id);
	uam::AppState app;
	app.data_root = fixture.temp.root / "private data";
	app.folders.push_back({"workspace", "Private", fixture.chat.workspace_directory, false, "local"});
	const auto import = [&](uam::opencode_import::Spool source)
	{
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		ChatSession summary = fixture.chat;
		summary.messages_loaded = false;
		discovery.open_code_exports.emplace(uam::chat_identity::NativeIdentityKeyForHistoryImport(summary), std::move(source));
		discovery.chats.push_back(std::move(summary));
		ChatHistorySyncService history;
		int imported = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		do
		{
			const auto result = history.ImportDiscoveredProviderChatsBatch(app, app.folders.front(), discovery);
			UAM_ASSERT(result.errors.empty());
			imported += result.imported_count;
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			std::this_thread::yield();
		} while (discovery.Pending());
		UAM_ASSERT_EQ(imported, 1);
	};
	import(fixture.Export());
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT(!app.chats.front().messages_loaded);
	auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.chat.id, true);
	UAM_ASSERT(saved && saved->messages.size() == 12);
	UAM_ASSERT_EQ(saved->messages.back().content, fixture.text + "11");
	UAM_ASSERT_EQ(saved->messages.front().tool_calls.size(), std::size_t{1});
	const ToolCall& imported_child = saved->messages.front().tool_calls.front();
	UAM_ASSERT_EQ(imported_child.id, std::string("call-0"));
	UAM_ASSERT_EQ(imported_child.name, std::string("task"));
	UAM_ASSERT(imported_child.is_sub_agent);
	UAM_ASSERT_EQ(imported_child.sub_agent_id, child_id);
	UAM_ASSERT(!imported_child.sub_agent_children_authoritative);
	UAM_ASSERT(imported_child.sub_agent_children.empty());
	saved->pinned = true;
	saved->title = "Keep local title";
	saved->messages.front().tool_calls.front().sub_agent_title = "Keep local tool detail";
	saved->messages.front().tool_calls.front().sub_agent_children = {{child_id, "Keep local tool detail", "running"}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *saved));
	app.chats.front() = *saved;
	fixture.document["messages"].push_back({{"info", {{"role", "user"}, {"time", {{"created", 1787951669000}}}}}, {"parts", nlohmann::json::array({{{"type", "text"}, {"text", "new tail"}}})}});
	fixture.Write();
	fixture.chat.updated_at = "2026-08-28T21:14:29.000Z";
	import(fixture.Export());
	saved = ChatRepository::LoadLocalChat(app.data_root, fixture.chat.id, true);
	UAM_ASSERT(saved && saved->messages.size() == 13);
	UAM_ASSERT_EQ(saved->messages.back().content, std::string("new tail"));
	UAM_ASSERT(saved->pinned);
	UAM_ASSERT_EQ(saved->title, std::string("Keep local title"));
	UAM_ASSERT_EQ(saved->messages.front().tool_calls.front().sub_agent_title, std::string("Keep local tool detail"));
	const ToolCall& refreshed_child = saved->messages.front().tool_calls.front();
	UAM_ASSERT_EQ(refreshed_child.id, std::string("call-0"));
	UAM_ASSERT_EQ(refreshed_child.name, std::string("task"));
	UAM_ASSERT(refreshed_child.is_sub_agent);
	UAM_ASSERT_EQ(refreshed_child.sub_agent_id, child_id);
	UAM_ASSERT(!refreshed_child.sub_agent_children_authoritative);
	const std::vector<SubAgentChild> expected_children = {{child_id, "Keep local tool detail", "running"}};
	UAM_ASSERT_EQ(refreshed_child.sub_agent_children, expected_children);
	for (std::size_t index = 0; index < 12; ++index)
	{
		UAM_ASSERT_EQ(saved->messages[index].content, fixture.text + std::to_string(index));
		UAM_ASSERT_EQ(saved->messages[index].thoughts, std::string("Private reasoning"));
		UAM_ASSERT_EQ(saved->messages[index].tool_calls.size(), std::size_t{1});
		UAM_ASSERT_EQ(saved->messages[index].tool_calls.front().id, "call-" + std::to_string(index));
		UAM_ASSERT_EQ(saved->messages[index].tool_calls.front().result_text, std::string("Private result"));
	}
	// Omission above preserves the matching child; an authoritative rejection must not.
	ChatSession rejected = *saved;
	ToolCall& rejected_update = rejected.messages.front().tool_calls.front();
	rejected_update.sub_agent_id.clear();
	rejected_update.sub_agent_title.clear();
	rejected_update.sub_agent_children.clear();
	rejected_update.sub_agent_children_authoritative = true;
	ChatHistorySyncService().OverlayPreparedLocalHistory(*saved, rejected);
	const ToolCall& rejected_child = rejected.messages.front().tool_calls.front();
	UAM_ASSERT(rejected_child.sub_agent_children_authoritative);
	UAM_ASSERT(rejected_child.sub_agent_id.empty());
	UAM_ASSERT(rejected_child.sub_agent_title.empty());
	UAM_ASSERT(rejected_child.sub_agent_children.empty());
#endif
}

UAM_TEST(OpenCodeStreamStartupSelectionRetainsOwnedSpool)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	for (bool selected : {false, true})
	{
		OpenCodeExportFixture fixture;
		uam::AppState app;
		app.data_root = fixture.temp.root / "private data";
		app.folders.push_back({"workspace", "Private", fixture.chat.workspace_directory, false, "local"});
		auto source = fixture.Export();
		const fs::path path = *source;
		uam::StartupNativeImportService service([&source, chat = fixture.chat](const uam::StartupNativeImportScanRequest&, std::stop_token)
		{
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			ChatSession metadata = chat;
			metadata.messages_loaded = false;
			discovery.open_code_exports.emplace(uam::chat_identity::NativeIdentityKeyForHistoryImport(metadata), std::move(source));
			discovery.chats.push_back(std::move(metadata));
			return discovery;
		});
		service.Initialize(app);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (service.Snapshot().phase == "scanning")
		{
			service.Poll(app);
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			std::this_thread::yield();
		}
		const auto ready = service.Snapshot();
		UAM_ASSERT_EQ(ready.candidates.size(), std::size_t{1});
		UAM_ASSERT(fs::exists(path));
		if (!selected)
		{
			service.Cancel();
			UAM_ASSERT(!fs::exists(path.parent_path()));
			UAM_ASSERT(app.chats.empty());
			continue;
		}
		std::string error;
		UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}, &error));
		while (service.ImportPending())
		{
			service.Poll(app);
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			std::this_thread::yield();
		}
		UAM_ASSERT(service.Snapshot().errors.empty());
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.chat.id, true);
		UAM_ASSERT(saved && saved->messages.size() == 12);
		UAM_ASSERT_EQ(saved->messages.back().content, fixture.text + "11");
		UAM_ASSERT(!fs::exists(path.parent_path()));
	}
#endif
}

UAM_TEST(OpenCodeStructuredExportSeparatesDiagnosticOutput)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	OpenCodeExportFixture fixture;
#if defined(_WIN32)
	const fs::path executable = fixture.temp.root / "fixture.cmd";
	const std::string script = "@echo off\r\ntype export.json\r\necho diagnostic after valid JSON 1>&2\r\nexit /b 0\r\n";
#else
	const fs::path executable = fixture.temp.root / "fixture";
	const std::string script = "#!/bin/sh\n/bin/cat export.json\nprintf 'diagnostic after valid JSON\\n' >&2\n";
#endif
	UAM_ASSERT(uam::io::WriteTextFile(executable, script));
	fs::permissions(executable, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
	const ChatHistorySyncService::RemoteOpenCodeTranscript loaded = ChatHistorySyncService().LoadLocalOpenCodeTranscript(fixture.chat, fixture.profile);
	UAM_ASSERT(loaded.success);
	UAM_ASSERT_EQ(loaded.messages.size(), std::size_t{12});
	UAM_ASSERT_EQ(loaded.messages.back().content, fixture.text + "11");
#endif
}

UAM_TEST(OpenCodeStructuredExportDrainsDiagnosticBackpressure)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	OpenCodeExportFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.workspace / "diagnostic.txt", std::string(5 * 1024 * 1024, 'd')));
#if defined(_WIN32)
	const fs::path executable = fixture.temp.root / "fixture.cmd";
	const std::string script = "@echo off\r\ntype diagnostic.txt 1>&2\r\ntype export.json\r\nexit /b 0\r\n";
#else
	const fs::path executable = fixture.temp.root / "fixture";
	const std::string script = "#!/bin/sh\n/bin/cat diagnostic.txt >&2\n/bin/cat export.json\n";
#endif
	UAM_ASSERT(uam::io::WriteTextFile(executable, script));
	fs::permissions(executable, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
	const ChatHistorySyncService::RemoteOpenCodeTranscript loaded = ChatHistorySyncService().LoadLocalOpenCodeTranscript(fixture.chat, fixture.profile);
	UAM_ASSERT(loaded.success);
	UAM_ASSERT_EQ(loaded.messages.size(), std::size_t{12});
	UAM_ASSERT_EQ(loaded.messages.back().content, fixture.text + "11");
#endif
}
