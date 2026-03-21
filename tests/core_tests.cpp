#include "core/app_models.h"
#include "core/app_paths.h"
#include "core/chat_branching.h"
#include "core/chat_repository.h"
#include "core/frontend_actions.h"
#include "core/gemini_command_builder.h"
#include "core/gemini_template_catalog.h"
#include "core/provider_profile.h"
#include "core/provider_runtime.h"
#include "core/rag_index_service.h"
#include "core/settings_store.h"
#include "core/ollama_engine_client.h"
#include "core/vcs_workspace_service.h"

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
  chat.folder_id = "folder-default";
  chat.template_override_id = "custom-template.md";
  chat.title = "Template Test";
  chat.created_at = "2026-03-19 10:11:12";
  chat.updated_at = "2026-03-19 10:11:13";
  chat.messages.push_back(Message{MessageRole::User, "hello", "2026-03-19 10:11:13"});

  UAM_ASSERT(ChatRepository::SaveChat(data_root.root, chat));
  const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
  UAM_ASSERT_EQ(1u, loaded.size());
  UAM_ASSERT_EQ(std::string("custom-template.md"), loaded.front().template_override_id);
}

UAM_TEST(TestChatRepositoryPersistsBranchMetadata) {
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

UAM_TEST(TestChatRepositoryDefaultsMissingBranchMetadata) {
  TempDir data_root("uam-chat-branch-defaults");
  const fs::path chat_root = data_root.root / "chats" / "chat-legacy";
  const fs::path messages_root = chat_root / "messages";
  fs::create_directories(messages_root);
  UAM_ASSERT(WriteTextFile(chat_root / "meta.txt",
                           "id=chat-legacy\n"
                           "title=Legacy\n"
                           "created_at=2026-03-21 11:00:00\n"
                           "updated_at=2026-03-21 11:00:01\n"));
  UAM_ASSERT(WriteTextFile(messages_root / "000001_user.txt", "hello"));

  const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
  UAM_ASSERT_EQ(1u, loaded.size());
  UAM_ASSERT_EQ(std::string("chat-legacy"), loaded.front().branch_root_chat_id);
  UAM_ASSERT_EQ(std::string(""), loaded.front().parent_chat_id);
  UAM_ASSERT_EQ(-1, loaded.front().branch_from_message_index);
}

UAM_TEST(TestChatBranchingReparentChildrenAfterDelete) {
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
  const auto find_by_id = [&](const std::string& id) -> const ChatSession* {
    for (const ChatSession& chat : chats) {
      if (chat.id == id) {
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

UAM_TEST(TestRagIndexServiceIndexesRetrievesAndCites) {
  TempDir workspace("uam-rag-workspace");
  UAM_ASSERT(WriteTextFile(workspace.root / "alpha.txt",
                           "First line about deployment\n"
                           "Second line about branches\n"
                           "Third line about release notes\n"));
  UAM_ASSERT(WriteTextFile(workspace.root / "beta.txt",
                           "A different topic with infra setup\n"
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

UAM_TEST(TestRagIndexServiceIncrementalRefreshDetectsChanges) {
  TempDir workspace("uam-rag-incremental");
  const fs::path file = workspace.root / "notes.txt";
  UAM_ASSERT(WriteTextFile(file, "line one\nline two\n"));

  RagIndexService rag;
  RagRefreshResult first = rag.RefreshIndexIncremental(workspace.root);
  UAM_ASSERT(first.ok);
  UAM_ASSERT(first.indexed_files >= 1);

  RagRefreshResult second = rag.RefreshIndexIncremental(workspace.root);
  UAM_ASSERT(second.ok);
  UAM_ASSERT_EQ(0, second.updated_files);

  {
    std::error_code ec;
    const fs::file_time_type current_mtime = fs::last_write_time(file, ec);
    UAM_ASSERT(!ec);
    fs::last_write_time(file, current_mtime + std::chrono::seconds(2), ec);
    UAM_ASSERT(!ec);
  }
  RagRefreshResult third = rag.RefreshIndexIncremental(workspace.root);
  UAM_ASSERT(third.ok);
  UAM_ASSERT_EQ(0, third.updated_files);

  UAM_ASSERT(WriteTextFile(file, "line one\nline two changed\n"));
  RagRefreshResult fourth = rag.RefreshIndexIncremental(workspace.root);
  UAM_ASSERT(fourth.ok);
  UAM_ASSERT(fourth.updated_files >= 1);
}

UAM_TEST(TestRagIndexServiceFiltersBinaryAndLargeFiles) {
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

UAM_TEST(TestRagIndexServiceRetrievalOrderingAndTopK) {
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

UAM_TEST(TestOllamaEngineInterfaceLifecycle) {
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
  std::thread load_worker([&]() {
    load_ok = engine.Load("alpha.gguf", &load_error);
  });
  for (int i = 0; i < 80; ++i) {
    const ollama_engine::CurrentStateResponse state = engine.QueryCurrentState();
    if (state.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Loading &&
        state.pOptLoadingStructure.has_value()) {
      saw_loading = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  load_worker.join();
  UAM_ASSERT(load_ok);
  UAM_ASSERT(saw_loading);

  const ollama_engine::CurrentStateResponse loaded = engine.QueryCurrentState();
  UAM_ASSERT(loaded.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Loaded);
  UAM_ASSERT_EQ(std::string("alpha.gguf"), loaded.pSLoadedModelName);

  bool saw_active_generation = false;
  bool saw_finished = false;
  ollama_engine::SendMessageResponse message_response;
  std::thread worker([&]() {
    message_response = engine.SendMessage(std::string(600, 'x'));
  });
  for (int i = 0; i < 300; ++i) {
    const ollama_engine::CurrentStateResponse state = engine.QueryCurrentState();
    if (state.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Running ||
        state.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Thinking) {
      saw_active_generation = true;
    }
    if (state.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Finished) {
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

UAM_TEST(TestRagIndexServiceVectorRetrievalWithEngineModel) {
  TempDir workspace("uam-rag-vector");
  TempDir model_dir("uam-rag-vector-models");
  UAM_ASSERT(WriteTextFile(model_dir.root / "mini.gguf", "fake-model"));
  UAM_ASSERT(WriteTextFile(workspace.root / "deployment.md",
                           "How to deploy service A\n"
                           "Use canary migrations and rollout checks\n"));
  UAM_ASSERT(WriteTextFile(workspace.root / "notes.txt",
                           "Team lunch plans\n"
                           "No production rollout guidance here\n"));

  RagIndexService::Config config;
  config.vector_enabled = true;
  config.vector_dimensions = 96;
  config.top_k = 2;
  RagIndexService rag(config);
  rag.SetModelFolder(model_dir.root);

  const std::vector<std::string> models = rag.ListModels();
  UAM_ASSERT_EQ(1u, models.size());
  UAM_ASSERT(rag.LoadModel("mini.gguf"));

  const RagRefreshResult refresh = rag.RefreshIndexIncremental(workspace.root);
  UAM_ASSERT(refresh.ok);

  const std::vector<RagSnippet> snippets = rag.RetrieveTopK(workspace.root, "canary rollout migration");
  UAM_ASSERT(!snippets.empty());
  UAM_ASSERT_EQ(std::string("deployment.md"), snippets.front().relative_path);
}

UAM_TEST(TestVcsWorkspaceServiceHandlesNonRepoWorkspace) {
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

UAM_TEST(TestVcsWorkspaceServiceAppliesOutputCapsAndTimeout) {
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
  fs::permissions(svn_binary,
                  fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                      fs::perms::group_read | fs::perms::group_exec |
                      fs::perms::others_read | fs::perms::others_exec,
                  fs::perm_options::replace,
                  chmod_ec);
  UAM_ASSERT(!chmod_ec);

  std::string path_value = fake_bin.root.string();
  if (const char* existing_path = std::getenv("PATH")) {
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
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start)
                              .count();
  UAM_ASSERT(!diff.ok);
  UAM_ASSERT(diff.timed_out);
  UAM_ASSERT(elapsed_ms < 7800);
#endif
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
