#include "core/app_models.h"
#include "core/app_paths.h"
#include "core/chat_repository.h"
#include "core/chat_import_utils.h"
#include "core/frontend_actions.h"
#include "core/gemini_cli_compat.h"
#include "core/gemini_command_builder.h"
#include "core/gemini_template_catalog.h"
#include "core/provider_profile.h"
#include "core/provider_runtime.h"
#include "core/settings_store.h"

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
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct TestRegistrar {
  TestRegistrar(const char* name, void (*fn)()) {
    Registry().push_back(TestCase{name, fn});
  }
};

#define UAM_TEST(name) \
  static void name(); \
  static TestRegistrar registrar_##name{#name, &name}; \
  static void name()

[[noreturn]] void Fail(const std::string& message, const char* file, int line) {
  std::ostringstream out;
  out << file << ":" << line << ": " << message;
  throw TestFailure(out.str());
}

#define UAM_ASSERT(cond) \
  do { \
    if (!(cond)) { \
      Fail(std::string("assertion failed: ") + #cond, __FILE__, __LINE__); \
    } \
  } while (0)

#define UAM_ASSERT_EQ(expected, actual) \
  do { \
    const auto& uam_expected = (expected); \
    const auto& uam_actual = (actual); \
    if (!(uam_expected == uam_actual)) { \
      std::ostringstream uam_out; \
      uam_out << "expected [" << uam_expected << "] but got [" << uam_actual << "]"; \
      Fail(uam_out.str(), __FILE__, __LINE__); \
    } \
  } while (0)

bool WriteTextFile(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    return false;
  }
  out << content;
  return out.good();
}

std::string JsonString(const std::string& value) {
  std::string out;
  out.push_back('"');
  for (const char ch : value) {
    switch (ch) {
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

fs::path UniqueTempDir(const std::string& prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::mt19937_64 rng(static_cast<std::mt19937_64::result_type>(now));
  std::uniform_int_distribution<unsigned long long> dist;
  const fs::path root = fs::temp_directory_path() /
                        (prefix + "-" + std::to_string(now) + "-" + std::to_string(dist(rng)));
  fs::create_directories(root);
  return root;
}

struct TempDir {
  fs::path root;

  explicit TempDir(const std::string& prefix) : root(UniqueTempDir(prefix)) {}

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

class ScopedEnvVar {
 public:
  ScopedEnvVar(std::string name, std::optional<std::string> value)
      : name_(std::move(name)), original_(ReadEnv(name_)) {
    Set(value);
  }

  ~ScopedEnvVar() {
    Set(original_);
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  static std::optional<std::string> ReadEnv(const std::string& name) {
    if (const char* value = std::getenv(name.c_str())) {
      return std::string(value);
    }
    return std::nullopt;
  }

  static void SetEnvValue(const std::string& name, const std::optional<std::string>& value) {
#if defined(_WIN32)
    if (value.has_value()) {
      _putenv_s(name.c_str(), value->c_str());
    } else {
      _putenv_s(name.c_str(), "");
    }
#else
    if (value.has_value()) {
      setenv(name.c_str(), value->c_str(), 1);
    } else {
      unsetenv(name.c_str());
    }
#endif
  }

  void Set(const std::optional<std::string>& value) {
    SetEnvValue(name_, value);
  }

  std::string name_;
  std::optional<std::string> original_;
};

void WriteProjectsJson(const fs::path& gemini_home, const fs::path& project_root, const std::string& tmp_name) {
  const fs::path file = gemini_home / "projects.json";
  std::ostringstream out;
  out << "{\n";
  out << "  \"projects\": {\n";
  out << "    " << JsonString(project_root.generic_string()) << ": " << JsonString(tmp_name) << "\n";
  out << "  }\n";
  out << "}\n";
  UAM_ASSERT(WriteTextFile(file, out.str()));
}

void WriteNativeProjectRoot(const fs::path& tmp_chat_dir, const fs::path& project_root) {
  UAM_ASSERT(WriteTextFile(tmp_chat_dir / ".project_root", project_root.generic_string() + "\n"));
}

}  // namespace

UAM_TEST(TestGeminiHomeResolutionPrecedence) {
  TempDir cli_home("uam-cli-home");
  TempDir gemini_home("uam-gemini-home");
  TempDir fallback_home("uam-fallback-home");

  {
    ScopedEnvVar cli("GEMINI_CLI_HOME", cli_home.root.string());
    ScopedEnvVar gemini("GEMINI_HOME", gemini_home.root.string());
    ScopedEnvVar user_profile("USERPROFILE", fallback_home.root.string());
    ScopedEnvVar home("HOME", fallback_home.root.string());
    UAM_ASSERT_EQ(cli_home.root.lexically_normal().generic_string(),
                  AppPaths::GeminiHomePath().lexically_normal().generic_string());
  }

  {
    ScopedEnvVar cli("GEMINI_CLI_HOME", std::nullopt);
    ScopedEnvVar gemini("GEMINI_HOME", gemini_home.root.string());
    ScopedEnvVar user_profile("USERPROFILE", fallback_home.root.string());
    ScopedEnvVar home("HOME", fallback_home.root.string());
    UAM_ASSERT_EQ(gemini_home.root.lexically_normal().generic_string(),
                  AppPaths::GeminiHomePath().lexically_normal().generic_string());
  }
}

UAM_TEST(TestDefaultDataRootPathResolution) {
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
    UAM_ASSERT_EQ(expected.lexically_normal().generic_string(),
                  AppPaths::DefaultDataRootPath().lexically_normal().generic_string());
  }
#else
  {
    ScopedEnvVar home("HOME", home_root.root.string());
#if defined(__APPLE__)
    const fs::path expected = home_root.root / "Library" / "Application Support" / "Universal Agent Manager";
#else
    const fs::path expected = home_root.root / ".universal_agent_manager";
#endif
    UAM_ASSERT_EQ(expected.lexically_normal().generic_string(),
                  AppPaths::DefaultDataRootPath().lexically_normal().generic_string());
  }
#endif
}

UAM_TEST(TestResolveGeminiProjectTmpDirViaProjectRootFile) {
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

UAM_TEST(TestResolveGeminiProjectTmpDirViaProjectsJsonMapping) {
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

UAM_TEST(TestSettingsStoreMigratesLegacyCommandTemplate) {
  TempDir data_root("uam-settings");
  const fs::path settings_file = data_root.root / "settings.txt";
  UAM_ASSERT(WriteTextFile(settings_file,
                           "gemini_command_template=gemini -p {prompt}\n"
                           "gemini_yolo_mode=1\n"
                           "gemini_extra_flags=--alpha --beta\n"
                           "center_view_mode=cli\n"));

  AppSettings settings;
  settings.gemini_command_template = "unused";
  settings.gemini_extra_flags = "";
  CenterViewMode view_mode = CenterViewMode::Structured;

  SettingsStore::Load(settings_file, settings, view_mode);

  UAM_ASSERT_EQ(std::string("gemini {resume} {flags} {prompt}"), settings.gemini_command_template);
  UAM_ASSERT(settings.gemini_yolo_mode);
  UAM_ASSERT_EQ(std::string("--alpha --beta"), settings.gemini_extra_flags);
  UAM_ASSERT(view_mode == CenterViewMode::CliConsole);
}

UAM_TEST(TestSettingsStoreRoundTripExtendedPreferences) {
  TempDir data_root("uam-settings-roundtrip");
  const fs::path settings_file = data_root.root / "settings.txt";

  AppSettings write_settings;
  write_settings.active_provider_id = "gemini";
  write_settings.gemini_command_template = "gemini {resume} {flags} {prompt}";
  write_settings.gemini_yolo_mode = true;
  write_settings.gemini_extra_flags = "--alpha --beta";
  write_settings.gemini_global_root_path = "/tmp/.Gemini_universal_agent_manager";
  write_settings.default_gemini_template_id = "baseline.md";
  write_settings.ui_theme = "system";
  write_settings.confirm_delete_chat = false;
  write_settings.confirm_delete_folder = false;
  write_settings.remember_last_chat = true;
  write_settings.delete_empty_native_gemini_chats_on_import = false;
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
  UAM_ASSERT(loaded.gemini_yolo_mode);
  UAM_ASSERT_EQ(std::string("--alpha --beta"), loaded.gemini_extra_flags);
  UAM_ASSERT_EQ(std::string("/tmp/.Gemini_universal_agent_manager"), loaded.gemini_global_root_path);
  UAM_ASSERT_EQ(std::string("baseline.md"), loaded.default_gemini_template_id);
  UAM_ASSERT(!loaded.confirm_delete_chat);
  UAM_ASSERT(!loaded.confirm_delete_folder);
  UAM_ASSERT(loaded.remember_last_chat);
  UAM_ASSERT(!loaded.delete_empty_native_gemini_chats_on_import);
  UAM_ASSERT_EQ(std::string("chat-123"), loaded.last_selected_chat_id);
  UAM_ASSERT(std::fabs(loaded.ui_scale_multiplier - 1.35f) < 0.0001f);
  UAM_ASSERT_EQ(1680, loaded.window_width);
  UAM_ASSERT_EQ(960, loaded.window_height);
  UAM_ASSERT(loaded.window_maximized);
}

UAM_TEST(TestSettingsStoreClampsInvalidValues) {
  TempDir data_root("uam-settings-clamp");
  const fs::path settings_file = data_root.root / "settings.txt";
  UAM_ASSERT(WriteTextFile(settings_file,
                           "ui_theme=invalid-theme\n"
                           "ui_scale_multiplier=9.0\n"
                           "window_width=64\n"
                           "window_height=99\n"
                           "remember_last_chat=0\n"
                           "last_selected_chat_id=stale-chat\n"));

  AppSettings loaded;
  CenterViewMode loaded_mode = CenterViewMode::Structured;
  SettingsStore::Load(settings_file, loaded, loaded_mode);

  UAM_ASSERT_EQ(std::string("dark"), loaded.ui_theme);
  UAM_ASSERT(std::fabs(loaded.ui_scale_multiplier - 1.75f) < 0.0001f);
  UAM_ASSERT_EQ(960, loaded.window_width);
  UAM_ASSERT_EQ(620, loaded.window_height);
  UAM_ASSERT(!loaded.remember_last_chat);
  UAM_ASSERT_EQ(std::string(""), loaded.last_selected_chat_id);
}

UAM_TEST(TestSettingsStoreLoadsLowScaleClamp) {
  TempDir data_root("uam-settings-scale-low");
  const fs::path settings_file = data_root.root / "settings.txt";
  UAM_ASSERT(WriteTextFile(settings_file,
                           "ui_theme=light\n"
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

UAM_TEST(TestGeminiCommandBuilderReplacesPlaceholders) {
  AppSettings settings;
  settings.gemini_command_template = "gemini --mode test {resume} {flags} --prompt {prompt} --files {files}";
  settings.gemini_yolo_mode = true;
  settings.gemini_extra_flags = "--profile nightly --dry-run";

  const std::string command = GeminiCommandBuilder::BuildCommand(
      settings,
      "Review this patch",
      {"notes one.txt", "more/files.md"},
      "resume-42");

  UAM_ASSERT(command.find("{resume}") == std::string::npos);
  UAM_ASSERT(command.find("{flags}") == std::string::npos);
  UAM_ASSERT(command.find("{prompt}") == std::string::npos);
  UAM_ASSERT(command.find("{files}") == std::string::npos);
  UAM_ASSERT(command.find("resume-42") != std::string::npos);
  UAM_ASSERT(command.find("Review this patch") != std::string::npos);
  UAM_ASSERT(command.find("notes one.txt") != std::string::npos);
  UAM_ASSERT(command.find("more/files.md") != std::string::npos);
  UAM_ASSERT(command.find("--yolo") != std::string::npos);
  UAM_ASSERT(command.find("--profile") != std::string::npos);
  UAM_ASSERT(command.find("nightly") != std::string::npos);
  UAM_ASSERT(command.find("--dry-run") != std::string::npos);
}

UAM_TEST(TestChatRepositoryPersistsTemplateOverride) {
  TempDir data_root("uam-chat-repository");

  ChatSession chat;
  chat.id = "chat-test-1";
  chat.native_session_id = "native-session-1";
  chat.native_session_file_name = "session-2026-03-27T12-00-native.json";
  chat.native_project_root = "/tmp/native-project";
  chat.uses_native_session = true;
  chat.folder_id = "folder-default";
  chat.template_override_id = "custom-template.md";
  chat.title = "Template Test";
  chat.created_at = "2026-03-19 10:11:12";
  chat.updated_at = "2026-03-19 10:11:13";
  chat.messages.push_back(Message{MessageRole::User, "hello", "2026-03-19 10:11:13"});

  UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));
  const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
  UAM_ASSERT_EQ(1u, loaded.size());
  UAM_ASSERT_EQ(std::string("native-session-1"), loaded.front().native_session_id);
  UAM_ASSERT_EQ(std::string("session-2026-03-27T12-00-native.json"), loaded.front().native_session_file_name);
  UAM_ASSERT_EQ(std::string("/tmp/native-project"), loaded.front().native_project_root);
  UAM_ASSERT_EQ(std::string("custom-template.md"), loaded.front().template_override_id);
}

UAM_TEST(TestBuildImportedChatTitleUsesUserPromptSection) {
  std::vector<Message> messages;
  messages.push_back(Message{
      MessageRole::User,
      "Retrieved context:\n1. snippet\n\nUser prompt:\nwhat is the difference between setup wizard and testing suite?",
      "2026-03-19 10:11:13"});

  const std::string title = uam::BuildImportedChatTitle(messages, "2026-03-19 10:11:12", 120);
  UAM_ASSERT_EQ(std::string("what is the difference between setup wizard and testing suite?"), title);
}

UAM_TEST(TestBuildImportedChatTitleFallsBackToFirstPromptLine) {
  std::vector<Message> messages;
  messages.push_back(Message{MessageRole::System, "ignored", "2026-03-19 10:11:13"});
  messages.push_back(Message{
      MessageRole::User,
      "@.gemini/gemini.md\n\nEnsure the directory structure '.gemini/Memory/Lessons/' and '.gemini/Memory/Failures/' is present.",
      "2026-03-19 10:11:13"});

  const std::string title = uam::BuildImportedChatTitle(messages, "2026-03-19 10:11:12", 120);
  UAM_ASSERT_EQ(std::string("Ensure the directory structure '.gemini/Memory/Lessons/' and '.gemini/Memory/Failures/' is present."),
                title);
}

UAM_TEST(TestBuildImportedChatTitleTruncatesLongPrompt) {
  std::vector<Message> messages;
  messages.push_back(Message{MessageRole::User, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ", "2026-03-19 10:11:13"});

  const std::string title = uam::BuildImportedChatTitle(messages, "2026-03-19 10:11:12", 12);
  UAM_ASSERT_EQ(std::string("abcdefghi..."), title);
}

UAM_TEST(TestBuildFolderTitleFromProjectRootUsesLeafDirectory) {
  const std::string title = uam::BuildFolderTitleFromProjectRoot(
      fs::path("/Users/davidtaylormacbookpro/Documents/GitHub/Universal Agent Manager"));
  UAM_ASSERT_EQ(std::string("Universal Agent Manager"), title);
}

UAM_TEST(TestResolveImportedProjectRootOrFallbackUsesExistingDirectory) {
  TempDir existing_root("uam-import-existing-root");
  TempDir fallback_root("uam-import-fallback-root");

  UAM_ASSERT(uam::ImportedProjectRootExists(existing_root.root));
  const fs::path resolved = uam::ResolveImportedProjectRootOrFallback(existing_root.root, fallback_root.root);
  UAM_ASSERT_EQ(existing_root.root.lexically_normal(), resolved);
}

UAM_TEST(TestResolveImportedProjectRootOrFallbackUsesFallbackForMissingDirectory) {
  TempDir fallback_root("uam-import-missing-fallback-root");
  const fs::path missing_root = fallback_root.root / "missing-workspace";

  UAM_ASSERT(!uam::ImportedProjectRootExists(missing_root));
  const fs::path resolved = uam::ResolveImportedProjectRootOrFallback(missing_root, fallback_root.root);
  UAM_ASSERT_EQ(fallback_root.root.lexically_normal(), resolved);
}

UAM_TEST(TestGeminiCliCompatSupportsAllowlistedVersions) {
  UAM_ASSERT_EQ("0.34.0", std::string(uam::PreferredGeminiCliVersion()));
  UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.34.0"));
  UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.30.0"));
  UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.35.0"));
  UAM_ASSERT_EQ("0.34.0, 0.30.0", uam::SupportedGeminiCliVersionsLabel());
}

UAM_TEST(TestGeminiTemplateCatalogImportCollisionAndFiltering) {
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
  UAM_ASSERT(GeminiTemplateCatalog::ImportMarkdownTemplate(global_root.root, file_a, &imported_a, &error));
  UAM_ASSERT(GeminiTemplateCatalog::ImportMarkdownTemplate(global_root.root, file_b, &imported_b, &error));
  UAM_ASSERT(imported_a != imported_b);

  UAM_ASSERT(!GeminiTemplateCatalog::ImportMarkdownTemplate(global_root.root, non_markdown, nullptr, &error));

  const fs::path catalog_path = GeminiTemplateCatalog::CatalogPath(global_root.root);
  UAM_ASSERT(WriteTextFile(catalog_path / "not-a-template.txt", "ignored"));

  const std::vector<TemplateCatalogEntry> entries = GeminiTemplateCatalog::List(global_root.root);
  UAM_ASSERT_EQ(2u, entries.size());
  UAM_ASSERT(GeminiTemplateCatalog::HasTemplate(global_root.root, imported_a));
  UAM_ASSERT(GeminiTemplateCatalog::HasTemplate(global_root.root, imported_b));
}

UAM_TEST(TestGeminiCommandBuilderInteractiveArgvIncludesResumeAndFlags) {
  AppSettings settings;
  settings.gemini_yolo_mode = true;
  settings.gemini_extra_flags = "--profile nightly --dry-run";

  ChatSession chat;
  chat.uses_native_session = true;
  chat.native_session_id = "session-123";

  const std::vector<std::string> argv = GeminiCommandBuilder::BuildInteractiveArgv(chat, settings);
  const std::vector<std::string> expected = {
      "gemini",
      "--yolo",
      "--profile",
      "nightly",
      "--dry-run",
      "--resume",
      "session-123",
  };
  UAM_ASSERT(argv == expected);
}

UAM_TEST(TestGeminiCommandBuilderParsesWindowsPathFlags) {
#if defined(_WIN32)
  AppSettings settings;
  settings.gemini_extra_flags = R"(--config "C:\Users\david\gemini\settings.json" --profile nightly)";

  ChatSession chat;
  const std::vector<std::string> argv = GeminiCommandBuilder::BuildInteractiveArgv(chat, settings);
  const std::vector<std::string> expected = {
      "gemini",
      "--config",
      R"(C:\Users\david\gemini\settings.json)",
      "--profile",
      "nightly",
  };
  UAM_ASSERT(argv == expected);
#endif
}

UAM_TEST(TestProviderProfileStoreRoundTrip) {
  TempDir data_root("uam-provider-profiles");
  std::vector<ProviderProfile> profiles;

  ProviderProfile custom = ProviderProfileStore::DefaultGeminiProfile();
  custom.id = "custom-cli";
  custom.title = "Custom CLI";
  custom.command_template = "custom-cli {prompt}";
  custom.interactive_command = "custom-cli --interactive";
  custom.supports_resume = false;
  custom.resume_argument = "--continue";
  custom.history_adapter = "local-only";
  custom.user_message_types = {"user"};
  custom.assistant_message_types = {"assistant", "bot"};
  profiles.push_back(custom);

  UAM_ASSERT(ProviderProfileStore::Save(data_root.root, profiles));

  const std::vector<ProviderProfile> loaded = ProviderProfileStore::Load(data_root.root);
  const ProviderProfile* found = ProviderProfileStore::FindById(loaded, "custom-cli");
  UAM_ASSERT(found != nullptr);
  UAM_ASSERT_EQ(std::string("Custom CLI"), found->title);
  UAM_ASSERT_EQ(std::string("custom-cli {prompt}"), found->command_template);
  UAM_ASSERT(found->supports_resume == false);
  UAM_ASSERT_EQ(std::string("--continue"), found->resume_argument);
  UAM_ASSERT_EQ(std::string("local-only"), found->history_adapter);
}

UAM_TEST(TestProviderRuntimeParsesWindowsInteractiveCommandPath) {
#if defined(_WIN32)
  ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
  profile.interactive_command = R"("C:\Program Files\Gemini\gemini.exe" --interactive)";
  profile.supports_resume = false;
  profile.resume_argument.clear();

  AppSettings settings;
  ChatSession chat;
  const std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
  const std::vector<std::string> expected = {
      R"(C:\Program Files\Gemini\gemini.exe)",
      "--interactive",
  };
  UAM_ASSERT(argv == expected);
#endif
}

UAM_TEST(TestProviderRuntimeRoleMappingHonorsProfileTypes) {
  ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
  profile.user_message_types = {"human", "user"};
  profile.assistant_message_types = {"assistant", "model", "gemini", "bot"};

  UAM_ASSERT(ProviderRuntime::RoleFromNativeType(profile, "human") == MessageRole::User);
  UAM_ASSERT(ProviderRuntime::RoleFromNativeType(profile, "BOT") == MessageRole::Assistant);
  UAM_ASSERT(ProviderRuntime::RoleFromNativeType(profile, "tool") == MessageRole::System);
}

UAM_TEST(TestProviderRuntimeMergesProfileFlags) {
  ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
  profile.runtime_flags = {"--profile", "nightly"};

  AppSettings settings;
  settings.gemini_extra_flags = "--dry-run";
  settings.gemini_command_template = "gemini {flags} {prompt}";

  const std::string command =
      ProviderRuntime::BuildCommand(profile, settings, "hello", std::vector<std::string>{}, std::string{});
  UAM_ASSERT(command.find("--profile") != std::string::npos);
  UAM_ASSERT(command.find("nightly") != std::string::npos);
  UAM_ASSERT(command.find("--dry-run") != std::string::npos);
}

UAM_TEST(TestFrontendActionMapRoundTrip) {
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

int main() {
  int failures = 0;
  for (const TestCase& test : Registry()) {
    try {
      test.fn();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << " - " << failure.what() << '\n';
    } catch (const std::exception& ex) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << " - unexpected exception: " << ex.what() << '\n';
    } catch (...) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << " - unknown exception\n";
    }
  }

  if (failures != 0) {
    std::cerr << failures << " test(s) failed.\n";
    return 1;
  }

  std::cout << Registry().size() << " test(s) passed.\n";
  return 0;
}
