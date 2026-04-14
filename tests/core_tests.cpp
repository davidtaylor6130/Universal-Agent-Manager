#include "app/runtime_orchestration_services.h"
#include "common/models/app_models.h"
#include "common/paths/app_paths.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_repository.h"
#include "common/chat/chat_folder_store.h"
#include "common/config/frontend_actions.h"
#include "common/constants/app_constants.h"
#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/provider/markdown_template_catalog.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/rag/rag_index_service.h"
#include "common/config/settings_store.h"
#include "common/platform/platform_services.h"
#include "common/utils/string_utils.h"
#include "common/rag/ollama_engine_client.h"
#include "common/state/app_state.h"
#include "common/ui/chat_actions/chat_action_folder_lifecycle.h"
#include "common/ui/chat_actions/chat_action_pending_calls.h"
#include "common/ui/chat_actions/chat_action_remove_chat.h"
#include "common/vcs/vcs_workspace_service.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_security.h"
#include "common/runtime/terminal/terminal_provider_cli.h"

#include <filesystem>
#include <fstream>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{

	struct TestFailure : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	struct TestCase
	{
		const char* name;
		void (*fn)();
	};

	std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> tests;
		return tests;
	}

	struct TestRegistrar
	{
		TestRegistrar(const char* name, void (*fn)())
		{
			Registry().push_back(TestCase{name, fn});
		}
	};

	// clang-format off
#define UAM_TEST(name) \
	static void name(); \
	static TestRegistrar registrar_##name{#name, &name}; \
	static void name()

#define UAM_ASSERT(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			Fail(std::string("assertion failed: ") + #cond, __FILE__, __LINE__); \
		} \
	} while (0)

#define UAM_ASSERT_EQ(expected, actual) \
	do \
	{ \
		const auto& uam_expected = (expected); \
		const auto& uam_actual = (actual); \
		if (!(uam_expected == uam_actual)) \
		{ \
			std::ostringstream uam_out; \
			uam_out << "expected [" << uam_expected << "] but got [" << uam_actual << "]"; \
			Fail(uam_out.str(), __FILE__, __LINE__); \
		} \
	} while (0)
	// clang-format on

	[[noreturn]] void Fail(const std::string& message, const char* file, int line)
	{
		std::ostringstream out;
		out << file << ":" << line << ": " << message;
		throw TestFailure(out.str());
	}

#define WriteTextFile WriteTestTextFile

	bool WriteTextFile(const fs::path& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);

		if (!out.good())
		{
			return false;
		}

		out << content;
		return out.good();
	}

	std::string JsonString(const std::string& value)
	{
		std::string out;
		out.push_back('"');

		for (const char ch : value)
		{
			switch (ch)
			{
			case '\\':
				out += "\\\\";
				break;
			case '"':
				out += "\\\"";
				break;
			case '\b':
				out += "\\b";
				break;
			case '\f':
				out += "\\f";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				out.push_back(ch);
				break;
			}
		}

		out.push_back('"');
		return out;
	}

	fs::path UniqueTempDir(const std::string& prefix)
	{
		const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
		std::mt19937_64 rng(static_cast<std::mt19937_64::result_type>(now));
		std::uniform_int_distribution<unsigned long long> dist;
		const fs::path root = fs::temp_directory_path() / (prefix + "-" + std::to_string(now) + "-" + std::to_string(dist(rng)));
		fs::create_directories(root);
		return root;
	}

	struct TempDir
	{
		fs::path root;

		explicit TempDir(const std::string& prefix) : root(UniqueTempDir(prefix))
		{
		}

		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(root, ec);
		}
	};

	class ScopedEnvVar
	{
	  public:
		ScopedEnvVar(std::string name, std::optional<std::string> value) : name_(std::move(name)), original_(ReadEnv(name_))
		{
			Set(value);
		}

		~ScopedEnvVar()
		{
			Set(original_);
		}

		ScopedEnvVar(const ScopedEnvVar&) = delete;
		ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

	  private:
		static std::optional<std::string> ReadEnv(const std::string& name)
		{
			if (const char* value = std::getenv(name.c_str()))
			{
				return std::string(value);
			}

			return std::nullopt;
		}

		static void SetEnvValue(const std::string& name, const std::optional<std::string>& value)
		{
#if defined(_WIN32)

			if (value.has_value())
			{
				_putenv_s(name.c_str(), value->c_str());
			}
			else
			{
				_putenv_s(name.c_str(), "");
			}

#else

			if (value.has_value())
			{
				setenv(name.c_str(), value->c_str(), 1);
			}
			else
			{
				unsetenv(name.c_str());
			}

#endif
		}

		void Set(const std::optional<std::string>& value)
		{
			SetEnvValue(name_, value);
		}

		std::string name_;
		std::optional<std::string> original_;
	};

	void WriteProjectsJson(const fs::path& gemini_home, const fs::path& project_root, const std::string& tmp_name)
	{
		const fs::path file = gemini_home / "projects.json";
		std::ostringstream out;
		out << "{\n";
		out << "  \"projects\": {\n";
		out << "    " << JsonString(project_root.generic_string()) << ": " << JsonString(tmp_name) << "\n";
		out << "  }\n";
		out << "}\n";
		UAM_ASSERT(WriteTextFile(file, out.str()));
	}

	void WriteNativeProjectRoot(const fs::path& tmp_chat_dir, const fs::path& project_root)
	{
		UAM_ASSERT(WriteTextFile(tmp_chat_dir / ".project_root", project_root.generic_string() + "\n"));
	}

	void WriteGeminiNativeSession(const fs::path& chats_dir, const std::string& session_id, const std::vector<std::pair<std::string, std::string>>& messages, const std::string& start_time = "2026-03-21 10:00:00", const std::string& updated_time = "2026-03-21 10:00:01")
	{
		std::ostringstream out;
		out << "{\n";
		out << "  \"sessionId\": " << JsonString(session_id) << ",\n";
		out << "  \"startTime\": " << JsonString(start_time) << ",\n";
		out << "  \"lastUpdated\": " << JsonString(updated_time) << ",\n";
		out << "  \"messages\": [\n";

		for (std::size_t i = 0; i < messages.size(); ++i)
		{
			out << "    {\n";
			out << "      \"type\": " << JsonString(messages[i].first) << ",\n";
			out << "      \"timestamp\": " << JsonString(updated_time) << ",\n";
			out << "      \"content\": { \"text\": " << JsonString(messages[i].second) << " }\n";
			out << "    }";

			if (i + 1 < messages.size())
			{
				out << ",";
			}

			out << "\n";
		}

		out << "  ]\n";
		out << "}\n";
		UAM_ASSERT(WriteTextFile(chats_dir / (session_id + ".json"), out.str()));
	}

	uam::AppState MakeTestAppState(const fs::path& data_root)
	{
	uam::AppState app;
	app.data_root = data_root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.active_provider_id = "gemini-cli";
	return app;
}

	ChatSession* FindChatById(std::vector<ChatSession>& chats, const std::string& id)
	{
		for (ChatSession& chat : chats)
		{
			if (chat.id == id)
			{
				return &chat;
			}
		}

		return nullptr;
	}

	void WaitForFileMissing(const fs::path& path)
	{
		for (int i = 0; i < 100; ++i)
		{
			if (!fs::exists(path))
			{
				return;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

} // namespace

UAM_TEST(TestGeminiHomeResolutionPrecedence)
{
	TempDir cli_home("uam-cli-home");
	TempDir gemini_home("uam-gemini-home");
	TempDir fallback_home("uam-fallback-home");

	{
		ScopedEnvVar cli("GEMINI_CLI_HOME", cli_home.root.string());
		ScopedEnvVar gemini("GEMINI_HOME", gemini_home.root.string());
		ScopedEnvVar user_profile("USERPROFILE", fallback_home.root.string());
		ScopedEnvVar home("HOME", fallback_home.root.string());
		UAM_ASSERT_EQ(cli_home.root.lexically_normal().generic_string(), AppPaths::GeminiHomePath().lexically_normal().generic_string());
	}

	{
		ScopedEnvVar cli("GEMINI_CLI_HOME", std::nullopt);
		ScopedEnvVar gemini("GEMINI_HOME", gemini_home.root.string());
		ScopedEnvVar user_profile("USERPROFILE", fallback_home.root.string());
		ScopedEnvVar home("HOME", fallback_home.root.string());
		UAM_ASSERT_EQ(gemini_home.root.lexically_normal().generic_string(), AppPaths::GeminiHomePath().lexically_normal().generic_string());
	}
}

UAM_TEST(TestDefaultDataRootPathResolution)
{
	TempDir home_root("uam-home-root");

#if defined(_WIN32)
	TempDir local_app_data("uam-local-app-data");
	{
		ScopedEnvVar local("LOCALAPPDATA", local_app_data.root.string());
		ScopedEnvVar app_data("APPDATA", std::nullopt);
		ScopedEnvVar user_profile("USERPROFILE", std::nullopt);
		ScopedEnvVar home_drive("HOMEDRIVE", std::nullopt);
		ScopedEnvVar home_path("HOMEPATH", std::nullopt);
		ScopedEnvVar home("HOME", home_root.root.string());
		const fs::path expected = local_app_data.root / "Universal Agent Manager";
		UAM_ASSERT_EQ(expected.lexically_normal().generic_string(), AppPaths::DefaultDataRootPath().lexically_normal().generic_string());
	}

#else
	{
		ScopedEnvVar home("HOME", home_root.root.string());
#if defined(__APPLE__)
		const fs::path expected = home_root.root / "Library" / "Application Support" / "Universal Agent Manager";
#else
		const fs::path expected = home_root.root / ".universal_agent_manager";
#endif
		UAM_ASSERT_EQ(expected.lexically_normal().generic_string(), AppPaths::DefaultDataRootPath().lexically_normal().generic_string());
	}

#endif
}

UAM_TEST(TestResolveGeminiProjectTmpDirViaProjectRootFile)
{
	TempDir gemini_home("uam-gemini-home");
	TempDir project("uam-project");

	const fs::path tmp_chat_dir = gemini_home.root / "tmp" / "release";
	fs::create_directories(tmp_chat_dir / "chats");
	WriteNativeProjectRoot(tmp_chat_dir, project.root);

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	const std::optional<fs::path> resolved = AppPaths::ResolveGeminiProjectTmpDir(project.root);
	UAM_ASSERT(resolved.has_value());
	UAM_ASSERT_EQ(tmp_chat_dir.lexically_normal().generic_string(), resolved->lexically_normal().generic_string());
}

UAM_TEST(TestResolveGeminiProjectTmpDirViaProjectsJsonMapping)
{
	TempDir gemini_home("uam-gemini-home");
	TempDir project("uam-project");

	const fs::path tmp_chat_dir = gemini_home.root / "tmp" / "release";
	fs::create_directories(tmp_chat_dir / "chats");
	WriteProjectsJson(gemini_home.root, project.root, "release");

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	const std::optional<fs::path> resolved = AppPaths::ResolveGeminiProjectTmpDir(project.root);
	UAM_ASSERT(resolved.has_value());
	UAM_ASSERT_EQ(tmp_chat_dir.lexically_normal().generic_string(), resolved->lexically_normal().generic_string());
}

UAM_TEST(TestSettingsStoreMigratesLegacyCommandTemplate)
{
	TempDir data_root("uam-settings");
	const fs::path settings_file = data_root.root / "settings.txt";
	UAM_ASSERT(WriteTextFile(settings_file, "gemini_command_template=gemini -p {prompt}\n"
	                                        "gemini_yolo_mode=1\n"
	                                        "gemini_extra_flags=--alpha --beta\n"
	                                        "center_view_mode=cli\n"));

	AppSettings settings;
	settings.gemini_command_template = "unused";
	settings.gemini_extra_flags = "";
	CenterViewMode view_mode = CenterViewMode::Structured;

	SettingsStore::Load(settings_file, settings, view_mode);

	UAM_ASSERT_EQ(std::string("gemini -p {prompt}"), settings.gemini_command_template);
	UAM_ASSERT(settings.gemini_yolo_mode);
	UAM_ASSERT_EQ(std::string("--alpha --beta"), settings.gemini_extra_flags);
	UAM_ASSERT(view_mode == CenterViewMode::CliConsole);
}

UAM_TEST(TestSettingsStoreRoundTripExtendedPreferences)
{
	TempDir data_root("uam-settings-roundtrip");
	const fs::path settings_file = data_root.root / "settings.txt";

	AppSettings write_settings;
	write_settings.active_provider_id = "gemini-cli";
	write_settings.provider_command_template = "gemini {resume} {flags} {prompt}";
	write_settings.provider_yolo_mode = true;
	write_settings.provider_extra_flags = "--alpha --beta";
	write_settings.runtime_backend = "provider-cli";
	write_settings.selected_model_id = "chat-model.gguf";
	write_settings.vector_db_backend = "none";
	write_settings.selected_vector_model_id = "embed-model.gguf";
	write_settings.vector_database_name_override = "team_index_v1";
	write_settings.cli_idle_timeout_seconds = 420;
	write_settings.prompt_profile_root_path = "/tmp/.Gemini_universal_agent_manager";
	write_settings.default_prompt_profile_id = "baseline.md";
	write_settings.ui_theme = "system";
	write_settings.confirm_delete_chat = false;
	write_settings.confirm_delete_folder = false;
	write_settings.remember_last_chat = true;
	write_settings.last_selected_chat_id = "chat-123";
	write_settings.ui_scale_multiplier = 1.35f;
	write_settings.window_width = 1680;
	write_settings.window_height = 960;
	write_settings.window_maximized = true;

	UAM_ASSERT(SettingsStore::Save(settings_file, write_settings, CenterViewMode::CliConsole));

	AppSettings loaded;
	CenterViewMode loaded_mode = CenterViewMode::Structured;
	SettingsStore::Load(settings_file, loaded, loaded_mode);

	UAM_ASSERT_EQ(std::string("system"), loaded.ui_theme);
	UAM_ASSERT(loaded_mode == CenterViewMode::CliConsole);
	UAM_ASSERT_EQ(std::string("gemini-cli"), loaded.active_provider_id);
	UAM_ASSERT(loaded.provider_yolo_mode);
	UAM_ASSERT_EQ(std::string("--alpha --beta"), loaded.provider_extra_flags);
	UAM_ASSERT_EQ(std::string("provider-cli"), loaded.runtime_backend);
	UAM_ASSERT_EQ(std::string("chat-model.gguf"), loaded.selected_model_id);
	UAM_ASSERT_EQ(std::string("none"), loaded.vector_db_backend);
	UAM_ASSERT_EQ(std::string("embed-model.gguf"), loaded.selected_vector_model_id);
	UAM_ASSERT_EQ(std::string("team_index_v1"), loaded.vector_database_name_override);
	UAM_ASSERT_EQ(420, loaded.cli_idle_timeout_seconds);
	UAM_ASSERT_EQ(std::string("/tmp/.Gemini_universal_agent_manager"), loaded.prompt_profile_root_path);
	UAM_ASSERT_EQ(std::string("baseline.md"), loaded.default_prompt_profile_id);
	UAM_ASSERT(!loaded.confirm_delete_chat);
	UAM_ASSERT(!loaded.confirm_delete_folder);
	UAM_ASSERT(loaded.remember_last_chat);
	UAM_ASSERT_EQ(std::string("chat-123"), loaded.last_selected_chat_id);
	UAM_ASSERT(std::fabs(loaded.ui_scale_multiplier - 1.35f) < 0.0001f);
	UAM_ASSERT_EQ(1680, loaded.window_width);
	UAM_ASSERT_EQ(960, loaded.window_height);
	UAM_ASSERT(loaded.window_maximized);
}

UAM_TEST(TestSettingsStoreClampsInvalidValues)
{
	TempDir data_root("uam-settings-clamp");
	const fs::path settings_file = data_root.root / "settings.txt";
	UAM_ASSERT(WriteTextFile(settings_file, "ui_theme=invalid-theme\n"
	                                        "ui_scale_multiplier=9.0\n"
	                                        "window_width=64\n"
	                                        "window_height=99\n"
	                                        "runtime_backend=unsupported\n"
	                                        "vector_db_backend=unsupported\n"
	                                        "cli_idle_timeout_seconds=1\n"
	                                        "remember_last_chat=0\n"
	                                        "last_selected_chat_id=stale-chat\n"));

	AppSettings loaded;
	CenterViewMode loaded_mode = CenterViewMode::Structured;
	SettingsStore::Load(settings_file, loaded, loaded_mode);

	UAM_ASSERT_EQ(std::string("dark"), loaded.ui_theme);
	UAM_ASSERT_EQ(std::string("provider-cli"), loaded.runtime_backend);
	UAM_ASSERT_EQ(std::string("none"), loaded.vector_db_backend);
	UAM_ASSERT_EQ(30, loaded.cli_idle_timeout_seconds);
	UAM_ASSERT(std::fabs(loaded.ui_scale_multiplier - 1.75f) < 0.0001f);
	UAM_ASSERT_EQ(960, loaded.window_width);
	UAM_ASSERT_EQ(620, loaded.window_height);
	UAM_ASSERT(!loaded.remember_last_chat);
	UAM_ASSERT_EQ(std::string(""), loaded.last_selected_chat_id);
}

UAM_TEST(TestSettingsStoreMigratesLegacyRuntimeBackendToActiveProvider)
{
	TempDir data_root("uam-settings-runtime-migration");
	const fs::path settings_file = data_root.root / "settings.txt";
	UAM_ASSERT(WriteTextFile(settings_file, "runtime_backend=provider-cli\n"
	                                        "center_view_mode=structured\n"));

	AppSettings loaded;
	loaded.active_provider_id.clear();
	CenterViewMode loaded_mode = CenterViewMode::CliConsole;
	SettingsStore::Load(settings_file, loaded, loaded_mode);

	UAM_ASSERT_EQ(std::string("gemini-cli"), loaded.active_provider_id);
	UAM_ASSERT(loaded_mode == CenterViewMode::Structured);
}

UAM_TEST(TestSettingsStoreLoadsLowScaleClamp)
{
	TempDir data_root("uam-settings-scale-low");
	const fs::path settings_file = data_root.root / "settings.txt";
	UAM_ASSERT(WriteTextFile(settings_file, "ui_theme=light\n"
	                                        "ui_scale_multiplier=0.1\n"
	                                        "remember_last_chat=1\n"
	                                        "last_selected_chat_id=chat-keep\n"));

	AppSettings loaded;
	CenterViewMode loaded_mode = CenterViewMode::Structured;
	SettingsStore::Load(settings_file, loaded, loaded_mode);

	UAM_ASSERT_EQ(std::string("light"), loaded.ui_theme);
	UAM_ASSERT(std::fabs(loaded.ui_scale_multiplier - 0.85f) < 0.0001f);
	UAM_ASSERT(loaded.remember_last_chat);
	UAM_ASSERT_EQ(std::string("chat-keep"), loaded.last_selected_chat_id);
}

UAM_TEST(TestSettingsStoreRoundTripsEscapedStringValues)
{
	TempDir data_root("uam-settings-escaped");
	const fs::path settings_file = data_root.root / "settings.txt";

	AppSettings write_settings;
	write_settings.provider_command_template = "gemini -p line one\\ntext\nline two\tindent";
	write_settings.provider_extra_flags = R"(--config C:\temp\gemini --literal \n)";
	write_settings.prompt_profile_root_path = R"(C:\Users\david\.Gemini_universal_agent_manager)";
	write_settings.last_selected_chat_id = "chat=42";

	UAM_ASSERT(SettingsStore::Save(settings_file, write_settings, CenterViewMode::Structured));

	AppSettings loaded;
	CenterViewMode loaded_mode = CenterViewMode::CliConsole;
	SettingsStore::Load(settings_file, loaded, loaded_mode);

	UAM_ASSERT_EQ(write_settings.provider_command_template, loaded.provider_command_template);
	UAM_ASSERT_EQ(write_settings.provider_extra_flags, loaded.provider_extra_flags);
	UAM_ASSERT_EQ(write_settings.prompt_profile_root_path, loaded.prompt_profile_root_path);
	UAM_ASSERT_EQ(write_settings.last_selected_chat_id, loaded.last_selected_chat_id);
	UAM_ASSERT(loaded_mode == CenterViewMode::Structured);
}

UAM_TEST(TestSettingsStoreLoadsLegacyUnescapedStringValues)
{
	TempDir data_root("uam-settings-legacy-raw");
	const fs::path settings_file = data_root.root / "settings.txt";
	UAM_ASSERT(WriteTextFile(settings_file, "provider_extra_flags=--config C:\\temp\\gemini\n"
	                                        "prompt_profile_root_path=C:\\Users\\david\\.Gemini_universal_agent_manager\n"
	                                        "last_selected_chat_id=chat-legacy\n"));

	AppSettings loaded;
	CenterViewMode loaded_mode = CenterViewMode::Structured;
	SettingsStore::Load(settings_file, loaded, loaded_mode);

	UAM_ASSERT_EQ(std::string("--config C:\\temp\\gemini"), loaded.provider_extra_flags);
	UAM_ASSERT_EQ(std::string("C:\\Users\\david\\.Gemini_universal_agent_manager"), loaded.prompt_profile_root_path);
	UAM_ASSERT_EQ(std::string("chat-legacy"), loaded.last_selected_chat_id);
}

UAM_TEST(TestProviderRuntimeBuildCommandReplacesPlaceholders)
{
	AppSettings settings;
	settings.provider_command_template = "gemini -r {resume} --mode test {flags} --model {model} --prompt {prompt} --files {files}";
	settings.provider_yolo_mode = true;
	settings.provider_extra_flags = "--profile nightly --dry-run";
	settings.selected_model_id = "gpt-local.gguf";

	const std::string command = provider_runtime_internal::BuildCommandFromTemplate(settings, "Review this patch", {"notes one.txt", "more/files.md"}, "resume-42", "gemini -r {resume} {flags} {prompt}");

	UAM_ASSERT(command.find("{resume}") == std::string::npos);
	UAM_ASSERT(command.find("{flags}") == std::string::npos);
	UAM_ASSERT(command.find("{prompt}") == std::string::npos);
	UAM_ASSERT(command.find("{files}") == std::string::npos);
	UAM_ASSERT(command.find("{model}") == std::string::npos);
	UAM_ASSERT(command.find("resume-42") != std::string::npos);
	UAM_ASSERT(command.find("gpt-local.gguf") != std::string::npos);
	UAM_ASSERT(command.find("Review this patch") != std::string::npos);
	UAM_ASSERT(command.find("notes one.txt") != std::string::npos);
	UAM_ASSERT(command.find("more/files.md") != std::string::npos);
	UAM_ASSERT(command.find("--yolo") != std::string::npos);
	UAM_ASSERT(command.find("--profile") != std::string::npos);
	UAM_ASSERT(command.find("nightly") != std::string::npos);
	UAM_ASSERT(command.find("--dry-run") != std::string::npos);
}

UAM_TEST(TestRagIndexServiceSupportsDisabledVectorBackend)
{
	TempDir workspace("uam-rag-vector-disabled");
	UAM_ASSERT(WriteTextFile(workspace.root / "hello.md", "hello world"));

	RagIndexService::Config config;
	config.enabled = true;
	config.vector_backend = "none";
	config.vector_enabled = false;
	RagIndexService rag(config);

	std::string error;
	const std::vector<RagSnippet> snippets = rag.Retrieve(workspace.root, "hello", 3, 1, &error);
	UAM_ASSERT(!snippets.empty());
	UAM_ASSERT(error.empty());

	const RagRefreshResult refresh = rag.RebuildIndex(workspace.root);
	UAM_ASSERT(refresh.ok);
	UAM_ASSERT(refresh.indexed_files >= 1);
}

UAM_TEST(TestChatRepositoryRoundTripsRestoreSliceState)
{
	TempDir data_root("uam-chat-repository-core");

	ChatSession chat;
	chat.id = "chat-test-1";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-session-1";
	chat.parent_chat_id = "chat-parent";
	chat.branch_root_chat_id = "chat-branch-root";
	chat.branch_from_message_index = 2;
	chat.folder_id = "folder-default";
	chat.template_override_id = "custom-template.md";
	chat.prompt_profile_bootstrapped = true;
	chat.rag_enabled = false;
	chat.rag_source_directories = {"/tmp/workspace-a", "/tmp/workspace-b"};
	chat.title = "Core Chat";
	chat.created_at = "2026-03-19 10:11:12";
	chat.updated_at = "2026-03-19 10:11:13";
	chat.linked_files = {"notes.md", "todo.md"};
	chat.workspace_directory = "/tmp/workspace-a";
	chat.approval_mode = "yolo";
	chat.model_id = "chat-model.gguf";
	chat.extra_flags = "--alpha --beta";
	Message message;
	message.role = MessageRole::User;
	message.content = "hello";
	message.created_at = "2026-03-19 10:11:13";
	message.provider = "gemini-cli";
	message.tokens_input = 12;
	message.tokens_output = 34;
	message.estimated_cost_usd = 0.42;
	message.time_to_first_token_ms = 55;
	message.processing_time_ms = 1234;
	message.interrupted = true;
	message.thoughts = "ponder";
	message.tool_calls.push_back(ToolCall{"tool-1", "search", "{\"query\":\"hello\"}", "ok", "done"});
	chat.messages.push_back(std::move(message));

	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));

	const fs::path chat_file = AppPaths::UamChatFilePath(data_root.root, chat.id);
	const std::string file_text = ReadTextFile(chat_file);
	UAM_ASSERT(file_text.find("parent_chat_id") != std::string::npos);
	UAM_ASSERT(file_text.find("branch_root_chat_id") != std::string::npos);
	UAM_ASSERT(file_text.find("branch_from_message_index") != std::string::npos);
	UAM_ASSERT(file_text.find("template_override_id") != std::string::npos);
	UAM_ASSERT(file_text.find("prompt_profile_bootstrapped") != std::string::npos);
	UAM_ASSERT(file_text.find("rag_enabled") != std::string::npos);
	UAM_ASSERT(file_text.find("rag_source_directories") != std::string::npos);
	UAM_ASSERT(file_text.find("linked_files") != std::string::npos);
	UAM_ASSERT(file_text.find("workspace_directory") != std::string::npos);
	UAM_ASSERT(file_text.find("approval_mode") != std::string::npos);
	UAM_ASSERT(file_text.find("model_id") != std::string::npos);
	UAM_ASSERT(file_text.find("extra_flags") != std::string::npos);

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	const ChatSession& loaded_chat = loaded.front();
	UAM_ASSERT_EQ(std::string("chat-test-1"), loaded_chat.id);
	UAM_ASSERT_EQ(std::string("gemini-cli"), loaded_chat.provider_id);
	UAM_ASSERT_EQ(std::string("native-session-1"), loaded_chat.native_session_id);
	UAM_ASSERT_EQ(std::string("chat-parent"), loaded_chat.parent_chat_id);
	UAM_ASSERT_EQ(std::string("chat-branch-root"), loaded_chat.branch_root_chat_id);
	UAM_ASSERT_EQ(2, loaded_chat.branch_from_message_index);
	UAM_ASSERT_EQ(std::string("folder-default"), loaded_chat.folder_id);
	UAM_ASSERT_EQ(std::string("custom-template.md"), loaded_chat.template_override_id);
	UAM_ASSERT_EQ(true, loaded_chat.prompt_profile_bootstrapped);
	UAM_ASSERT_EQ(false, loaded_chat.rag_enabled);
	UAM_ASSERT(loaded_chat.rag_source_directories == std::vector<std::string>({"/tmp/workspace-a", "/tmp/workspace-b"}));
	UAM_ASSERT_EQ(std::string("Core Chat"), loaded_chat.title);
	UAM_ASSERT_EQ(std::string("2026-03-19 10:11:12"), loaded_chat.created_at);
	UAM_ASSERT_EQ(std::string("2026-03-19 10:11:13"), loaded_chat.updated_at);
	UAM_ASSERT(loaded_chat.linked_files == std::vector<std::string>({"notes.md", "todo.md"}));
	UAM_ASSERT_EQ(std::string("/tmp/workspace-a"), loaded_chat.workspace_directory);
	UAM_ASSERT_EQ(std::string("yolo"), loaded_chat.approval_mode);
	UAM_ASSERT_EQ(std::string("chat-model.gguf"), loaded_chat.model_id);
	UAM_ASSERT_EQ(std::string("--alpha --beta"), loaded_chat.extra_flags);
	UAM_ASSERT_EQ(1u, loaded_chat.messages.size());
	UAM_ASSERT(loaded_chat.messages.front().role == MessageRole::User);
	UAM_ASSERT_EQ(std::string("hello"), loaded_chat.messages.front().content);
	UAM_ASSERT_EQ(std::string("2026-03-19 10:11:13"), loaded_chat.messages.front().created_at);
	UAM_ASSERT_EQ(std::string("gemini-cli"), loaded_chat.messages.front().provider);
	UAM_ASSERT_EQ(12, loaded_chat.messages.front().tokens_input);
	UAM_ASSERT_EQ(34, loaded_chat.messages.front().tokens_output);
	UAM_ASSERT_EQ(0.42, loaded_chat.messages.front().estimated_cost_usd);
	UAM_ASSERT_EQ(55, loaded_chat.messages.front().time_to_first_token_ms);
	UAM_ASSERT_EQ(1234, loaded_chat.messages.front().processing_time_ms);
	UAM_ASSERT_EQ(true, loaded_chat.messages.front().interrupted);
	UAM_ASSERT_EQ(std::string("ponder"), loaded_chat.messages.front().thoughts);
	UAM_ASSERT_EQ(1u, loaded_chat.messages.front().tool_calls.size());
	UAM_ASSERT_EQ(std::string("tool-1"), loaded_chat.messages.front().tool_calls.front().id);
	UAM_ASSERT_EQ(std::string("search"), loaded_chat.messages.front().tool_calls.front().name);
	UAM_ASSERT_EQ(std::string("{\"query\":\"hello\"}"), loaded_chat.messages.front().tool_calls.front().args_json);
	UAM_ASSERT_EQ(std::string("ok"), loaded_chat.messages.front().tool_calls.front().result_text);
	UAM_ASSERT_EQ(std::string("done"), loaded_chat.messages.front().tool_calls.front().status);
}

UAM_TEST(TestChatRepositoryRecoversFromBackupFileWhenPrimaryIsCorrupt)
{
	TempDir data_root("uam-chat-repository-recover");
	const fs::path chats_root = AppPaths::UamChatsRootPath(data_root.root);
	fs::create_directories(chats_root);

	const fs::path primary = chats_root / "chat-recover.json";
	const fs::path backup = chats_root / "chat-recover.json.bak";

	UAM_ASSERT(WriteTextFile(primary, "{"));
	UAM_ASSERT(WriteTextFile(backup, R"({
  "id": "chat-recover",
  "provider_id": "gemini-cli",
  "native_session_id": "chat-recover",
  "parent_chat_id": "chat-parent",
  "branch_root_chat_id": "chat-recover",
  "branch_from_message_index": 1,
  "folder_id": "folder-default",
  "template_override_id": "template-recover.md",
  "prompt_profile_bootstrapped": true,
  "rag_enabled": false,
  "rag_source_directories": [
    "/tmp/workspace-a",
    "/tmp/workspace-b"
  ],
  "title": "Recovered Chat",
  "created_at": "2026-04-10 10:00:00",
  "updated_at": "2026-04-10 10:00:01",
  "linked_files": [
    "notes.md"
  ],
  "workspace_directory": "/tmp/workspace-a",
  "approval_mode": "yolo",
  "model_id": "chat-model.gguf",
  "extra_flags": "--alpha --beta",
  "messages": [
    {
      "role": "user",
      "content": "hello",
      "created_at": "2026-04-10 10:00:01",
      "provider": "gemini-cli",
      "tokens_input": 4,
      "tokens_output": 8,
      "estimated_cost_usd": 0.12,
      "time_to_first_token_ms": 50,
      "processing_time_ms": 125,
      "interrupted": true,
      "thoughts": "ponder",
      "tool_calls": [
        {
          "id": "tool-1",
          "name": "search",
          "args_json": "{\"query\":\"hello\"}",
          "result_text": "ok",
          "status": "done"
        }
      ]
    }
  ]
})"));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	const ChatSession& recovered = loaded.front();
	UAM_ASSERT_EQ(std::string("chat-recover"), recovered.id);
	UAM_ASSERT_EQ(std::string("gemini-cli"), recovered.provider_id);
	UAM_ASSERT_EQ(std::string("chat-recover"), recovered.native_session_id);
	UAM_ASSERT_EQ(std::string("chat-parent"), recovered.parent_chat_id);
	UAM_ASSERT_EQ(std::string("chat-recover"), recovered.branch_root_chat_id);
	UAM_ASSERT_EQ(1, recovered.branch_from_message_index);
	UAM_ASSERT_EQ(std::string("folder-default"), recovered.folder_id);
	UAM_ASSERT_EQ(std::string("template-recover.md"), recovered.template_override_id);
	UAM_ASSERT_EQ(true, recovered.prompt_profile_bootstrapped);
	UAM_ASSERT_EQ(false, recovered.rag_enabled);
	UAM_ASSERT(recovered.rag_source_directories == std::vector<std::string>({"/tmp/workspace-a", "/tmp/workspace-b"}));
	UAM_ASSERT_EQ(std::string("Recovered Chat"), recovered.title);
	UAM_ASSERT_EQ(std::string("2026-04-10 10:00:00"), recovered.created_at);
	UAM_ASSERT_EQ(std::string("2026-04-10 10:00:01"), recovered.updated_at);
	UAM_ASSERT(recovered.linked_files == std::vector<std::string>({"notes.md"}));
	UAM_ASSERT_EQ(std::string("/tmp/workspace-a"), recovered.workspace_directory);
	UAM_ASSERT_EQ(std::string("yolo"), recovered.approval_mode);
	UAM_ASSERT_EQ(std::string("chat-model.gguf"), recovered.model_id);
	UAM_ASSERT_EQ(std::string("--alpha --beta"), recovered.extra_flags);
	UAM_ASSERT_EQ(1u, recovered.messages.size());
	UAM_ASSERT_EQ(std::string("hello"), recovered.messages.front().content);
	UAM_ASSERT_EQ(true, recovered.messages.front().interrupted);
	UAM_ASSERT_EQ(1u, recovered.messages.front().tool_calls.size());

	const std::string repaired = ReadTextFile(primary);
	UAM_ASSERT(repaired.find("Recovered Chat") != std::string::npos);
	UAM_ASSERT(repaired.find("branch_root_chat_id") != std::string::npos);
	UAM_ASSERT(repaired.find("workspace_directory") != std::string::npos);
	UAM_ASSERT(repaired.find("extra_flags") != std::string::npos);
	UAM_ASSERT(!fs::exists(backup));
}

UAM_TEST(TestChatRepositoryRecoversWhenPrimaryMissingAndBackupExists)
{
	TempDir data_root("uam-chat-repository-missing-primary");
	const fs::path chats_root = AppPaths::UamChatsRootPath(data_root.root);
	fs::create_directories(chats_root);

	const fs::path primary = chats_root / "chat-recover-missing.json";
	const fs::path backup = chats_root / "chat-recover-missing.json.bak";

	UAM_ASSERT(WriteTextFile(backup, R"({
  "id": "chat-wrong-backup-id",
  "provider_id": "gemini-cli",
  "native_session_id": "chat-wrong-backup-id",
  "parent_chat_id": "chat-parent",
  "branch_root_chat_id": "chat-wrong-backup-id",
  "branch_from_message_index": 3,
  "folder_id": "folder-default",
  "template_override_id": "template-recover.md",
  "prompt_profile_bootstrapped": true,
  "rag_enabled": false,
  "rag_source_directories": [
    "/tmp/workspace-a"
  ],
  "title": "Recovered Missing Primary",
  "created_at": "2026-04-10 11:00:00",
  "updated_at": "2026-04-10 11:00:01",
  "linked_files": [
    "notes.md",
    "todo.md"
  ],
  "workspace_directory": "/tmp/workspace-a",
  "approval_mode": "yolo",
  "model_id": "chat-model.gguf",
  "extra_flags": "--gamma --delta",
  "messages": [
    {
      "role": "user",
      "content": "hello again",
      "created_at": "2026-04-10 11:00:01",
      "provider": "gemini-cli",
      "tokens_input": 5,
      "tokens_output": 9,
      "estimated_cost_usd": 0.15,
      "time_to_first_token_ms": 60,
      "processing_time_ms": 140,
      "interrupted": false,
      "thoughts": "retry",
      "tool_calls": []
    }
  ]
})"));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	const ChatSession& recovered = loaded.front();
	UAM_ASSERT_EQ(std::string("chat-recover-missing"), recovered.id);
	UAM_ASSERT_EQ(std::string("chat-wrong-backup-id"), recovered.native_session_id);
	UAM_ASSERT_EQ(std::string("chat-parent"), recovered.parent_chat_id);
	UAM_ASSERT_EQ(std::string("chat-recover-missing"), recovered.branch_root_chat_id);
	UAM_ASSERT_EQ(3, recovered.branch_from_message_index);
	UAM_ASSERT_EQ(std::string("Recovered Missing Primary"), recovered.title);
	UAM_ASSERT_EQ(false, recovered.rag_enabled);
	UAM_ASSERT(recovered.rag_source_directories == std::vector<std::string>({"/tmp/workspace-a"}));
	UAM_ASSERT(recovered.linked_files == std::vector<std::string>({"notes.md", "todo.md"}));
	UAM_ASSERT_EQ(std::string("/tmp/workspace-a"), recovered.workspace_directory);
	UAM_ASSERT_EQ(std::string("yolo"), recovered.approval_mode);
	UAM_ASSERT_EQ(std::string("chat-model.gguf"), recovered.model_id);
	UAM_ASSERT_EQ(std::string("--gamma --delta"), recovered.extra_flags);
	UAM_ASSERT_EQ(1u, recovered.messages.size());
	UAM_ASSERT_EQ(std::string("hello again"), recovered.messages.front().content);
	UAM_ASSERT(fs::exists(primary));
	UAM_ASSERT(!fs::exists(backup));

	const std::string repaired = ReadTextFile(primary);
	UAM_ASSERT(repaired.find("chat-recover-missing") != std::string::npos);
	UAM_ASSERT(repaired.find("branch_root_chat_id") != std::string::npos);
	UAM_ASSERT(repaired.find("workspace_directory") != std::string::npos);
	UAM_ASSERT(repaired.find("extra_flags") != std::string::npos);
}

UAM_TEST(TestChatRepositoryWarnsWhenPrimaryAndBackupDiverge)
{
	TempDir data_root("uam-chat-repository-diverge");
	const fs::path chats_root = AppPaths::UamChatsRootPath(data_root.root);
	fs::create_directories(chats_root);

	const fs::path primary = chats_root / "chat-diverge.json";
	const fs::path backup = chats_root / "chat-diverge.json.bak";

	UAM_ASSERT(WriteTextFile(primary, "{\n"
	                               "  \"id\": \"chat-diverge\",\n"
	                               "  \"provider_id\": \"gemini-cli\",\n"
	                               "  \"native_session_id\": \"chat-diverge\",\n"
	                               "  \"branch_root_chat_id\": \"chat-diverge\",\n"
	                               "  \"folder_id\": \"folder-default\",\n"
	                               "  \"workspace_directory\": \"/tmp/workspace-a\",\n"
	                               "  \"title\": \"Primary Title\",\n"
	                               "  \"created_at\": \"2026-04-10 12:00:00\",\n"
	                               "  \"updated_at\": \"2026-04-10 12:00:01\",\n"
	                               "  \"messages\": [\n"
	                               "    {\"role\": \"user\", \"content\": \"primary\", \"created_at\": \"2026-04-10 12:00:01\"}\n"
	                               "  ]\n"
	                               "}\n"));
	UAM_ASSERT(WriteTextFile(backup, "{\n"
	                               "  \"id\": \"chat-diverge\",\n"
	                               "  \"provider_id\": \"gemini-cli\",\n"
	                               "  \"native_session_id\": \"chat-diverge\",\n"
	                               "  \"branch_root_chat_id\": \"chat-diverge\",\n"
	                               "  \"folder_id\": \"folder-default\",\n"
	                               "  \"workspace_directory\": \"/tmp/workspace-b\",\n"
	                               "  \"title\": \"Primary Title\",\n"
	                               "  \"created_at\": \"2026-04-10 12:00:00\",\n"
	                               "  \"updated_at\": \"2026-04-10 12:00:01\",\n"
	                               "  \"messages\": [\n"
	                               "    {\"role\": \"user\", \"content\": \"primary\", \"created_at\": \"2026-04-10 12:00:01\"}\n"
	                               "  ]\n"
	                               "}\n"));

	std::string warning;
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root, &warning);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(std::string("Primary Title"), loaded.front().title);
	UAM_ASSERT(!warning.empty());
	UAM_ASSERT(warning.find("chat-diverge.json") != std::string::npos);
	UAM_ASSERT(warning.find("chat-diverge.json.bak") != std::string::npos);
}

UAM_TEST(TestChatRepositoryParsesLegacyBranchIndex)
{
	TempDir data_root("uam-chat-repository-legacy-branch");
	const fs::path legacy_root = AppPaths::ChatsRootPath(data_root.root);
	const fs::path legacy_chat = legacy_root / "legacy-branch";
	const fs::path messages_dir = legacy_chat / "messages";
	fs::create_directories(messages_dir);
	UAM_ASSERT(WriteTextFile(legacy_chat / "meta.txt",
	                         "provider_id=gemini-cli\n"
	                         "native_session_id=legacy-native\n"
	                         "branch_root=legacy-root\n"
	                         "branch_from_index=7\n"
	                         "title=Legacy Branch\n"
	                         "created_at=2026-04-10 10:00:00\n"
	                         "updated_at=2026-04-10 10:00:01\n"));
	UAM_ASSERT(WriteTextFile(messages_dir / "0001_user.txt", "hello\n"));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(7, loaded.front().branch_from_message_index);
	UAM_ASSERT_EQ(std::string("legacy-root"), loaded.front().branch_root_chat_id);
}

UAM_TEST(TestNativeSessionFileLookupRequiresExactFilename)
{
	TempDir data_root("uam-native-session-exact");
	const fs::path chats_dir = data_root.root / "chats";
	fs::create_directories(chats_dir);
	UAM_ASSERT(WriteTextFile(chats_dir / "session-12345.json", "{}"));
	UAM_ASSERT(WriteTextFile(chats_dir / "session-123.json", "{}"));

	uam::AppState app = MakeTestAppState(data_root.root);
	ChatHistorySyncService sync;
	const auto exact = sync.FindNativeSessionFilePath(chats_dir, "session-123");
	const auto missing = sync.FindNativeSessionFilePath(chats_dir, "session-12");

	UAM_ASSERT(exact.has_value());
	UAM_ASSERT_EQ((chats_dir / "session-123.json").lexically_normal().generic_string(), exact->lexically_normal().generic_string());
	UAM_ASSERT(!missing.has_value());
}

UAM_TEST(TestChatFolderStoreRoundTrip)
{
	TempDir data_root("uam-folder-store");

	std::vector<ChatFolder> folders;
	folders.push_back(ChatFolder{"folder-default", "General", "/tmp/workspace-a", true});
	folders.push_back(ChatFolder{"folder-product", "Product", "/tmp/workspace-b", false});

	UAM_ASSERT(ChatFolderStore::Save(data_root.root, folders));

	const std::string file_text = ReadTextFile(data_root.root / "folders.txt");
	UAM_ASSERT(file_text.find("collapsed=") != std::string::npos);

	const std::vector<ChatFolder> loaded = ChatFolderStore::Load(data_root.root);
	UAM_ASSERT_EQ(2u, loaded.size());
	UAM_ASSERT_EQ(std::string("folder-default"), loaded[0].id);
	UAM_ASSERT_EQ(std::string("General"), loaded[0].title);
	UAM_ASSERT_EQ(std::string("/tmp/workspace-a"), loaded[0].directory);
	UAM_ASSERT_EQ(true, loaded[0].collapsed);
	UAM_ASSERT_EQ(std::string("folder-product"), loaded[1].id);
	UAM_ASSERT_EQ(std::string("Product"), loaded[1].title);
	UAM_ASSERT_EQ(std::string("/tmp/workspace-b"), loaded[1].directory);
	UAM_ASSERT_EQ(false, loaded[1].collapsed);
}

UAM_TEST(TestFolderLifecycleCreateRenameAndDeletePersistToFolderStore)
{
	TempDir data_root("uam-folder-lifecycle");
	uam::AppState app = MakeTestAppState(data_root.root);

	UAM_ASSERT(CreateFolder(app, "Project Folder", "/tmp/project-folder"));
	UAM_ASSERT_EQ(1u, app.folders.size());

	const std::string folder_id = app.folders.front().id;
	UAM_ASSERT_EQ(std::string("Project Folder"), app.folders.front().title);
	UAM_ASSERT_EQ(std::string("/tmp/project-folder"), app.folders.front().directory);

	std::vector<ChatFolder> loaded = ChatFolderStore::Load(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(folder_id, loaded.front().id);
	UAM_ASSERT_EQ(std::string("Project Folder"), loaded.front().title);
	UAM_ASSERT_EQ(std::string("/tmp/project-folder"), loaded.front().directory);

	UAM_ASSERT(RenameFolderById(app, folder_id, "Renamed Folder", "/tmp/project-folder-renamed"));
	loaded = ChatFolderStore::Load(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(std::string("Renamed Folder"), loaded.front().title);
	UAM_ASSERT_EQ(std::string("/tmp/project-folder-renamed"), loaded.front().directory);

	ChatSession chat;
	chat.id = "chat-folder-delete";
	chat.provider_id = "gemini-cli";
	chat.folder_id = folder_id;
	chat.title = "Folder Chat";
	chat.created_at = "2026-04-11 10:00:00";
	chat.updated_at = "2026-04-11 10:00:01";
	app.chats.push_back(chat);

	UAM_ASSERT(DeleteFolderById(app, folder_id));
	UAM_ASSERT_EQ(1u, app.folders.size());
	UAM_ASSERT_EQ(std::string(uam::constants::kDefaultFolderId), app.folders.front().id);
	UAM_ASSERT_EQ(std::string(uam::constants::kDefaultFolderId), app.chats.front().folder_id);

	loaded = ChatFolderStore::Load(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(std::string(uam::constants::kDefaultFolderId), loaded.front().id);
}

UAM_TEST(TestResolveRequestedNewChatFolderIdClampsMissingFolderToValidSelection)
{
	TempDir data_root("uam-new-chat-folder-selection");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatDomainService().EnsureDefaultFolder(app);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", "/tmp/workspace-a", false});
	app.new_chat_folder_id = "folder-a";

	const std::string resolved_existing = ResolveRequestedNewChatFolderId(app, "folder-a");
	UAM_ASSERT_EQ(std::string("folder-a"), resolved_existing);
	UAM_ASSERT_EQ(std::string("folder-a"), app.new_chat_folder_id);

	const std::string resolved_missing = ResolveRequestedNewChatFolderId(app, "folder-missing");
	UAM_ASSERT_EQ(std::string("folder-a"), resolved_missing);
	UAM_ASSERT_EQ(std::string("folder-a"), app.new_chat_folder_id);

	app.new_chat_folder_id = "folder-missing";
	const std::string resolved_default = ResolveRequestedNewChatFolderId(app, "");
	UAM_ASSERT_EQ(std::string(uam::constants::kDefaultFolderId), resolved_default);
	UAM_ASSERT_EQ(std::string(uam::constants::kDefaultFolderId), app.new_chat_folder_id);
}

UAM_TEST(TestTrustedUiUrlGatesBridgeAccess)
{
	const std::string trusted = "file:///Users/test/Universal Agent Manager/UI-V2/dist/index.html";

	UAM_ASSERT(uam::cef::IsTrustedUiUrl("file:///Users/test/Universal Agent Manager/UI-V2/dist/index.html", trusted));
	UAM_ASSERT(uam::cef::IsTrustedUiUrl("file:///Users/test/Universal%20Agent%20Manager/UI-V2/dist/index.html", trusted));
	UAM_ASSERT(uam::cef::IsTrustedUiUrl("file:///Users/test/Universal%20Agent%20Manager/UI-V2/dist/index.html#chat-1", trusted));
	UAM_ASSERT(!uam::cef::IsTrustedUiUrl("file:///Users/test/Downloads/index.html", trusted));
	UAM_ASSERT(!uam::cef::IsTrustedUiUrl("https://example.com", trusted));
	UAM_ASSERT(uam::cef::ShouldOpenExternally("https://example.com/docs"));
	UAM_ASSERT(uam::cef::ShouldOpenExternally("mailto:support@example.com"));
	UAM_ASSERT(!uam::cef::ShouldOpenExternally("file:///Users/test/UI-V2/dist/index.html"));
}

UAM_TEST(TestResolveTrustedUiIndexUrlFindsMainBundleResourcesFromRendererHelper)
{
	TempDir bundle_root("uam-cef-bundle-path");

	const fs::path main_exe_dir =
		bundle_root.root / "universal_agent_manager.app" / "Contents" / "MacOS";
	const fs::path renderer_helper_exe_dir =
		bundle_root.root / "universal_agent_manager.app" / "Contents" / "Frameworks" /
		"universal_agent_manager Helper (Renderer).app" / "Contents" / "MacOS";
	const fs::path ui_index =
		bundle_root.root / "universal_agent_manager.app" / "Contents" / "Resources" /
		"UI-V2" / "dist" / "index.html";

	fs::create_directories(main_exe_dir);
	fs::create_directories(renderer_helper_exe_dir);
	fs::create_directories(ui_index.parent_path());
	UAM_ASSERT(WriteTextFile(ui_index, "<!doctype html>\n"));

	std::error_code ec;
	const fs::path normalized_ui_index = fs::weakly_canonical(ui_index, ec);
	const fs::path expected_path = ec ? ui_index.lexically_normal() : normalized_ui_index.lexically_normal();
	const std::string expected_url = std::string("file://") + expected_path.generic_string();
	UAM_ASSERT_EQ(expected_url, uam::cef::ResolveTrustedUiIndexUrl(main_exe_dir));
	UAM_ASSERT_EQ(expected_url, uam::cef::ResolveTrustedUiIndexUrl(renderer_helper_exe_dir));
}

UAM_TEST(TestChatRenameRejectsBlankTitle)
{
	TempDir data_root("uam-chat-rename-blank");

	ChatSession chat;
	chat.id = "chat-rename-blank";
	chat.provider_id = "codex-cli";
	chat.folder_id = "folder-default";
	chat.title = "Keep Me";
	chat.created_at = "2026-04-09 10:00:00";
	chat.updated_at = "2026-04-09 10:00:01";
	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));

	uam::AppState app = MakeTestAppState(data_root.root);
	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT(!ChatHistorySyncService().RenameChat(app, loaded.front(), "   \t  "));
	UAM_ASSERT_EQ(std::string("Keep Me"), loaded.front().title);
	UAM_ASSERT_EQ(std::string("Chat title is required."), app.status_line);

	const std::vector<ChatSession> reloaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, reloaded.size());
	UAM_ASSERT_EQ(std::string("Keep Me"), reloaded.front().title);
}

UAM_TEST(TestChatRenamePersistsAcrossReload)
{
	TempDir data_root("uam-chat-rename-reload");

	ChatSession chat;
	chat.id = "chat-rename-reload";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "chat-rename-reload";
	chat.folder_id = "folder-default";
	chat.title = "Original Title";
	chat.created_at = "2026-04-09 10:00:00";
	chat.updated_at = "2026-04-09 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "hello", "2026-04-09 10:00:00"});
	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));

	uam::AppState app = MakeTestAppState(data_root.root);
	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT(ChatHistorySyncService().RenameChat(app, loaded.front(), "  Renamed in UAM  "));
	UAM_ASSERT_EQ(std::string("Renamed in UAM"), loaded.front().title);

	const std::vector<ChatSession> reloaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, reloaded.size());
	UAM_ASSERT_EQ(std::string("Renamed in UAM"), reloaded.front().title);
}

UAM_TEST(TestChatRenameSurvivesNativeSyncRefresh)
{
	TempDir data_root("uam-chat-rename-native-sync");
	TempDir gemini_home("uam-chat-rename-native-sync-gemini-home");
	TempDir project("uam-chat-rename-native-sync-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "session-rename-sync", {{"user", "hello there"}, {"assistant", "native reply"}});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	ChatSession chat;
	chat.id = "session-rename-sync";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "session-rename-sync";
	chat.folder_id = "folder-a";
	chat.title = "Original Title";
	chat.created_at = "2026-03-21 10:00:00";
	chat.updated_at = "2026-03-21 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "hello there", "2026-03-21 10:00:01"});
	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));

	uam::AppState app = MakeTestAppState(data_root.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", project.root.string(), false});

	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT(ChatHistorySyncService().RenameChat(app, loaded.front(), "  Renamed in UAM  "));
	UAM_ASSERT_EQ(std::string("Renamed in UAM"), loaded.front().title);

	SyncChatsFromNative(app, chat.id, true);

	const std::vector<ChatSession> reloaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, reloaded.size());
	UAM_ASSERT_EQ(std::string("Renamed in UAM"), reloaded.front().title);
	UAM_ASSERT_EQ(std::string("folder-a"), reloaded.front().folder_id);
	UAM_ASSERT_EQ(std::string("session-rename-sync"), reloaded.front().native_session_id);

	uam::AppState restarted = MakeTestAppState(data_root.root);
	restarted.folders.push_back(ChatFolder{"folder-a", "Folder A", project.root.string(), false});
	ChatHistorySyncService().LoadSidebarChats(restarted);
	UAM_ASSERT_EQ(1u, restarted.chats.size());
	UAM_ASSERT_EQ(std::string("Renamed in UAM"), restarted.chats.front().title);
}

UAM_TEST(TestLocalOverridesApplyToLinkedNativeChatsByNativeSessionId)
{
	TempDir data_root("uam-linked-override");

	uam::AppState app = MakeTestAppState(data_root.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", "/tmp/project-a", false});

	ChatSession local;
	local.id = "chat-local-draft";
	local.provider_id = "gemini-cli";
	local.native_session_id = "session-linked";
	local.folder_id = "folder-a";
	local.title = "Renamed Locally";
	local.created_at = "2026-04-09 10:00:00";
	local.updated_at = "2026-04-09 10:00:01";
	local.workspace_directory = "/tmp/project-a";
	local.approval_mode = "yolo";
	local.model_id = "gemini-2.0";
	local.extra_flags = "--fast";
	local.rag_enabled = false;
	local.linked_files = {"notes.md"};
	local.rag_source_directories = {"/tmp/project-a/src"};
	app.chats.push_back(local);
	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, local));

	ChatSession native;
	native.id = "session-linked";
	native.provider_id = "gemini-cli";
	native.native_session_id = "session-linked";
	native.folder_id = uam::constants::kDefaultFolderId;
	native.title = "Gemini First Message";
	native.created_at = "2026-04-09 10:00:00";
	native.updated_at = "2026-04-09 10:05:00";
	native.workspace_directory = "/tmp/project-b";
	native.approval_mode = "safe";
	native.model_id = "other-model";
	native.extra_flags = "--slow";
	native.rag_enabled = true;
	native.messages.push_back(Message{MessageRole::User, "hello", "2026-04-09 10:00:00"});

	std::vector<ChatSession> native_chats{native};
	ChatHistorySyncService().ApplyLocalOverrides(app, native_chats);

	UAM_ASSERT_EQ(1u, native_chats.size());
	UAM_ASSERT_EQ(std::string("session-linked"), native_chats.front().id);
	UAM_ASSERT_EQ(std::string("Renamed Locally"), native_chats.front().title);
	UAM_ASSERT_EQ(std::string("folder-a"), native_chats.front().folder_id);
	UAM_ASSERT_EQ(std::string("/tmp/project-a"), native_chats.front().workspace_directory);
	UAM_ASSERT_EQ(std::string("yolo"), native_chats.front().approval_mode);
	UAM_ASSERT_EQ(std::string("gemini-2.0"), native_chats.front().model_id);
	UAM_ASSERT_EQ(std::string("--fast"), native_chats.front().extra_flags);
	UAM_ASSERT_EQ(false, native_chats.front().rag_enabled);
	UAM_ASSERT(native_chats.front().linked_files == std::vector<std::string>({"notes.md"}));
	UAM_ASSERT(native_chats.front().rag_source_directories == std::vector<std::string>({"/tmp/project-a/src"}));
}

UAM_TEST(TestLoadSidebarChatsByDiscoveryImportsNativeGeminiChats)
{
	TempDir data_root("uam-discovery-import");
	TempDir gemini_home("uam-discovery-import-gemini-home");
	TempDir project("uam-discovery-import-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "session-discovery", {{"user", "hello from old gemini"}, {"assistant", "native reply"}});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	uam::AppState app = MakeTestAppState(data_root.root);
	ChatDomainService().EnsureDefaultFolder(app);
	ChatHistorySyncService().LoadSidebarChatsByDiscovery(app);

	UAM_ASSERT_EQ(1u, app.chats.size());
	const ChatSession& imported = app.chats.front();
	UAM_ASSERT_EQ(std::string("session-discovery"), imported.id);
	UAM_ASSERT_EQ(std::string("session-discovery"), imported.native_session_id);
	UAM_ASSERT_EQ(project.root.lexically_normal().generic_string(), fs::path(imported.workspace_directory).lexically_normal().generic_string());

	const auto folder_it = std::find_if(app.folders.begin(), app.folders.end(), [&](const ChatFolder& folder)
	{
		return FolderDirectoryMatches(folder.directory, project.root);
	});
	UAM_ASSERT(folder_it != app.folders.end());
	UAM_ASSERT_EQ(folder_it->id, imported.folder_id);

	const std::vector<ChatSession> persisted = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, persisted.size());
	UAM_ASSERT_EQ(std::string("session-discovery"), persisted.front().id);
}

UAM_TEST(TestLoadSidebarChatsByDiscoverySkipsEmptyNativeGeminiChats)
{
	TempDir data_root("uam-discovery-import-empty");
	TempDir gemini_home("uam-discovery-import-empty-gemini-home");
	TempDir project("uam-discovery-import-empty-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "chat-1775856499765-f3739f", {});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	uam::AppState app = MakeTestAppState(data_root.root);
	ChatDomainService().EnsureDefaultFolder(app);
	ChatHistorySyncService().LoadSidebarChatsByDiscovery(app);

	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(ChatRepository::LoadLocalChats(data_root.root).empty());
}

UAM_TEST(TestResolveNativeHistoryChatsDirUsesChatWorkspace)
{
	TempDir gemini_home("uam-native-dir-gemini-home");
	TempDir project_a("uam-native-dir-project-a");
	TempDir project_b("uam-native-dir-project-b");

	const fs::path tmp_a = gemini_home.root / "tmp" / "release-a";
	const fs::path tmp_b = gemini_home.root / "tmp" / "release-b";
	fs::create_directories(tmp_a / "chats");
	fs::create_directories(tmp_b / "chats");
	WriteNativeProjectRoot(tmp_a, project_a.root);
	WriteNativeProjectRoot(tmp_b, project_b.root);

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	uam::AppState app = MakeTestAppState(project_a.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", project_a.root.string(), false});
	app.folders.push_back(ChatFolder{"folder-b", "Folder B", project_b.root.string(), false});

	ChatSession chat_a;
	chat_a.id = "session-a";
	chat_a.provider_id = "gemini-cli";
	chat_a.folder_id = "folder-a";

	ChatSession chat_b;
	chat_b.id = "session-b";
	chat_b.provider_id = "gemini-cli";
	chat_b.folder_id = "folder-b";

	const fs::path resolved_a = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, chat_a);
	const fs::path resolved_b = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, chat_b);

	UAM_ASSERT_EQ((tmp_a / "chats").lexically_normal().generic_string(), resolved_a.lexically_normal().generic_string());
	UAM_ASSERT_EQ((tmp_b / "chats").lexically_normal().generic_string(), resolved_b.lexically_normal().generic_string());
}

UAM_TEST(TestResolveNativeHistoryChatsDirReturnsNulloptForMissingTmpDir)
{
	TempDir gemini_home("uam-native-dir-empty-home");
	TempDir project("uam-native-dir-project");

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	const std::optional<fs::path> resolved = ChatHistorySyncService().ResolveNativeHistoryChatsDirForWorkspace(project.root);
	UAM_ASSERT(!resolved.has_value());
}

UAM_TEST(TestPendingCallCompletionUsesLaunchTimeProviderSnapshot)
{
	TempDir data_root("uam-pending-call-data");
	TempDir gemini_home("uam-pending-call-gemini-home");
	TempDir project("uam-pending-call-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "session-202", {{"user", "queued prompt"}, {"assistant", "native reply"}});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	uam::AppState app = MakeTestAppState(data_root.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", project.root.string(), false});

	ChatSession draft;
	draft.id = "chat-202";
	draft.provider_id = "gemini-cli";
	draft.folder_id = "folder-a";
	draft.title = "Draft";
	draft.created_at = "2026-03-21 10:00:00";
	draft.updated_at = "2026-03-21 10:00:01";
	draft.messages.push_back(Message{MessageRole::User, "queued prompt", "2026-03-21 10:00:01"});
	app.chats.push_back(draft);
	app.selected_chat_index = 0;
	app.settings.active_provider_id = "gemini-cli";

	PendingRuntimeCall call;
	call.chat_id = draft.id;
	call.provider_id_snapshot = "gemini-cli";
	call.native_history_chats_dir_snapshot = chats_dir.string();
	call.state = std::make_shared<AsyncProcessTaskState>();
	call.state->result.output = "raw local fallback output";
	call.state->completed.store(true, std::memory_order_release);
	app.pending_calls.push_back(std::move(call));

	PollPendingRuntimeCall(app);

	UAM_ASSERT(app.pending_calls.empty());
	UAM_ASSERT_EQ(std::string("Provider response synced from native session."), app.status_line);
	UAM_ASSERT_EQ(1u, app.chats.size());
	UAM_ASSERT_EQ(std::string("session-202"), app.chats.front().id);
	UAM_ASSERT_EQ(std::string("gemini-cli"), app.chats.front().provider_id);
	UAM_ASSERT_EQ(2u, app.chats.front().messages.size());
	UAM_ASSERT_EQ(std::string("native reply"), app.chats.front().messages.back().content);
	UAM_ASSERT_EQ(0, app.selected_chat_index);
}

UAM_TEST(TestEnsureCliTerminalForChatKeepsDistinctTerminalPerChat)
{
	TempDir data_root("uam-cli-terminal-distinct-data");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession first;
	first.id = "chat-a";
	first.title = "Chat A";
	first.provider_id = "gemini-cli";
	first.created_at = "2026-04-12 10:00:00";
	first.updated_at = "2026-04-12 10:00:00";

	ChatSession second;
	second.id = "chat-b";
	second.title = "Chat B";
	second.provider_id = "gemini-cli";
	second.created_at = "2026-04-12 10:01:00";
	second.updated_at = "2026-04-12 10:01:00";

	app.chats.push_back(first);
	app.chats.push_back(second);

	uam::CliTerminalState& first_terminal = EnsureCliTerminalForChat(app, app.chats[0]);
	uam::CliTerminalState& second_terminal = EnsureCliTerminalForChat(app, app.chats[1]);

	UAM_ASSERT_EQ(2u, app.cli_terminals.size());
	UAM_ASSERT(&first_terminal != &second_terminal);
	UAM_ASSERT_EQ(std::string("term-chat-a"), first_terminal.terminal_id);
	UAM_ASSERT_EQ(std::string("term-chat-b"), second_terminal.terminal_id);
	UAM_ASSERT_EQ(std::string("chat-a"), first_terminal.frontend_chat_id);
	UAM_ASSERT_EQ(std::string("chat-b"), second_terminal.frontend_chat_id);
	UAM_ASSERT_EQ(std::string("chat-a"), first_terminal.attached_chat_id);
	UAM_ASSERT_EQ(std::string("chat-b"), second_terminal.attached_chat_id);
}

UAM_TEST(TestStateSerializerIncludesDistinctCliDebugInventory)
{
	TempDir data_root("uam-cli-debug-state-data");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession first;
	first.id = "chat-a";
	first.title = "Chat A";
	first.provider_id = "gemini-cli";
	first.native_session_id = "session-a";
	first.created_at = "2026-04-12 10:00:00";
	first.updated_at = "2026-04-12 10:00:00";

	ChatSession second;
	second.id = "chat-b";
	second.title = "Chat B";
	second.provider_id = "gemini-cli";
	second.native_session_id = "session-b";
	second.created_at = "2026-04-12 10:01:00";
	second.updated_at = "2026-04-12 10:01:00";

	app.chats.push_back(first);
	app.chats.push_back(second);
	app.selected_chat_index = 0;

	uam::CliTerminalState& first_terminal = EnsureCliTerminalForChat(app, app.chats[0]);
	first_terminal.running = true;
	first_terminal.ui_attached = true;
	first_terminal.attached_session_id = "session-a";
	first_terminal.last_user_input_time_s = 10.0;
	first_terminal.last_ai_output_time_s = 12.0;

	uam::CliTerminalState& second_terminal = EnsureCliTerminalForChat(app, app.chats[1]);
	second_terminal.running = true;
	second_terminal.ui_attached = false;
	second_terminal.attached_session_id = "session-b";
	second_terminal.turn_state = uam::CliTerminalTurnState::Busy;
	second_terminal.generation_in_progress = true;
	second_terminal.last_user_input_time_s = 20.0;
	second_terminal.last_ai_output_time_s = 25.0;

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);

	UAM_ASSERT(serialized.contains("cliDebug"));
	const nlohmann::json& cli_debug = serialized["cliDebug"];
	UAM_ASSERT_EQ(2u, cli_debug["terminalCount"].get<std::size_t>());
	UAM_ASSERT_EQ(2u, cli_debug["runningTerminalCount"].get<std::size_t>());
	UAM_ASSERT_EQ(1u, cli_debug["busyTerminalCount"].get<std::size_t>());
	UAM_ASSERT_EQ(std::string("chat-a"), cli_debug["selectedChatId"].get<std::string>());
	UAM_ASSERT(cli_debug["terminals"].is_array());
	UAM_ASSERT_EQ(2u, cli_debug["terminals"].size());

	const nlohmann::json& first_debug = cli_debug["terminals"][0];
	const nlohmann::json& second_debug = cli_debug["terminals"][1];

	UAM_ASSERT_EQ(std::string("term-chat-a"), first_debug["terminalId"].get<std::string>());
	UAM_ASSERT_EQ(std::string("chat-a"), first_debug["frontendChatId"].get<std::string>());
	UAM_ASSERT_EQ(std::string("session-a"), first_debug["attachedSessionId"].get<std::string>());
	UAM_ASSERT_EQ(std::string("idle"), first_debug["turnState"].get<std::string>());

	UAM_ASSERT_EQ(std::string("term-chat-b"), second_debug["terminalId"].get<std::string>());
	UAM_ASSERT_EQ(std::string("chat-b"), second_debug["frontendChatId"].get<std::string>());
	UAM_ASSERT_EQ(std::string("session-b"), second_debug["attachedSessionId"].get<std::string>());
	UAM_ASSERT_EQ(std::string("busy"), second_debug["turnState"].get<std::string>());

	UAM_ASSERT(serialized["chats"].is_array());
	UAM_ASSERT_EQ(2u, serialized["chats"].size());
	UAM_ASSERT_EQ(std::string("term-chat-a"), serialized["chats"][0]["cliTerminal"]["terminalId"].get<std::string>());
	UAM_ASSERT_EQ(std::string("term-chat-b"), serialized["chats"][1]["cliTerminal"]["terminalId"].get<std::string>());
}

UAM_TEST(TestRemoveChatUsesDeletedChatProviderForNativeCleanup)
{
	TempDir data_root("uam-remove-chat-data");
	TempDir gemini_home("uam-remove-chat-gemini-home");
	TempDir project("uam-remove-chat-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "session-delete", {{"user", "delete me"}, {"assistant", "done"}});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	uam::AppState app = MakeTestAppState(data_root.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", project.root.string(), false});
	app.settings.active_provider_id = "gemini-cli";

	ChatSession chat;
	chat.id = "session-delete";
	chat.provider_id = "gemini-cli";
	chat.folder_id = "folder-a";
	chat.native_session_id = "session-delete";
	chat.title = "Delete Me";
	chat.created_at = "2026-03-21 10:00:00";
	chat.updated_at = "2026-03-21 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "delete me", "2026-03-21 10:00:01"});
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	std::error_code delete_ec;
	UAM_ASSERT(ChatHistorySyncService().DeleteNativeSessionFileForChat(app, chat, &delete_ec));
	UAM_ASSERT(!delete_ec);
	WaitForFileMissing(chats_dir / "session-delete.json");
	UAM_ASSERT(!fs::exists(chats_dir / "session-delete.json"));
}

UAM_TEST(TestRemoveChatDeletesUamJsonMetadataFile)
{
	TempDir data_root("uam-remove-chat-metadata-data");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession chat;
	chat.id = "chat-to-delete";
	chat.title = "Delete Me";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	// Manually create the UAM JSON file
	const std::filesystem::path json_path = AppPaths::UamChatFilePath(data_root.root, chat.id);
	std::filesystem::create_directories(json_path.parent_path());
	std::ofstream f(json_path);
	f << "{}";
	f.close();

	UAM_ASSERT(std::filesystem::exists(json_path));

	// Perform deletion
	UAM_ASSERT(RemoveChatById(app, chat.id));

	// Confirm file is gone
	UAM_ASSERT(!std::filesystem::exists(json_path));
}

UAM_TEST(TestRemoveChatDeletesPersistedArtifactsAndStaysGoneAfterReload)
{
	TempDir data_root("uam-remove-chat-reload-data");
	TempDir gemini_home("uam-remove-chat-reload-gemini-home");
	TempDir project("uam-remove-chat-reload-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "session-delete-reload", {{"user", "delete me"}, {"assistant", "done"}});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	ChatSession chat;
	chat.id = "session-delete-reload";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "session-delete-reload";
	chat.folder_id = "folder-a";
	chat.title = "Delete Me";
	chat.created_at = "2026-03-21 10:00:00";
	chat.updated_at = "2026-03-21 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "delete me", "2026-03-21 10:00:01"});
	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));

	const fs::path chat_root = AppPaths::ChatPath(data_root.root, chat.id);
	fs::create_directories(chat_root / "messages");
	UAM_ASSERT(WriteTextFile(chat_root / "messages" / "user.txt", "legacy local artifact"));

	const fs::path json_path = AppPaths::UamChatFilePath(data_root.root, chat.id);
	UAM_ASSERT(fs::exists(json_path));
	UAM_ASSERT(fs::exists(chat_root));
	UAM_ASSERT(fs::exists(chats_dir / "session-delete-reload.json"));

	uam::AppState app = MakeTestAppState(data_root.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", project.root.string(), false});
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	UAM_ASSERT(RemoveChatById(app, chat.id));
	UAM_ASSERT_EQ(-1, app.selected_chat_index);
	WaitForFileMissing(json_path);
	WaitForFileMissing(chats_dir / "session-delete-reload.json");
	UAM_ASSERT(!fs::exists(json_path));
	UAM_ASSERT(!fs::exists(chat_root));
	UAM_ASSERT(!fs::exists(chats_dir / "session-delete-reload.json"));

	uam::AppState reloaded = MakeTestAppState(data_root.root);
	ChatHistorySyncService().LoadSidebarChats(reloaded);
	UAM_ASSERT(reloaded.chats.empty());
}

UAM_TEST(TestRemoveChatDeletesNativeSessionFileWhenSessionIdLooksLikeLocalDraft)
{
	TempDir data_root("uam-remove-chat-draftlike-native-data");
	TempDir gemini_home("uam-remove-chat-draftlike-native-gemini-home");
	TempDir project("uam-remove-chat-draftlike-native-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "chat-1775856499765-f3739f", {});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	uam::AppState app = MakeTestAppState(data_root.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", project.root.string(), false});
	app.settings.active_provider_id = "gemini-cli";

	ChatSession chat;
	chat.id = "chat-1775856499765-f3739f";
	chat.provider_id = "gemini-cli";
	chat.folder_id = "folder-a";
	chat.native_session_id = "chat-1775856499765-f3739f";
	chat.title = "Session 2026-04-10 22:28:19";
	chat.created_at = "2026-04-10 22:28:19";
	chat.updated_at = "2026-04-10 22:28:19";
	chat.workspace_directory = project.root.string();
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	UAM_ASSERT(RemoveChatById(app, chat.id));
	WaitForFileMissing(chats_dir / "chat-1775856499765-f3739f.json");
	UAM_ASSERT(!fs::exists(chats_dir / "chat-1775856499765-f3739f.json"));

	uam::AppState reloaded = MakeTestAppState(data_root.root);
	ChatDomainService().EnsureDefaultFolder(reloaded);
	ChatHistorySyncService().LoadSidebarChatsByDiscovery(reloaded);
	UAM_ASSERT(reloaded.chats.empty());
}

UAM_TEST(TestRemoveChatDeletesNativeSessionViaDiscoveryFallbackWhenWorkspaceDrifts)
{
	TempDir data_root("uam-remove-chat-fallback-data");
	TempDir gemini_home("uam-remove-chat-fallback-gemini-home");
	TempDir project("uam-remove-chat-fallback-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "session-delete-fallback", {{"user", "delete me"}, {"assistant", "done"}});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	ChatSession chat;
	chat.id = "session-delete-fallback";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "session-delete-fallback";
	chat.folder_id = uam::constants::kDefaultFolderId;
	chat.workspace_directory = (project.root.parent_path() / "wrong-workspace").string();
	chat.title = "Delete Me";
	chat.created_at = "2026-03-21 10:00:00";
	chat.updated_at = "2026-03-21 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "delete me", "2026-03-21 10:00:01"});
	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));

	uam::AppState app = MakeTestAppState(data_root.root);
	ChatDomainService().EnsureDefaultFolder(app);
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	UAM_ASSERT(RemoveChatById(app, chat.id));
	WaitForFileMissing(chats_dir / "session-delete-fallback.json");
	UAM_ASSERT(!fs::exists(chats_dir / "session-delete-fallback.json"));

	uam::AppState reloaded = MakeTestAppState(data_root.root);
	ChatDomainService().EnsureDefaultFolder(reloaded);
	ChatHistorySyncService().LoadSidebarChatsByDiscovery(reloaded);
	UAM_ASSERT(reloaded.chats.empty());
}

UAM_TEST(TestDeleteNativeSessionFileForChatReturnsFalseWhenRemoveFails)
{
	TempDir data_root("uam-remove-chat-delete-error-data");
	TempDir gemini_home("uam-remove-chat-delete-error-gemini-home");
	TempDir project("uam-remove-chat-delete-error-project");

	const fs::path tmp_dir = gemini_home.root / "tmp" / "release";
	const fs::path chats_dir = tmp_dir / "chats";
	fs::create_directories(chats_dir);
	WriteNativeProjectRoot(tmp_dir, project.root);
	WriteGeminiNativeSession(chats_dir, "session-delete-error", {{"user", "delete me"}, {"assistant", "done"}});

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	uam::AppState app = MakeTestAppState(data_root.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", project.root.string(), false});
	app.settings.active_provider_id = "gemini-cli";

	ChatSession chat;
	chat.id = "session-delete-error";
	chat.provider_id = "gemini-cli";
	chat.folder_id = "folder-a";
	chat.native_session_id = "session-delete-error";

#if defined(_WIN32)
	std::error_code delete_ec;
	UAM_ASSERT(ChatHistorySyncService().DeleteNativeSessionFileForChat(app, chat, &delete_ec));
	UAM_ASSERT(!delete_ec);
#else
	fs::permissions(chats_dir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

	std::error_code delete_ec;
	const bool deleted = ChatHistorySyncService().DeleteNativeSessionFileForChat(app, chat, &delete_ec);
	fs::permissions(chats_dir, fs::perms::owner_all, fs::perm_options::replace);

	UAM_ASSERT(!deleted);
	UAM_ASSERT(static_cast<bool>(delete_ec));
	UAM_ASSERT(fs::exists(chats_dir / "session-delete-error.json"));
#endif
}

UAM_TEST(TestProcessServiceExecuteCommandTimesOut)
{
#if defined(_WIN32)
	const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("powershell -Command \"Start-Sleep -Seconds 2\"", 100);
#else
	const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("sleep 2", 100);
#endif

	UAM_ASSERT(result.timed_out);
	UAM_ASSERT(!result.ok);
}

UAM_TEST(TestChatBranchingReparentChildrenAfterDelete)
{
	std::vector<ChatSession> chats;

	ChatSession root;
	root.id = "root";
	root.parent_chat_id = "";
	root.branch_root_chat_id = "root";
	root.branch_from_message_index = -1;
	chats.push_back(root);

	ChatSession child_a;
	child_a.id = "child-a";
	child_a.parent_chat_id = "root";
	child_a.branch_root_chat_id = "root";
	child_a.branch_from_message_index = 1;
	chats.push_back(child_a);

	ChatSession child_b;
	child_b.id = "child-b";
	child_b.parent_chat_id = "child-a";
	child_b.branch_root_chat_id = "root";
	child_b.branch_from_message_index = 2;
	chats.push_back(child_b);

	ChatBranching::ReparentChildrenAfterDelete(chats, "root");

	ChatBranching::Normalize(chats);
	const auto find_by_id = [&](const std::string& id) -> const ChatSession*
	{
		for (const ChatSession& chat : chats)
		{
			if (chat.id == id)
			{
				return &chat;
			}
		}

		return nullptr;
	};

	const ChatSession* a = find_by_id("child-a");
	const ChatSession* b = find_by_id("child-b");
	UAM_ASSERT(a != nullptr);
	UAM_ASSERT(b != nullptr);
	UAM_ASSERT_EQ(std::string(""), a->parent_chat_id);
	UAM_ASSERT_EQ(std::string("child-a"), a->branch_root_chat_id);
	UAM_ASSERT_EQ(std::string("child-a"), b->branch_root_chat_id);
}

UAM_TEST(TestRagIndexServiceIndexesRetrievesAndCites)
{
	TempDir workspace("uam-rag-workspace");
	UAM_ASSERT(WriteTextFile(workspace.root / "alpha.txt", "First line about deployment\n"
	                                                       "Second line about branches\n"
	                                                       "Third line about release notes\n"));
	UAM_ASSERT(WriteTextFile(workspace.root / "beta.txt", "A different topic with infra setup\n"
	                                                      "No mention of deployment here\n"));

	RagIndexService::Config config;
	config.top_k = 3;
	config.max_snippet_chars = 240;
	RagIndexService rag(config);

	const RagRefreshResult refresh = rag.RefreshIndexIncremental(workspace.root);
	UAM_ASSERT(refresh.ok);
	const std::vector<RagSnippet> snippets = rag.RetrieveTopK(workspace.root, "deployment branches");
	UAM_ASSERT(!snippets.empty());
	UAM_ASSERT(!snippets.front().relative_path.empty());
	UAM_ASSERT(snippets.front().start_line >= 1);
	UAM_ASSERT(snippets.front().end_line >= snippets.front().start_line);
}

UAM_TEST(TestRagIndexServiceIncrementalRefreshDetectsChanges)
{
	TempDir workspace("uam-rag-incremental");
	const fs::path file = workspace.root / "notes.txt";
	UAM_ASSERT(WriteTextFile(file, "line one\nline two\n"));

	RagIndexService rag;
	RagRefreshResult first = rag.RefreshIndexIncremental(workspace.root);
	UAM_ASSERT(first.ok);
	UAM_ASSERT(first.indexed_files >= 1);

	RagRefreshResult second = rag.RefreshIndexIncremental(workspace.root);
	UAM_ASSERT(second.ok);
	UAM_ASSERT(second.updated_files == 0 || second.updated_files == 1);

	{
		std::error_code ec;
		const fs::file_time_type current_mtime = fs::last_write_time(file, ec);
		UAM_ASSERT(!ec);
		fs::last_write_time(file, current_mtime + std::chrono::seconds(2), ec);
		UAM_ASSERT(!ec);
	}

	RagRefreshResult third = rag.RefreshIndexIncremental(workspace.root);
	UAM_ASSERT(third.ok);
	UAM_ASSERT(third.updated_files == 0 || third.updated_files == 1);

	UAM_ASSERT(WriteTextFile(file, "line one\nline two changed\n"));
	RagRefreshResult fourth = rag.RefreshIndexIncremental(workspace.root);
	UAM_ASSERT(fourth.ok);
	UAM_ASSERT(fourth.updated_files >= 1);
}

UAM_TEST(TestRagIndexServiceFiltersBinaryAndLargeFiles)
{
	TempDir workspace("uam-rag-filtering");
	UAM_ASSERT(WriteTextFile(workspace.root / "keep.txt", "search token stays here\n"));
	{
		std::ofstream binary(workspace.root / "binary.bin", std::ios::binary | std::ios::trunc);
		binary << "abc";
		binary.put('\0');
		binary << "def";
	}

	UAM_ASSERT(WriteTextFile(workspace.root / "huge.txt", std::string(1024, 'x')));

	RagIndexService::Config config;
	config.max_file_bytes = 128;
	RagIndexService rag(config);

	const RagRefreshResult refresh = rag.RefreshIndexIncremental(workspace.root);
	UAM_ASSERT(refresh.ok);
	const std::vector<RagSnippet> snippets = rag.RetrieveTopK(workspace.root, "token");
	UAM_ASSERT_EQ(1u, snippets.size());
	UAM_ASSERT_EQ(std::string("keep.txt"), snippets.front().relative_path);
}

UAM_TEST(TestRagIndexServiceRetrievalOrderingAndTopK)
{
	TempDir workspace("uam-rag-ordering");
	UAM_ASSERT(WriteTextFile(workspace.root / "a.txt", "token token alpha\n"));
	UAM_ASSERT(WriteTextFile(workspace.root / "b.txt", "token token beta\n"));
	UAM_ASSERT(WriteTextFile(workspace.root / "c.txt", "token gamma\n"));

	RagIndexService::Config config;
	config.vector_enabled = false;
	config.top_k = 2;
	RagIndexService rag(config);
	const RagRefreshResult refresh = rag.RefreshIndexIncremental(workspace.root);
	UAM_ASSERT(refresh.ok);

	const std::vector<RagSnippet> first = rag.RetrieveTopK(workspace.root, "token");
	const std::vector<RagSnippet> second = rag.RetrieveTopK(workspace.root, "token");
	UAM_ASSERT_EQ(2u, first.size());
	UAM_ASSERT_EQ(first.size(), second.size());
	UAM_ASSERT_EQ(std::string("a.txt"), first[0].relative_path);
	UAM_ASSERT_EQ(std::string("b.txt"), first[1].relative_path);
	UAM_ASSERT_EQ(first[0].relative_path, second[0].relative_path);
	UAM_ASSERT_EQ(first[1].relative_path, second[1].relative_path);
}

UAM_TEST(TestOllamaEngineInterfaceLifecycle)
{
	TempDir model_dir("uam-vector-rag-models");
	UAM_ASSERT(WriteTextFile(model_dir.root / "alpha.gguf", "alpha"));
	UAM_ASSERT(WriteTextFile(model_dir.root / "beta.gguf", "beta"));

	OllamaEngineClient engine;
	engine.SetModelFolder(model_dir.root);
	engine.SetEmbeddingDimensions(128);

	const std::vector<std::string> models = engine.ListModels();
	UAM_ASSERT_EQ(2u, models.size());
	UAM_ASSERT_EQ(std::string("alpha.gguf"), models[0]);
	UAM_ASSERT_EQ(std::string("beta.gguf"), models[1]);

	const ollama_engine::CurrentStateResponse initial = engine.QueryCurrentState();
	UAM_ASSERT(initial.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Idle);

	std::string load_error;
	UAM_ASSERT(!engine.Load("missing.gguf", &load_error));
	UAM_ASSERT(!load_error.empty());
	bool saw_loading = false;
	bool load_ok = false;
	std::thread load_worker([&]() { load_ok = engine.Load("alpha.gguf", &load_error); });

	for (int i = 0; i < 80; ++i)
	{
		const ollama_engine::CurrentStateResponse state = engine.QueryCurrentState();

		if (state.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Loading && state.pOptLoadingStructure.has_value())
		{
			saw_loading = true;
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	load_worker.join();

	if (!load_ok)
	{
		// When llama.cpp backend is enabled, invalid/fake GGUF fixtures are expected to fail.
		return;
	}

	UAM_ASSERT(load_ok);
	UAM_ASSERT(saw_loading);

	const ollama_engine::CurrentStateResponse loaded = engine.QueryCurrentState();
	UAM_ASSERT(loaded.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Loaded);
	UAM_ASSERT_EQ(std::string("alpha.gguf"), loaded.pSLoadedModelName);

	bool saw_active_generation = false;
	bool saw_finished = false;
	ollama_engine::SendMessageResponse message_response;
	std::thread worker([&]() { message_response = engine.SendMessage(std::string(600, 'x')); });

	for (int i = 0; i < 300; ++i)
	{
		const ollama_engine::CurrentStateResponse state = engine.QueryCurrentState();

		if (state.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Running || state.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Thinking)
		{
			saw_active_generation = true;
		}

		if (state.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Finished)
		{
			saw_finished = true;
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	worker.join();

	UAM_ASSERT(saw_active_generation);
	UAM_ASSERT(saw_finished);
	UAM_ASSERT(message_response.pbOk);
	UAM_ASSERT(message_response.pVecfEmbedding.has_value());
	UAM_ASSERT_EQ(128u, message_response.pVecfEmbedding->size());
	const ollama_engine::CurrentStateResponse done = engine.QueryCurrentState();
	UAM_ASSERT(done.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Loaded);
}

UAM_TEST(TestRagIndexServiceVectorRetrievalWithEngineModel)
{
	TempDir workspace("uam-rag-vector");
	TempDir model_dir("uam-rag-vector-models");
	UAM_ASSERT(WriteTextFile(model_dir.root / "mini.gguf", "fake-model"));
	UAM_ASSERT(WriteTextFile(workspace.root / "deployment.md", "How to deploy service A\n"
	                                                           "Use canary migrations and rollout checks\n"));
	UAM_ASSERT(WriteTextFile(workspace.root / "notes.txt", "Team lunch plans\n"
	                                                       "No production rollout guidance here\n"));

	RagIndexService::Config config;
	config.vector_enabled = true;
	config.vector_dimensions = 96;
	config.top_k = 2;
	RagIndexService rag(config);
	rag.SetModelFolder(model_dir.root);

	const std::vector<std::string> models = rag.ListModels();
	UAM_ASSERT_EQ(1u, models.size());

	if (!rag.LoadModel("mini.gguf"))
	{
		// Skip on strict backends that require valid GGUF model binaries.
		return;
	}

	const RagRefreshResult refresh = rag.RefreshIndexIncremental(workspace.root);
	UAM_ASSERT(refresh.ok);

	const std::vector<RagSnippet> snippets = rag.RetrieveTopK(workspace.root, "canary rollout migration");
	UAM_ASSERT(!snippets.empty());
	UAM_ASSERT_EQ(std::string("deployment.md"), snippets.front().relative_path);
}

UAM_TEST(TestVcsWorkspaceServiceHandlesNonRepoWorkspace)
{
	TempDir workspace("uam-vcs-none");
	UAM_ASSERT(WriteTextFile(workspace.root / "file.txt", "hello\n"));

	const VcsRepoType repo = VcsWorkspaceService::DetectRepo(workspace.root);
	UAM_ASSERT(repo == VcsRepoType::None);

	VcsSnapshot snapshot;
	const VcsCommandResult snapshot_result = VcsWorkspaceService::ReadSnapshot(workspace.root, snapshot);
	UAM_ASSERT(!snapshot_result.ok || snapshot.repo_type == VcsRepoType::Svn);

	const VcsCommandResult status = VcsWorkspaceService::ReadStatus(workspace.root);
	UAM_ASSERT(!status.ok || !status.output.empty());
}

UAM_TEST(TestVcsWorkspaceServiceAppliesOutputCapsAndTimeout)
{
#if defined(_WIN32)
	return;
#else
	TempDir workspace("uam-vcs-fake-workspace");
	TempDir fake_bin("uam-vcs-fake-bin");
	const fs::path svn_binary = fake_bin.root / "svn";

	std::ostringstream script;
	script << "#!/bin/sh\n";
	script << "cmd=\"$1\"\n";
	script << "case \"$cmd\" in\n";
	script << "  info)\n";
	script << "    echo \"Working Copy Root Path: " << workspace.root.generic_string() << "\"\n";
	script << "    echo \"URL: https://example.com/svn/repo/branches/feature-x\"\n";
	script << "    echo \"Revision: 123\"\n";
	script << "    echo \"Relative URL: ^/branches/feature-x\"\n";
	script << "    ;;\n";
	script << "  status)\n";
	script << "    i=0\n";
	script << "    while [ \"$i\" -lt 20000 ]; do\n";
	script << "      echo \"M       file_$i.txt\"\n";
	script << "      i=$((i + 1))\n";
	script << "    done\n";
	script << "    ;;\n";
	script << "  diff)\n";
	script << "    sleep 8\n";
	script << "    echo \"delayed diff\"\n";
	script << "    ;;\n";
	script << "  log)\n";
	script << "    echo \"r123 | test | 2026-03-21\"\n";
	script << "    ;;\n";
	script << "  *)\n";
	script << "    echo \"unknown\" >&2\n";
	script << "    exit 1\n";
	script << "    ;;\n";
	script << "esac\n";
	script << "exit 0\n";
	UAM_ASSERT(WriteTextFile(svn_binary, script.str()));
	std::error_code chmod_ec;
	fs::permissions(svn_binary, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec, fs::perm_options::replace, chmod_ec);
	UAM_ASSERT(!chmod_ec);

	std::string path_value = fake_bin.root.string();

	if (const char* existing_path = std::getenv("PATH"))
	{
		path_value += ":";
		path_value += existing_path;
	}

	ScopedEnvVar path_override("PATH", path_value);

	UAM_ASSERT(VcsWorkspaceService::DetectRepo(workspace.root) == VcsRepoType::Svn);

	VcsSnapshot snapshot;
	const VcsCommandResult snapshot_result = VcsWorkspaceService::ReadSnapshot(workspace.root, snapshot);
	UAM_ASSERT(snapshot_result.ok);
	UAM_ASSERT(snapshot.repo_type == VcsRepoType::Svn);
	UAM_ASSERT_EQ(std::string("123"), snapshot.revision);
	UAM_ASSERT_EQ(std::string("/branches/feature-x"), snapshot.branch_path);

	const VcsCommandResult status = VcsWorkspaceService::ReadStatus(workspace.root);
	UAM_ASSERT(status.ok);
	UAM_ASSERT(status.truncated);
	UAM_ASSERT(status.output.find("[Output truncated due to size limit.]") != std::string::npos);

	const auto start = std::chrono::steady_clock::now();
	const VcsCommandResult diff = VcsWorkspaceService::ReadDiff(workspace.root);
	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	UAM_ASSERT(!diff.ok);
	UAM_ASSERT(diff.timed_out);
	UAM_ASSERT(elapsed_ms < 7800);
#endif
}

UAM_TEST(TestMarkdownTemplateCatalogImportCollisionAndFiltering)
{
	TempDir global_root("uam-template-global-root");
	TempDir source_a("uam-template-source-a");
	TempDir source_b("uam-template-source-b");

	const fs::path file_a = source_a.root / "gemini.md";
	const fs::path file_b = source_b.root / "gemini.md";
	const fs::path non_markdown = source_b.root / "ignore.txt";
	UAM_ASSERT(WriteTextFile(file_a, "# A\n"));
	UAM_ASSERT(WriteTextFile(file_b, "# B\n"));
	UAM_ASSERT(WriteTextFile(non_markdown, "skip"));

	std::string imported_a;
	std::string imported_b;
	std::string error;
	UAM_ASSERT(MarkdownTemplateCatalog::ImportMarkdownTemplate(global_root.root, file_a, &imported_a, &error));
	UAM_ASSERT(MarkdownTemplateCatalog::ImportMarkdownTemplate(global_root.root, file_b, &imported_b, &error));
	UAM_ASSERT(imported_a != imported_b);

	UAM_ASSERT(!MarkdownTemplateCatalog::ImportMarkdownTemplate(global_root.root, non_markdown, nullptr, &error));

	const fs::path catalog_path = MarkdownTemplateCatalog::CatalogPath(global_root.root);
	UAM_ASSERT(WriteTextFile(catalog_path / "not-a-template.txt", "ignored"));

	const std::vector<TemplateCatalogEntry> entries = MarkdownTemplateCatalog::List(global_root.root);
	UAM_ASSERT_EQ(2u, entries.size());
	UAM_ASSERT(MarkdownTemplateCatalog::HasTemplate(global_root.root, imported_a));
	UAM_ASSERT(MarkdownTemplateCatalog::HasTemplate(global_root.root, imported_b));
}

UAM_TEST(TestProviderRuntimeInteractiveArgvIncludesResumeAndFlags)
{
	AppSettings settings;
	settings.provider_yolo_mode = true;
	settings.provider_extra_flags = "--profile nightly --dry-run";

	ChatSession chat;
	chat.native_session_id = "session-123";

	ProviderProfile profile;
	profile.interactive_command = "gemini";
	profile.supports_resume = true;
	profile.resume_argument = "-r";

	const std::vector<std::string> argv = provider_runtime_internal::BuildInteractiveArgv(profile, chat, settings);
	const std::vector<std::string> expected = {"gemini", "--yolo", "--profile", "nightly", "--dry-run", "-r", "session-123"};

	UAM_ASSERT(argv == expected);
}

UAM_TEST(TestProviderRuntimeParsesWindowsPathFlags)
{
#if defined(_WIN32)
	AppSettings settings;
	settings.provider_extra_flags = R"(--config "C:\Users\david\gemini\settings.json" --profile nightly)";

	ChatSession chat;
	ProviderProfile profile;
	profile.interactive_command = "gemini";
	profile.supports_resume = false;
	profile.resume_argument = "";

	const std::vector<std::string> argv = provider_runtime_internal::BuildInteractiveArgv(profile, chat, settings);
	const std::vector<std::string> expected = {"gemini", "--config", R"(C:\Users\david\gemini\settings.json)", "--profile", "nightly"};

	UAM_ASSERT(argv == expected);
#endif
}

UAM_TEST(TestProviderProfileStoreBuiltInProfiles)
{
	const std::vector<ProviderProfile> profiles = ProviderProfileStore::BuiltInProfiles();
	const ProviderProfile* gemini_cli = ProviderProfileStore::FindById(profiles, "gemini-cli");
	UAM_ASSERT(ProviderProfileStore::FindById(profiles, "gemini") == nullptr);
	UAM_ASSERT_EQ(1u, profiles.size());
	UAM_ASSERT(gemini_cli != nullptr);
	UAM_ASSERT_EQ(std::string("cli"), gemini_cli->output_mode);
	UAM_ASSERT_EQ(std::string("gemini -r {resume} {flags} {prompt}"), gemini_cli->command_template);
	UAM_ASSERT(gemini_cli->supports_interactive);
}

UAM_TEST(TestProviderRuntimeBuildCommandUsesGeminiCliPromptTemplate)
{
	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	AppSettings settings;
	settings.provider_command_template = "gemini -r {resume} {flags} {prompt}";
	settings.provider_extra_flags = "--alpha --beta";

	const std::string command = ProviderRuntime::BuildCommand(profile, settings, "Review this patch", {}, "resume-42");
	UAM_ASSERT_EQ(std::string("gemini -r 'resume-42' '--alpha' '--beta' 'Review this patch'"), command);
}

UAM_TEST(TestProviderRuntimeParsesWindowsInteractiveCommandPath)
{
#if defined(_WIN32)
	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	profile.output_mode = "cli";
	profile.supports_interactive = true;
	profile.interactive_command = R"("C:\Program Files\Gemini\gemini.exe" --interactive)";
	profile.supports_resume = false;
	profile.resume_argument.clear();

	AppSettings settings;
	ChatSession chat;
	const std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	const std::vector<std::string> expected = {R"(C:\Program Files\Gemini\gemini.exe)", "--interactive"};

	UAM_ASSERT(argv == expected);
#endif
}

UAM_TEST(TestProviderRuntimeOutputModeHelpers)
{
	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	profile.id = "custom-runtime";
	UAM_ASSERT(ProviderRuntime::UsesCliOutput(profile));
	UAM_ASSERT(!ProviderRuntime::UsesStructuredOutput(profile));
}

UAM_TEST(TestProviderRuntimeBuiltInPolicyLocks)
{
	ProviderProfile gemini_cli = ProviderProfileStore::DefaultGeminiProfile();
	UAM_ASSERT(ProviderRuntime::SupportsGeminiJsonHistory(gemini_cli));
	UAM_ASSERT(ProviderRuntime::UsesCliOutput(gemini_cli));
	UAM_ASSERT(ProviderRuntime::UsesGeminiPathBootstrap(gemini_cli));
}

UAM_TEST(TestProviderRuntimeRoleMappingHonorsProfileTypes)
{
	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	profile.user_message_types = {"human", "user"};
	profile.assistant_message_types = {"assistant", "model", "gemini", "bot"};

	UAM_ASSERT(ProviderRuntime::RoleFromNativeType(profile, "human") == MessageRole::User);
	UAM_ASSERT(ProviderRuntime::RoleFromNativeType(profile, "BOT") == MessageRole::Assistant);
	UAM_ASSERT(ProviderRuntime::RoleFromNativeType(profile, "tool") == MessageRole::System);
}

UAM_TEST(TestProviderRuntimeMergesProfileFlags)
{
	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	profile.id = "custom-runtime";
	profile.history_adapter = "local-only";
	profile.runtime_flags = {"--profile", "nightly"};

	AppSettings settings;
	settings.provider_extra_flags = "--dry-run";
	settings.provider_command_template = "gemini {flags} {prompt}";

	const std::string command = ProviderRuntime::BuildCommand(profile, settings, "hello", std::vector<std::string>{}, std::string{});
	UAM_ASSERT(command.find("--profile") != std::string::npos);
	UAM_ASSERT(command.find("nightly") != std::string::npos);
	UAM_ASSERT(command.find("--dry-run") != std::string::npos);
}

UAM_TEST(TestProviderRuntimeRegistryMapsBuiltInIds)
{
	const std::vector<std::string> ids = {"gemini", "gemini-cli"};

	for (const std::string& id : ids)
	{
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(id);
		UAM_ASSERT_EQ(std::string("gemini-cli"), std::string(runtime.RuntimeId()));
		UAM_ASSERT(ProviderRuntimeRegistry::IsKnownRuntimeId(id));
	}

	UAM_ASSERT(!ProviderRuntimeRegistry::IsKnownRuntimeId("custom-runtime"));
}

UAM_TEST(TestProviderRuntimeFamilyBaseTypes)
{
	const IProviderRuntime& gemini_cli = ProviderRuntimeRegistry::ResolveById("gemini-cli");
	UAM_ASSERT(dynamic_cast<const GeminiCliProviderRuntime*>(&gemini_cli) != nullptr);
	UAM_ASSERT(dynamic_cast<const GeminiCliProviderRuntime*>(&ProviderRuntimeRegistry::ResolveById("gemini")) != nullptr);
}

UAM_TEST(TestProviderRuntimeBuildToggleReporting)
{
	ProviderProfile gemini_cli = ProviderProfileStore::DefaultGeminiProfile();
	gemini_cli.id = "gemini-cli";
	gemini_cli.output_mode = "cli";
	gemini_cli.supports_interactive = true;
	gemini_cli.interactive_command = "gemini";

	const bool enabled = ProviderRuntime::IsRuntimeEnabled(gemini_cli);
	const std::string disabled_reason = ProviderRuntime::DisabledReason(gemini_cli);

	if (enabled)
	{
		UAM_ASSERT(disabled_reason.empty());
	}
	else
	{
		UAM_ASSERT(!disabled_reason.empty());
		UAM_ASSERT(ProviderRuntime::BuildCommand(gemini_cli, AppSettings{}, "hello", std::vector<std::string>{}, std::string{}).empty());
		UAM_ASSERT(ProviderRuntime::BuildInteractiveArgv(gemini_cli, ChatSession{}, AppSettings{}).empty());
	}

	ProviderProfile custom = ProviderProfileStore::DefaultGeminiProfile();
	custom.id = "custom-runtime";
	custom.history_adapter = "local-only";
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled(custom));
	UAM_ASSERT(ProviderRuntime::DisabledReason(custom).empty());
}

UAM_TEST(TestProviderRuntimeHistoryPolicyLocalOnly)
{
	TempDir data_root("uam-runtime-history-local");
	TempDir native_root("uam-runtime-history-local-native");

	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	profile.id = "custom-runtime";
	profile.history_adapter = "local-only";
	profile.prompt_bootstrap = "prepend";
	profile.supports_interactive = true;

	ChatSession chat;
	chat.id = "chat-local-1";
	chat.provider_id = profile.id;
	chat.title = "Local chat";
	chat.created_at = "2026-04-02 10:00:00";
	chat.updated_at = "2026-04-02 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "hello local", "2026-04-02 10:00:01"});
	UAM_ASSERT(ProviderRuntime::SaveHistory(profile, data_root.root, chat));

	const std::vector<ChatSession> loaded = ProviderRuntime::LoadHistory(profile, data_root.root, native_root.root, ProviderRuntimeHistoryLoadOptions{});
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(std::string("chat-local-1"), loaded.front().id);
	UAM_ASSERT_EQ(std::string("hello local"), loaded.front().messages.front().content);
	UAM_ASSERT(ProviderRuntime::UsesLocalHistory(profile));
	UAM_ASSERT(!ProviderRuntime::UsesNativeOverlayHistory(profile));
}

UAM_TEST(TestProviderRuntimeHistoryPolicyGeminiNativeOverlay)
{
	TempDir data_root("uam-runtime-history-gemini");
	TempDir native_root("uam-runtime-history-gemini-native");
	const fs::path chats_dir = native_root.root / "chats";
	fs::create_directories(chats_dir);

	const std::string native_json = "{\n"
	                                "  \"sessionId\": \"native-session-1\",\n"
	                                "  \"startTime\": \"2026-04-02 09:00:00\",\n"
	                                "  \"lastUpdated\": \"2026-04-02 09:00:01\",\n"
	                                "  \"messages\": [\n"
	                                "    {\"type\": \"user\", \"timestamp\": \"2026-04-02 09:00:00\", \"content\": \"Hello native\"},\n"
	                                "    {\"type\": \"assistant\", \"timestamp\": \"2026-04-02 09:00:01\", \"content\": \"Hi from native\"}\n"
	                                "  ]\n"
	                                "}\n";
	UAM_ASSERT(WriteTextFile(chats_dir / "native-session-1.json", native_json));

	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	profile.id = "gemini-cli";
	profile.history_adapter = "local-only";
	profile.output_mode = "structured";

	const std::vector<ChatSession> loaded_native = ProviderRuntime::LoadHistory(profile, data_root.root, chats_dir, ProviderRuntimeHistoryLoadOptions{});
	UAM_ASSERT_EQ(1u, loaded_native.size());
	UAM_ASSERT_EQ(std::string("native-session-1"), loaded_native.front().id);
	UAM_ASSERT_EQ(std::string("native-session-1"), loaded_native.front().native_session_id);
	UAM_ASSERT_EQ(std::string("Hello native"), loaded_native.front().messages.front().content);
	UAM_ASSERT(ProviderRuntime::SupportsGeminiJsonHistory(profile));
	UAM_ASSERT(!ProviderRuntime::UsesLocalHistory(profile));
	UAM_ASSERT(ProviderRuntime::UsesNativeOverlayHistory(profile));

	ChatSession overlay_chat;
	overlay_chat.id = "chat-overlay-1";
	overlay_chat.provider_id = profile.id;
	overlay_chat.title = "Overlay";
	overlay_chat.created_at = "2026-04-02 09:05:00";
	overlay_chat.updated_at = "2026-04-02 09:05:01";
	overlay_chat.messages.push_back(Message{MessageRole::User, "overlay msg", "2026-04-02 09:05:01"});
	UAM_ASSERT(ProviderRuntime::SaveHistory(profile, data_root.root, overlay_chat));

	const std::vector<ChatSession> local_loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, local_loaded.size());
	UAM_ASSERT_EQ(std::string("chat-overlay-1"), local_loaded.front().id);
}

UAM_TEST(TestProviderRuntimeBuildToggleMatrixContracts)
{
	struct RuntimeExpectation
	{
		const char* runtime_id;
		bool enabled;
	};

	const std::vector<RuntimeExpectation> expectations = {
	    {"gemini", true},
	    {"gemini-structured", true},
	    {"gemini-cli", true},
	};

	for (const RuntimeExpectation& expected : expectations)
	{
		const bool enabled = ProviderRuntime::IsRuntimeEnabled(expected.runtime_id);
		UAM_ASSERT_EQ(expected.enabled, enabled);
		const std::string reason = ProviderRuntime::DisabledReason(expected.runtime_id);

		if (expected.enabled)
		{
			UAM_ASSERT(reason.empty());
		}
		else
		{
			UAM_ASSERT(!reason.empty());
			UAM_ASSERT(reason.find(expected.runtime_id) != std::string::npos);
		}
	}
}

UAM_TEST(TestFrontendActionMapRoundTrip)
{
	uam::FrontendActionMap action_map = uam::DefaultFrontendActionMap();
	uam::FrontendAction* send_action = uam::FindAction(action_map, "send_prompt");
	UAM_ASSERT(send_action != nullptr);
	send_action->label = "Ship Prompt";
	send_action->properties["hotkey"] = "Ctrl+Enter";

	const std::string serialized = uam::SerializeFrontendActionMap(action_map);
	uam::FrontendActionMap parsed;
	std::string error;
	UAM_ASSERT(uam::ParseFrontendActionMap(serialized, parsed, &error));
	const uam::FrontendAction* parsed_send = uam::FindAction(parsed, "send_prompt");
	UAM_ASSERT(parsed_send != nullptr);
	UAM_ASSERT_EQ(std::string("Ship Prompt"), parsed_send->label);
	UAM_ASSERT(parsed_send->properties.find("hotkey") != parsed_send->properties.end());
	UAM_ASSERT_EQ(std::string("Ctrl+Enter"), parsed_send->properties.at("hotkey"));
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
		catch (const TestFailure& failure)
		{
			++failures;
			std::cerr << "[FAIL] " << test.name << " - " << failure.what() << '\n';
		}
		catch (const std::exception& ex)
		{
			++failures;
			std::cerr << "[FAIL] " << test.name << " - unexpected exception: " << ex.what() << '\n';
		}
		catch (...)
		{
			++failures;
			std::cerr << "[FAIL] " << test.name << " - unknown exception\n";
		}
	}

	if (failures != 0)
	{
		std::cerr << failures << " test(s) failed.\n";
		return 1;
	}

	std::cout << Registry().size() << " test(s) passed.\n";
	return 0;
}
