#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/settings_store.h"
#include "common/constants/app_constants.h"
#include "common/paths/app_paths.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/io_utils.h"
#include "core/gemini_cli_compat.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	struct TestCase
	{
		std::string name;
		std::function<void()> fn;
	};

	std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> tests;
		return tests;
	}

	struct RegisterTest
	{
		RegisterTest(std::string name, std::function<void()> fn)
		{
			Registry().push_back({std::move(name), std::move(fn)});
		}
	};

	struct TestFailure : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	void Fail(const char* expr, const char* file, int line)
	{
		std::ostringstream out;
		out << file << ':' << line << ": assertion failed: " << expr;
		throw TestFailure(out.str());
	}

	template <typename T, typename U>
	void AssertEq(const T& actual, const U& expected, const char* actual_expr, const char* expected_expr, const char* file, int line)
	{
		if (!(actual == expected))
		{
			std::ostringstream out;
			out << file << ':' << line << ": expected " << actual_expr << " == " << expected_expr;
			throw TestFailure(out.str());
		}
	}

	struct TempDir
	{
		fs::path root;

		explicit TempDir(const std::string& prefix)
		{
			const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
			root = fs::temp_directory_path() / (prefix + "-" + std::to_string(ticks));
			fs::create_directories(root);
		}

		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(root, ec);
		}
	};

	std::string ReadFile(const fs::path& path)
	{
		return uam::io::ReadTextFile(path);
	}
}

#define UAM_TEST(name) \
	static void name(); \
	static RegisterTest register_##name(#name, name); \
	static void name()

#define UAM_ASSERT(expr) \
	do { if (!(expr)) Fail(#expr, __FILE__, __LINE__); } while (false)

#define UAM_ASSERT_EQ(actual, expected) \
	do { AssertEq((actual), (expected), #actual, #expected, __FILE__, __LINE__); } while (false)

UAM_TEST(SettingsStoreLoadsLegacyButWritesReleaseSliceOnly)
{
	TempDir temp("uam-settings");
	const fs::path settings_file = temp.root / "settings.txt";
	UAM_ASSERT(uam::io::WriteTextFile(settings_file,
		"active_provider_id=codex-cli\n"
		"gemini_command_template=gemini -p {prompt}\n"
		"gemini_yolo_mode=1\n"
		"gemini_extra_flags=--approval-mode yolo\n"
		"selected_model_id=legacy-model.gguf\n"
		"vector_db_backend=ollama-engine\n"
		"prompt_profile_root_path=/tmp/templates\n"
		"rag_enabled=1\n"
		"cli_idle_timeout_seconds=9999\n"
		"ui_theme=system\n"
		"last_selected_chat_id=chat-1\n"));

	AppSettings settings;
	CenterViewMode mode = CenterViewMode::CliConsole;
	SettingsStore::Load(settings_file, settings, mode);

	UAM_ASSERT_EQ(settings.active_provider_id, std::string("gemini-cli"));
	UAM_ASSERT_EQ(settings.provider_command_template, std::string("gemini -p {prompt}"));
	UAM_ASSERT_EQ(settings.provider_yolo_mode, true);
	UAM_ASSERT_EQ(settings.provider_extra_flags, std::string("--approval-mode yolo"));
	UAM_ASSERT_EQ(settings.cli_idle_timeout_seconds, 3600);
	UAM_ASSERT_EQ(settings.ui_theme, std::string("system"));
	UAM_ASSERT_EQ(mode, CenterViewMode::CliConsole);

	UAM_ASSERT(SettingsStore::Save(settings_file, settings, mode));
	const std::string saved = ReadFile(settings_file);
	UAM_ASSERT(saved.find("active_provider_id=gemini-cli") != std::string::npos);
	UAM_ASSERT(saved.find("provider_command_template=gemini -p {prompt}") != std::string::npos);
	UAM_ASSERT(saved.find("rag_") == std::string::npos);
	UAM_ASSERT(saved.find("selected_model_id") == std::string::npos);
	UAM_ASSERT(saved.find("vector_db_backend") == std::string::npos);
	UAM_ASSERT(saved.find("prompt_profile") == std::string::npos);
}

UAM_TEST(ChatRepositoryToleratesLegacyFieldsAndDropsThemOnWrite)
{
	TempDir temp("uam-chats");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	const fs::path legacy_file = chats_dir / "legacy-chat.json";

	UAM_ASSERT(uam::io::WriteTextFile(legacy_file, R"({
  "id": "legacy-chat",
  "provider_id": "codex-cli",
  "native_session_id": "native-1",
  "folder_id": "folder-a",
  "template_override_id": "removed-template.md",
  "prompt_profile_bootstrapped": true,
  "rag_enabled": false,
  "rag_source_directories": ["/tmp/a"],
  "title": "Legacy",
  "created_at": "2026-01-01 00:00:00",
  "updated_at": "2026-01-01 00:00:01",
  "messages": [
    {"role": "user", "content": "hello", "created_at": "2026-01-01 00:00:00"}
  ]
})"));

	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().id, std::string("legacy-chat"));
	UAM_ASSERT_EQ(loaded.front().native_session_id, std::string("native-1"));
	UAM_ASSERT_EQ(loaded.front().messages.size(), static_cast<std::size_t>(1));

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, loaded.front()));
	const std::string rewritten = ReadFile(legacy_file);
	UAM_ASSERT(rewritten.find("template_override_id") == std::string::npos);
	UAM_ASSERT(rewritten.find("prompt_profile_bootstrapped") == std::string::npos);
	UAM_ASSERT(rewritten.find("rag_enabled") == std::string::npos);
	UAM_ASSERT(rewritten.find("rag_source_directories") == std::string::npos);
}

UAM_TEST(ProviderRegistryNormalizesEveryIdToGeminiCli)
{
	const std::vector<std::string> legacy_ids = {
		"gemini",
		"gemini-structured",
		"codex-cli",
		"claude-cli",
		"opencode-local",
		"ollama-engine",
		"unknown",
	};

	for (const std::string& id : legacy_ids)
	{
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(id);
		UAM_ASSERT_EQ(std::string(runtime.RuntimeId()), std::string("gemini-cli"));
		UAM_ASSERT(ProviderRuntimeRegistry::IsKnownRuntimeId(id));
		UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled(id));
	}
}

UAM_TEST(GeminiCliInteractiveArgvUsesResumeAndFlags)
{
	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	AppSettings settings;
	settings.provider_yolo_mode = true;
	settings.provider_extra_flags = "--checkpointing";

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-abc";

	const std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(argv.size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(argv[1], std::string("--yolo"));
	UAM_ASSERT_EQ(argv[2], std::string("--checkpointing"));
	UAM_ASSERT_EQ(argv[3], std::string("-r"));
	UAM_ASSERT_EQ(argv[4], std::string("native-abc"));
}

UAM_TEST(GeminiCliCompatibilityAcceptsCurrentStableVersions)
{
	UAM_ASSERT_EQ(std::string(uam::PreferredGeminiCliVersion()), std::string("0.38.1"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.38.1"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.36.0"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.39.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.30.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("not-a-version"));
}

UAM_TEST(GeminiPromptClassifierStripsAnsiAndDetectsPrompt)
{
	const std::string output = "\x1b[33mThinking...\x1b[0m\r\n\xe2\x94\x82 > Type your message or @path\r\n";
	UAM_ASSERT(GeminiCliRecentOutputIndicatesInputPrompt(output));

	const std::string stripped = StripTerminalControlSequencesForLifecycle("\x1b[31mhello\x1b[0m\b!");
	UAM_ASSERT_EQ(stripped, std::string("hell!"));
	UAM_ASSERT(!GeminiCliRecentOutputIndicatesInputPrompt("tool output is still streaming\nno prompt yet"));
}

UAM_TEST(CliLifecycleTransitionsDriveBackgroundShutdownEligibility)
{
	uam::AppState app;
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.frontend_chat_id = "chat-1";
	terminal.attached_chat_id = "chat-1";
	terminal.attached_session_id = "native-1";

	MarkCliTerminalTurnBusy(terminal);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Busy);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Busy);
	UAM_ASSERT(terminal.generation_in_progress);
	terminal.last_idle_confirmed_time_s = 1.0;
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 121.0));

	MarkCliTerminalTurnIdle(terminal);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Idle);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Idle);
	UAM_ASSERT(!terminal.generation_in_progress);
	terminal.last_idle_confirmed_time_s = 59.0;
	UAM_ASSERT(IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-1", 120.0));
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "native-1", 120.0));

	terminal.ui_attached = true;
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	terminal.ui_attached = false;

	PendingRuntimeCall pending;
	pending.chat_id = "chat-1";
	app.pending_calls.push_back(std::move(pending));
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
}

UAM_TEST(CliTerminalIdentitySeparatesFrontendChatAndNativeSession)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-local";
	chat.native_session_id = "native-session";
	app.chats.push_back(chat);

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = "chat-local";
	terminal.attached_chat_id = "chat-local";
	terminal.attached_session_id = "native-session";

	UAM_ASSERT(CliTerminalMatchesChatId(terminal, "chat-local"));
	UAM_ASSERT(CliTerminalMatchesChatId(terminal, "native-session"));
	UAM_ASSERT(!CliTerminalMatchesChatId(terminal, "other-chat"));
	UAM_ASSERT_EQ(CliTerminalPrimaryChatId(terminal), std::string("chat-local"));
	UAM_ASSERT_EQ(CliTerminalSyncTargetId(terminal), std::string("native-session"));
	UAM_ASSERT_EQ(FindChatIndexForCliTerminal(app, terminal), 0);
}

UAM_TEST(FolderLifecycleKeepsWorkspaceRootsMinimal)
{
	TempDir temp("uam-folders");
	uam::AppState app;
	app.data_root = temp.root;
	ChatDomainService().EnsureDefaultFolder(app);

	std::string created_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &created_id));
	UAM_ASSERT(!created_id.empty());
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) != nullptr);

	const fs::path renamed_root = temp.root / "renamed";
	fs::create_directories(renamed_root);
	UAM_ASSERT(RenameFolderById(app, created_id, "Renamed", renamed_root.string()));
	const ChatFolder* renamed = ChatDomainService().FindFolderById(app, created_id);
	UAM_ASSERT(renamed != nullptr);
	UAM_ASSERT_EQ(renamed->title, std::string("Renamed"));
	UAM_ASSERT_EQ(renamed->directory, renamed_root.string());

	ChatSession folder_chat;
	folder_chat.id = "chat-in-folder";
	folder_chat.provider_id = "gemini-cli";
	folder_chat.folder_id = created_id;
	folder_chat.title = "Folder chat";
	folder_chat.created_at = "2026-01-01T00:00:00.000Z";
	folder_chat.updated_at = "2026-01-01T00:00:00.000Z";

	ChatSession general_chat;
	general_chat.id = "chat-in-general";
	general_chat.provider_id = "gemini-cli";
	general_chat.folder_id = uam::constants::kDefaultFolderId;
	general_chat.title = "General chat";
	general_chat.created_at = "2026-01-01T00:00:00.000Z";
	general_chat.updated_at = "2026-01-01T00:00:00.000Z";

	app.chats.push_back(folder_chat);
	app.chats.push_back(general_chat);
	app.selected_chat_index = 0;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, folder_chat));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, general_chat));

	const fs::path folder_chat_file = AppPaths::UamChatFilePath(temp.root, folder_chat.id);
	const fs::path general_chat_file = AppPaths::UamChatFilePath(temp.root, general_chat.id);
	UAM_ASSERT(fs::exists(folder_chat_file));
	UAM_ASSERT(fs::exists(general_chat_file));

	UAM_ASSERT(DeleteFolderById(app, created_id));
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) == nullptr);
	UAM_ASSERT(ChatDomainService().FindFolderById(app, uam::constants::kDefaultFolderId) != nullptr);
	UAM_ASSERT(ChatDomainService().FindChatIndexById(app, folder_chat.id) < 0);
	UAM_ASSERT(ChatDomainService().FindChatIndexById(app, general_chat.id) >= 0);
	UAM_ASSERT_EQ(app.chats[ChatDomainService().FindChatIndexById(app, general_chat.id)].folder_id, std::string(uam::constants::kDefaultFolderId));
	UAM_ASSERT(fs::exists(renamed_root));
	UAM_ASSERT(!fs::exists(folder_chat_file));
	UAM_ASSERT(fs::exists(general_chat_file));
}

UAM_TEST(DeleteFolderBlocksWhenContainedChatIsRunning)
{
	TempDir temp("uam-folder-pending-delete");
	uam::AppState app;
	app.data_root = temp.root;
	ChatDomainService().EnsureDefaultFolder(app);

	std::string created_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &created_id));

	ChatSession folder_chat;
	folder_chat.id = "chat-running";
	folder_chat.provider_id = "gemini-cli";
	folder_chat.folder_id = created_id;
	folder_chat.title = "Running chat";
	app.chats.push_back(folder_chat);

	PendingRuntimeCall call;
	call.chat_id = folder_chat.id;
	app.pending_calls.push_back(std::move(call));

	UAM_ASSERT(!DeleteFolderById(app, created_id));
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) != nullptr);
	UAM_ASSERT(ChatDomainService().FindChatIndexById(app, folder_chat.id) >= 0);
}

int main()
{
	int failures = 0;
	for (const TestCase& test : Registry())
	{
		try
		{
			test.fn();
			std::cout << "[PASS] " << test.name << '\n';
		}
		catch (const std::exception& ex)
		{
			++failures;
			std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
		}
	}

	if (failures != 0)
	{
		std::cerr << failures << " test(s) failed.\n";
		return 1;
	}

	return 0;
}
