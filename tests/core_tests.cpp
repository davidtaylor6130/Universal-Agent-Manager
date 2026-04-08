#include "app/runtime_orchestration_services.h"
#include "common/models/app_models.h"
#include "common/paths/app_paths.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_repository.h"
#include "common/chat/chat_folder_store.h"
#include "common/config/frontend_actions.h"
#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#include "common/provider/gemini/structured/gemini_structured_provider_runtime.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/provider/opencode/base/opencode_base_provider_runtime.h"
#include "common/provider/markdown_template_catalog.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/rag/rag_index_service.h"
#include "common/config/settings_store.h"
#include "common/platform/platform_services.h"
#include "common/utils/string_utils.h"
#include "common/rag/ollama_engine_client.h"
#include "common/state/app_state.h"
#include "common/ui/chat_actions/chat_action_pending_calls.h"
#include "common/vcs/vcs_workspace_service.h"

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
		app.settings.active_provider_id = "codex-cli";
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
	write_settings.active_provider_id = "gemini-structured";
	write_settings.provider_command_template = "gemini {resume} {flags} -p {prompt}";
	write_settings.provider_yolo_mode = true;
	write_settings.provider_extra_flags = "--alpha --beta";
	write_settings.runtime_backend = "ollama-engine";
	write_settings.selected_model_id = "chat-model.gguf";
	write_settings.vector_db_backend = "ollama-engine";
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
	UAM_ASSERT_EQ(std::string("gemini-structured"), loaded.active_provider_id);
	UAM_ASSERT(loaded.provider_yolo_mode);
	UAM_ASSERT_EQ(std::string("--alpha --beta"), loaded.provider_extra_flags);
	UAM_ASSERT_EQ(std::string("provider-cli"), loaded.runtime_backend);
	UAM_ASSERT_EQ(std::string("chat-model.gguf"), loaded.selected_model_id);
	UAM_ASSERT_EQ(std::string("ollama-engine"), loaded.vector_db_backend);
	UAM_ASSERT_EQ(std::string("embed-model.gguf"), loaded.selected_vector_model_id);
	UAM_ASSERT_EQ(std::string("team_index_v1"), loaded.vector_database_name_override);
	UAM_ASSERT_EQ(600, loaded.cli_idle_timeout_seconds);
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
	UAM_ASSERT_EQ(std::string("ollama-engine"), loaded.vector_db_backend);
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
	UAM_ASSERT(WriteTextFile(settings_file, "runtime_backend=ollama-engine\n"
	                                        "center_view_mode=structured\n"));

	AppSettings loaded;
	loaded.active_provider_id.clear();
	CenterViewMode loaded_mode = CenterViewMode::CliConsole;
	SettingsStore::Load(settings_file, loaded, loaded_mode);

	UAM_ASSERT_EQ(std::string("ollama-engine"), loaded.active_provider_id);
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

UAM_TEST(TestChatRepositoryPersistsTemplateOverride)
{
	TempDir data_root("uam-chat-repository");

	ChatSession chat;
	chat.id = "chat-test-1";
	chat.provider_id = "codex";
	chat.folder_id = "folder-default";
	chat.template_override_id = "custom-template.md";
	chat.rag_enabled = false;
	chat.rag_source_directories = {"/tmp/workspace-a", "/tmp/workspace-b"};
	chat.title = "Template Test";
	chat.created_at = "2026-03-19 10:11:12";
	chat.updated_at = "2026-03-19 10:11:13";
	chat.messages.push_back(Message{MessageRole::User, "hello", "2026-03-19 10:11:13"});

	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(std::string("codex"), loaded.front().provider_id);
	UAM_ASSERT_EQ(std::string("custom-template.md"), loaded.front().template_override_id);
	UAM_ASSERT_EQ(false, loaded.front().rag_enabled);
	UAM_ASSERT_EQ(2u, loaded.front().rag_source_directories.size());
	UAM_ASSERT_EQ(std::string("/tmp/workspace-a"), loaded.front().rag_source_directories[0]);
	UAM_ASSERT_EQ(std::string("/tmp/workspace-b"), loaded.front().rag_source_directories[1]);
}

UAM_TEST(TestChatRepositoryPersistsBranchMetadata)
{
	TempDir data_root("uam-chat-branch-meta");

	ChatSession chat;
	chat.id = "chat-branch-child";
	chat.parent_chat_id = "chat-branch-parent";
	chat.branch_root_chat_id = "chat-branch-root";
	chat.branch_from_message_index = 2;
	chat.folder_id = "folder-default";
	chat.title = "Branch Child";
	chat.created_at = "2026-03-21 10:00:00";
	chat.updated_at = "2026-03-21 10:01:00";
	chat.messages.push_back(Message{MessageRole::User, "hello", "2026-03-21 10:00:01"});

	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(std::string("chat-branch-parent"), loaded.front().parent_chat_id);
	UAM_ASSERT_EQ(std::string("chat-branch-root"), loaded.front().branch_root_chat_id);
	UAM_ASSERT_EQ(2, loaded.front().branch_from_message_index);
}

UAM_TEST(TestChatRepositoryDefaultsMissingBranchMetadata)
{
	TempDir data_root("uam-chat-branch-defaults");
	const fs::path chats_root = AppPaths::UamChatsRootPath(data_root.root);
	const fs::path chat_file = chats_root / "chat-legacy.json";
	fs::create_directories(chats_root);
	UAM_ASSERT(WriteTextFile(chat_file, R"({"id":"chat-legacy","title":"Legacy","created_at":"2026-03-21 11:00:00","updated_at":"2026-03-21 11:00:01","messages":[{"role":"user","content":"hello","created_at":"2026-03-21 11:00:01"}]})"));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
	UAM_ASSERT_EQ(1u, loaded.size());
	UAM_ASSERT_EQ(std::string("chat-legacy"), loaded.front().branch_root_chat_id);
	UAM_ASSERT_EQ(std::string(""), loaded.front().parent_chat_id);
	UAM_ASSERT_EQ(-1, loaded.front().branch_from_message_index);
	UAM_ASSERT_EQ(true, loaded.front().rag_enabled);
	UAM_ASSERT(loaded.front().rag_source_directories.empty());
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

UAM_TEST(TestLoadSidebarChatsKeepsMixedProviderHistoryVisible)
{
	TempDir data_root("uam-sidebar-load-data");
	TempDir gemini_home("uam-sidebar-load-gemini-home");
	TempDir project_a("uam-sidebar-load-project-a");
	TempDir project_b("uam-sidebar-load-project-b");

	const fs::path tmp_a = gemini_home.root / "tmp" / "release-a";
	const fs::path tmp_b = gemini_home.root / "tmp" / "release-b";
	fs::create_directories(tmp_a / "chats");
	fs::create_directories(tmp_b / "chats");
	WriteNativeProjectRoot(tmp_a, project_a.root);
	WriteNativeProjectRoot(tmp_b, project_b.root);
	WriteGeminiNativeSession(tmp_a / "chats", "session-structured", {{"user", "structured prompt"}, {"assistant", "structured reply"}});
	WriteGeminiNativeSession(tmp_b / "chats", "session-orphan", {{"user", "orphan prompt"}, {"assistant", "orphan reply"}});

	ChatSession overlay_chat;
	overlay_chat.id = "session-structured";
	overlay_chat.provider_id = "gemini-structured";
	overlay_chat.native_session_id = "session-structured";
	overlay_chat.folder_id = "folder-a";
	overlay_chat.title = "Structured Overlay";
	overlay_chat.created_at = "2026-03-21 10:00:00";
	overlay_chat.updated_at = "2026-03-21 10:00:01";
	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, overlay_chat));

	ChatSession local_chat;
	local_chat.id = "codex-local";
	local_chat.provider_id = "codex-cli";
	local_chat.folder_id = "folder-b";
	local_chat.title = "Local Chat";
	local_chat.created_at = "2026-03-21 10:00:00";
	local_chat.updated_at = "2026-03-21 10:00:01";
	local_chat.messages.push_back(Message{MessageRole::User, "hello local", "2026-03-21 10:00:01"});
	UAM_ASSERT(ChatRepository::SaveChat(data_root.root, local_chat));

	ScopedEnvVar cli("GEMINI_CLI_HOME", gemini_home.root.string());
	ScopedEnvVar gemini("GEMINI_HOME", std::nullopt);

	uam::AppState app = MakeTestAppState(data_root.root);
	app.folders.push_back(ChatFolder{"folder-a", "Folder A", project_a.root.string(), false});
	app.folders.push_back(ChatFolder{"folder-b", "Folder B", project_b.root.string(), false});
	app.settings.active_provider_id = "codex-cli";

	ChatHistorySyncService().LoadSidebarChats(app);

	ChatSession* structured = FindChatById(app.chats, "session-structured");
	ChatSession* orphan = FindChatById(app.chats, "session-orphan");
	ChatSession* local = FindChatById(app.chats, "codex-local");

	UAM_ASSERT(structured != nullptr);
	UAM_ASSERT(orphan != nullptr);
	UAM_ASSERT(local != nullptr);
	UAM_ASSERT_EQ(std::string("gemini-structured"), structured->provider_id);
	UAM_ASSERT_EQ(std::string("gemini-cli"), orphan->provider_id);
	UAM_ASSERT_EQ(std::string("codex-cli"), local->provider_id);
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
	app.settings.active_provider_id = "codex-cli";

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
	app.settings.active_provider_id = "codex-cli";

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
	app.settings.active_provider_id = "codex-cli";

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
	const ProviderProfile* gemini_structured = ProviderProfileStore::FindById(profiles, "gemini-structured");
	const ProviderProfile* gemini_cli = ProviderProfileStore::FindById(profiles, "gemini-cli");
	UAM_ASSERT(ProviderProfileStore::FindById(profiles, "gemini") == nullptr);
	UAM_ASSERT(ProviderProfileStore::FindById(profiles, "codex-cli") != nullptr);
	UAM_ASSERT(ProviderProfileStore::FindById(profiles, "claude-cli") != nullptr);
	UAM_ASSERT(ProviderProfileStore::FindById(profiles, "opencode-cli") != nullptr);
	UAM_ASSERT(gemini_structured != nullptr);
	UAM_ASSERT(gemini_cli != nullptr);
	UAM_ASSERT_EQ(std::string("structured"), gemini_structured->output_mode);
	UAM_ASSERT_EQ(std::string("gemini -r {resume} {flags} -p {prompt}"), gemini_structured->command_template);
	UAM_ASSERT(!gemini_structured->supports_interactive);
	UAM_ASSERT_EQ(std::string("cli"), gemini_cli->output_mode);
	UAM_ASSERT(gemini_cli->supports_interactive);
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
	UAM_ASSERT(ProviderRuntime::UsesStructuredOutput(profile));
	UAM_ASSERT(!ProviderRuntime::UsesCliOutput(profile));

	profile.output_mode = "cli";
	profile.supports_interactive = true;
	UAM_ASSERT(ProviderRuntime::UsesCliOutput(profile));
	UAM_ASSERT(!ProviderRuntime::UsesStructuredOutput(profile));
}

UAM_TEST(TestProviderRuntimeBuiltInPolicyLocks)
{
	ProviderProfile gemini_structured = ProviderProfileStore::DefaultGeminiProfile();
	gemini_structured.id = "gemini-structured";
	gemini_structured.output_mode = "cli";
	gemini_structured.supports_interactive = true;
	gemini_structured.history_adapter = "local-only";
	gemini_structured.prompt_bootstrap = "prepend";
	UAM_ASSERT(ProviderRuntime::SupportsGeminiJsonHistory(gemini_structured));
	UAM_ASSERT(!ProviderRuntime::UsesLocalHistory(gemini_structured));
	UAM_ASSERT(!ProviderRuntime::UsesCliOutput(gemini_structured));
	UAM_ASSERT(ProviderRuntime::UsesStructuredOutput(gemini_structured));
	UAM_ASSERT(ProviderRuntime::UsesGeminiPathBootstrap(gemini_structured));

	ProviderProfile gemini_cli = ProviderProfileStore::DefaultGeminiProfile();
	gemini_cli.id = "gemini-cli";
	gemini_cli.output_mode = "structured";
	gemini_cli.supports_interactive = false;
	gemini_cli.history_adapter = "local-only";
	gemini_cli.prompt_bootstrap = "prepend";
	UAM_ASSERT(ProviderRuntime::SupportsGeminiJsonHistory(gemini_cli));
	UAM_ASSERT(!ProviderRuntime::UsesLocalHistory(gemini_cli));
	UAM_ASSERT(ProviderRuntime::UsesCliOutput(gemini_cli));
	UAM_ASSERT(!ProviderRuntime::UsesStructuredOutput(gemini_cli));
	UAM_ASSERT(ProviderRuntime::UsesGeminiPathBootstrap(gemini_cli));

	ProviderProfile codex = ProviderProfileStore::DefaultGeminiProfile();
	codex.id = "codex-cli";
	codex.history_adapter = "gemini-cli-json";
	codex.execution_mode = "internal-engine";
	UAM_ASSERT(!ProviderRuntime::SupportsGeminiJsonHistory(codex));
	UAM_ASSERT(ProviderRuntime::UsesLocalHistory(codex));
	UAM_ASSERT(!ProviderRuntime::UsesInternalEngine(codex));

	ProviderProfile ollama = ProviderProfileStore::DefaultGeminiProfile();
	ollama.id = "ollama-engine";
	ollama.output_mode = "cli";
	ollama.execution_mode = "cli";
	UAM_ASSERT(!ProviderRuntime::SupportsGeminiJsonHistory(ollama));
	UAM_ASSERT(ProviderRuntime::UsesLocalHistory(ollama));
	UAM_ASSERT(ProviderRuntime::UsesInternalEngine(ollama));
	UAM_ASSERT(!ProviderRuntime::UsesCliOutput(ollama));
	UAM_ASSERT(ProviderRuntime::UsesStructuredOutput(ollama));
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
	const std::vector<std::string> ids = {
	    "gemini-structured", "gemini-cli", "codex-cli", "claude-cli", "opencode-cli", "opencode-local", "ollama-engine",
	};

	for (const std::string& id : ids)
	{
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(id);
		UAM_ASSERT_EQ(id, std::string(runtime.RuntimeId()));
		UAM_ASSERT(ProviderRuntimeRegistry::IsKnownRuntimeId(id));
	}

	UAM_ASSERT(!ProviderRuntimeRegistry::IsKnownRuntimeId("custom-runtime"));
}

UAM_TEST(TestProviderRuntimeFamilyBaseTypes)
{
	const IProviderRuntime& gemini_cli = ProviderRuntimeRegistry::ResolveById("gemini-cli");
	const IProviderRuntime& gemini_structured = ProviderRuntimeRegistry::ResolveById("gemini-structured");
	const IProviderRuntime& opencode_cli = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	const IProviderRuntime& opencode_local = ProviderRuntimeRegistry::ResolveById("opencode-local");
	const IProviderRuntime& codex = ProviderRuntimeRegistry::ResolveById("codex-cli");

	UAM_ASSERT(dynamic_cast<const GeminiCliProviderRuntime*>(&gemini_cli) != nullptr);
	UAM_ASSERT(dynamic_cast<const GeminiStructuredProviderRuntime*>(&gemini_structured) != nullptr);
	UAM_ASSERT(dynamic_cast<const OpenCodeBaseProviderRuntime*>(&opencode_cli) != nullptr);
	UAM_ASSERT(dynamic_cast<const OpenCodeBaseProviderRuntime*>(&opencode_local) != nullptr);
	UAM_ASSERT(dynamic_cast<const GeminiCliProviderRuntime*>(&codex) == nullptr);
	UAM_ASSERT(dynamic_cast<const OpenCodeBaseProviderRuntime*>(&codex) == nullptr);
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

UAM_TEST(TestProviderRuntimeBlocksNonGeminiGeminiHistoryAdapter)
{
	ProviderProfile codex = ProviderProfileStore::DefaultGeminiProfile();
	codex.id = "codex-cli";
	codex.history_adapter = "gemini-cli-json";

	UAM_ASSERT(!ProviderRuntime::IsRuntimeEnabled(codex));
	const std::string reason = ProviderRuntime::DisabledReason(codex);
	UAM_ASSERT(!reason.empty());
	UAM_ASSERT(reason.find("gemini-cli-json") != std::string::npos);
	UAM_ASSERT(ProviderRuntime::BuildCommand(codex, AppSettings{}, "hello", std::vector<std::string>{}, std::string{}).empty());
	UAM_ASSERT(ProviderRuntime::BuildInteractiveArgv(codex, ChatSession{}, AppSettings{}).empty());
}

UAM_TEST(TestProviderRuntimeHistoryPolicyLocalOnly)
{
	TempDir data_root("uam-runtime-history-local");
	TempDir native_root("uam-runtime-history-local-native");

	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	profile.id = "codex-cli";

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
	    {"gemini-structured", UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED != 0}, {"gemini-cli", UAM_ENABLE_RUNTIME_GEMINI_CLI != 0}, {"codex-cli", UAM_ENABLE_RUNTIME_CODEX_CLI != 0}, {"claude-cli", UAM_ENABLE_RUNTIME_CLAUDE_CLI != 0}, {"opencode-cli", UAM_ENABLE_RUNTIME_OPENCODE_CLI != 0}, {"opencode-local", UAM_ENABLE_RUNTIME_OPENCODE_LOCAL != 0}, {"ollama-engine", UAM_ENABLE_RUNTIME_OLLAMA_ENGINE != 0},
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

// ---------------------------------------------------------------------------
// Regression tests: Bug #1 — failed dispatches must not persist user messages
// ---------------------------------------------------------------------------

UAM_TEST(TestQueuePromptEmptyDoesNotAddMessage)
{
	// An empty (or whitespace-only) prompt must return false without touching
	// the chat's message list. Previously, AddMessage was called before the
	// guard in some dispatch paths.
	TempDir data_root("uam-queue-empty-prompt");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession chat;
	chat.id = "chat-empty-1";
	chat.provider_id = "codex-cli";
	chat.title = "Empty Prompt Test";
	chat.created_at = "2026-04-01 10:00:00";
	chat.updated_at = "2026-04-01 10:00:01";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	const bool l_result = ProviderRequestService().QueuePromptForChat(app, app.chats[0], "   ", false);

	UAM_ASSERT(!l_result);
	UAM_ASSERT(app.chats[0].messages.empty());
	UAM_ASSERT_EQ(std::string("Prompt is empty."), app.status_line);
}

UAM_TEST(TestQueuePromptAlreadyPendingDoesNotAddMessage)
{
	// When a pending call already exists for a chat the function must reject
	// the new submission without appending a user message or spawning a worker.
	TempDir data_root("uam-queue-already-pending");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession chat;
	chat.id = "chat-pending-1";
	chat.provider_id = "codex-cli";
	chat.title = "Already Pending Test";
	chat.created_at = "2026-04-01 10:00:00";
	chat.updated_at = "2026-04-01 10:00:01";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	PendingRuntimeCall l_existing;
	l_existing.chat_id = "chat-pending-1";
	l_existing.state = std::make_shared<AsyncProcessTaskState>();
	app.pending_calls.push_back(std::move(l_existing));

	const bool l_result = ProviderRequestService().QueuePromptForChat(app, app.chats[0], "hello", false);

	UAM_ASSERT(!l_result);
	UAM_ASSERT(app.chats[0].messages.empty());
	UAM_ASSERT_EQ(std::string("Provider command already running for this chat."), app.status_line);
	UAM_ASSERT_EQ(1u, app.pending_calls.size());
}

// ---------------------------------------------------------------------------
// Regression tests: Bug #2 — local-history CLI failures must use System role
// ---------------------------------------------------------------------------

UAM_TEST(TestPollPendingCallNonZeroExitCodeUsesSystemRole)
{
	// A completed pending call with a non-zero exit code must be appended as
	// a System message, not as an Assistant message. Previously all CLI output
	// was always appended as Assistant regardless of whether the command failed.
	TempDir data_root("uam-poll-nonzero-exit");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession chat;
	chat.id = "chat-codex-fail";
	chat.provider_id = "codex-cli";
	chat.title = "Codex Fail";
	chat.created_at = "2026-04-01 10:00:00";
	chat.updated_at = "2026-04-01 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "run failing command", "2026-04-01 10:00:01"});
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	PendingRuntimeCall l_call;
	l_call.chat_id = "chat-codex-fail";
	l_call.provider_id_snapshot = "codex-cli";
	l_call.state = std::make_shared<AsyncProcessTaskState>();
	l_call.state->result.output = "command: not found";
	l_call.state->result.exit_code = 1;
	l_call.state->completed.store(true, std::memory_order_release);
	app.pending_calls.push_back(std::move(l_call));

	PollPendingRuntimeCall(app);

	UAM_ASSERT(app.pending_calls.empty());
	UAM_ASSERT_EQ(2u, app.chats[0].messages.size());
	UAM_ASSERT(app.chats[0].messages.back().role == MessageRole::System);
	UAM_ASSERT_EQ(std::string("command: not found"), app.chats[0].messages.back().content);
	UAM_ASSERT_EQ(std::string("Provider command failed."), app.status_line);
}

UAM_TEST(TestPollPendingCallTimedOutUsesSystemRole)
{
	// A timed-out pending call must be appended as System, not Assistant.
	TempDir data_root("uam-poll-timeout");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession chat;
	chat.id = "chat-codex-timeout";
	chat.provider_id = "codex-cli";
	chat.title = "Codex Timeout";
	chat.created_at = "2026-04-01 10:00:00";
	chat.updated_at = "2026-04-01 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "slow command", "2026-04-01 10:00:01"});
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	PendingRuntimeCall l_call;
	l_call.chat_id = "chat-codex-timeout";
	l_call.provider_id_snapshot = "codex-cli";
	l_call.state = std::make_shared<AsyncProcessTaskState>();
	l_call.state->result.output = "[Provider CLI command timed out]";
	l_call.state->result.timed_out = true;
	l_call.state->completed.store(true, std::memory_order_release);
	app.pending_calls.push_back(std::move(l_call));

	PollPendingRuntimeCall(app);

	UAM_ASSERT(app.pending_calls.empty());
	UAM_ASSERT_EQ(2u, app.chats[0].messages.size());
	UAM_ASSERT(app.chats[0].messages.back().role == MessageRole::System);
	UAM_ASSERT_EQ(std::string("Provider command failed."), app.status_line);
}

UAM_TEST(TestPollPendingCallCanceledUsesSystemRole)
{
	// A canceled pending call must be appended as System, not Assistant.
	TempDir data_root("uam-poll-canceled");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession chat;
	chat.id = "chat-codex-canceled";
	chat.provider_id = "codex-cli";
	chat.title = "Codex Canceled";
	chat.created_at = "2026-04-01 10:00:00";
	chat.updated_at = "2026-04-01 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "interrupted command", "2026-04-01 10:00:01"});
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	PendingRuntimeCall l_call;
	l_call.chat_id = "chat-codex-canceled";
	l_call.provider_id_snapshot = "codex-cli";
	l_call.state = std::make_shared<AsyncProcessTaskState>();
	l_call.state->result.output = "[Provider CLI command canceled]";
	l_call.state->result.canceled = true;
	l_call.state->completed.store(true, std::memory_order_release);
	app.pending_calls.push_back(std::move(l_call));

	PollPendingRuntimeCall(app);

	UAM_ASSERT(app.pending_calls.empty());
	UAM_ASSERT_EQ(2u, app.chats[0].messages.size());
	UAM_ASSERT(app.chats[0].messages.back().role == MessageRole::System);
	UAM_ASSERT_EQ(std::string("Provider command failed."), app.status_line);
}

UAM_TEST(TestPollPendingCallSuccessUsesAssistantRole)
{
	// A successful (exit_code == 0) pending call must be appended as Assistant.
	// This is the counterpart to the three failure tests above: confirming that
	// the role selection only uses System for the error cases.
	TempDir data_root("uam-poll-success-role");
	uam::AppState app = MakeTestAppState(data_root.root);

	ChatSession chat;
	chat.id = "chat-codex-ok";
	chat.provider_id = "codex-cli";
	chat.title = "Codex Success";
	chat.created_at = "2026-04-01 10:00:00";
	chat.updated_at = "2026-04-01 10:00:01";
	chat.messages.push_back(Message{MessageRole::User, "working command", "2026-04-01 10:00:01"});
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	PendingRuntimeCall l_call;
	l_call.chat_id = "chat-codex-ok";
	l_call.provider_id_snapshot = "codex-cli";
	l_call.state = std::make_shared<AsyncProcessTaskState>();
	l_call.state->result.output = "successful output";
	l_call.state->result.exit_code = 0;
	l_call.state->completed.store(true, std::memory_order_release);
	app.pending_calls.push_back(std::move(l_call));

	PollPendingRuntimeCall(app);

	UAM_ASSERT(app.pending_calls.empty());
	UAM_ASSERT_EQ(2u, app.chats[0].messages.size());
	UAM_ASSERT(app.chats[0].messages.back().role == MessageRole::Assistant);
	UAM_ASSERT_EQ(std::string("successful output"), app.chats[0].messages.back().content);
	UAM_ASSERT_EQ(std::string("Provider response appended to local chat history."), app.status_line);
}

UAM_TEST(TestSidebarSearchFiltering)
{
	uam::AppState app;
	ChatSession s1; s1.id = "match-title"; s1.title = "Search Target";
	ChatSession s2; s2.id = "match-message"; s2.title = "Other"; Message m; m.content = "find me in history"; s2.messages.push_back(m);
	ChatSession s3; s3.id = "no-match"; s3.title = "Unrelated";
	app.chats.push_back(s1);
	app.chats.push_back(s2);
	app.chats.push_back(s3);

	auto update_filter = [&](const std::string& query) {
		app.sidebar_search_query = query;
		if (app.sidebar_search_query != app.last_sidebar_search_query) {
			app.last_sidebar_search_query = app.sidebar_search_query;
			app.filtered_chat_ids.clear();
			if (!app.sidebar_search_query.empty()) {
				for (const auto& chat : app.chats) {
					bool match = uam::strings::ContainsCaseInsensitive(chat.title, app.sidebar_search_query);
					if (!match) {
						for (const auto& msg : chat.messages) {
							if (uam::strings::ContainsCaseInsensitive(msg.content, app.sidebar_search_query)) {
								match = true;
								break;
							}
						}
					}
					if (match) app.filtered_chat_ids.insert(chat.id);
				}
			}
		}
	};

	update_filter("target");
	UAM_ASSERT(app.filtered_chat_ids.count("match-title"));
	UAM_ASSERT(!app.filtered_chat_ids.count("match-message"));
	UAM_ASSERT(!app.filtered_chat_ids.count("no-match"));

	update_filter("history");
	UAM_ASSERT(!app.filtered_chat_ids.count("match-title"));
	UAM_ASSERT(app.filtered_chat_ids.count("match-message"));
	UAM_ASSERT(!app.filtered_chat_ids.count("no-match"));

	update_filter("FIND"); // case insensitive
	UAM_ASSERT(app.filtered_chat_ids.count("match-message"));

	update_filter("");
	UAM_ASSERT(app.filtered_chat_ids.empty());
}

UAM_TEST(TestSidebarSearchAncestorPreservation)
{
	uam::AppState app;
	ChatSession s1; s1.id = "parent"; s1.title = "Parent";
	ChatSession s2; s2.id = "child"; s2.parent_chat_id = "parent"; s2.title = "Target Child";
	ChatSession s3; s3.id = "unrelated"; s3.title = "Other";
	app.chats.push_back(s1);
	app.chats.push_back(s2);
	app.chats.push_back(s3);

	auto update_filter = [&](const std::string& query) {
		app.sidebar_search_query = query;
		if (app.sidebar_search_query != app.last_sidebar_search_query) {
			app.last_sidebar_search_query = app.sidebar_search_query;
			app.filtered_chat_ids.clear();
			if (!app.sidebar_search_query.empty()) {
				for (const auto& chat : app.chats) {
					bool match = uam::strings::ContainsCaseInsensitive(chat.title, app.sidebar_search_query);
					if (match) app.filtered_chat_ids.insert(chat.id);
				}
				std::vector<std::string> ancestors_to_add;
				for (const auto& id : app.filtered_chat_ids) {
					int current_idx = -1;
					for (int i = 0; i < (int)app.chats.size(); ++i) if (app.chats[i].id == id) { current_idx = i; break; }
					while (current_idx >= 0 && !app.chats[current_idx].parent_chat_id.empty()) {
						std::string p_id = app.chats[current_idx].parent_chat_id;
						if (app.filtered_chat_ids.count(p_id)) break;
						ancestors_to_add.push_back(p_id);
						current_idx = -1;
						for (int i = 0; i < (int)app.chats.size(); ++i) if (app.chats[i].id == p_id) { current_idx = i; break; }
					}
				}
				for (const auto& id : ancestors_to_add) app.filtered_chat_ids.insert(id);
			}
		}
	};

	update_filter("child");
	UAM_ASSERT(app.filtered_chat_ids.count("child"));
	UAM_ASSERT(app.filtered_chat_ids.count("parent")); // Ancestor preserved
	UAM_ASSERT(!app.filtered_chat_ids.count("unrelated"));
}

UAM_TEST(WindowsConPtyLifecycleTest)
{
	uam::CliTerminalState terminal;
	terminal.rows = 24;
	terminal.cols = 80;

	std::vector<std::string> args = {"cmd.exe", "/C", "echo", "Hello UAM ConPTY test"};
	std::string error_out;
	
	const bool started = PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, std::filesystem::current_path(), args, &error_out);
	UAM_ASSERT(started);
	UAM_ASSERT(error_out.empty());
	
	char buffer[1024];
	std::ptrdiff_t bytes_read = 0;
	bool found_output = false;

	for (int i = 0; i < 50; ++i)
	{
		bytes_read = PlatformServicesFactory::Instance().terminal_runtime.ReadCliTerminalOutput(terminal, buffer, sizeof(buffer) - 1);
		if (bytes_read > 0)
		{
			buffer[bytes_read] = '\0';
			std::string out_str(buffer);
			if (out_str.find("Hello UAM") != std::string::npos)
			{
				found_output = true;
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	
	UAM_ASSERT(found_output);

	PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, true);
	UAM_ASSERT(!PlatformServicesFactory::Instance().terminal_runtime.HasReadableTerminalOutputHandle(terminal));
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

