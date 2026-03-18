#include "core/app_models.h"
#include "core/app_paths.h"
#include "core/chat_folder_store.h"
#include "core/chat_repository.h"
#include "core/frontend_actions.h"
#include "core/gemini_command_builder.h"
#include "core/provider_profile.h"
#include "core/provider_runtime.h"
#include "core/settings_store.h"
#include "core/chat_sync.h"
#include "core/terminal_polling.h"
#include "core/terminal_typography.h"

#include <filesystem>
#include <fstream>
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

UAM_TEST(TestProviderRuntimeForcesYoloOnlyWhenStructuredNeedsIt) {
  ProviderProfile gemini_profile = ProviderProfileStore::DefaultGeminiProfile();
  gemini_profile.history_adapter = "gemini-cli-json";

  AppSettings settings;
  settings.gemini_yolo_mode = false;

  UAM_ASSERT(ProviderRuntime::ShouldForceYoloForStructuredMode(gemini_profile, settings, CenterViewMode::Structured));
  UAM_ASSERT(!ProviderRuntime::ShouldForceYoloForStructuredMode(gemini_profile, settings, CenterViewMode::CliConsole));

  settings.gemini_yolo_mode = true;
  UAM_ASSERT(!ProviderRuntime::ShouldForceYoloForStructuredMode(gemini_profile, settings, CenterViewMode::Structured));

  ProviderProfile custom_profile = gemini_profile;
  custom_profile.history_adapter = "local-only";
  settings.gemini_yolo_mode = false;
  UAM_ASSERT(!ProviderRuntime::ShouldForceYoloForStructuredMode(custom_profile, settings, CenterViewMode::Structured));
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

UAM_TEST(TestTerminalDrainBudgetStopsAfterReadLimit) {
  uam::TerminalDrainBudget budget;
  int drained_reads = 0;
  while (budget.CanDrainMore()) {
    budget.RecordRead(1);
    ++drained_reads;
  }

  UAM_ASSERT_EQ(static_cast<std::size_t>(uam::TerminalDrainBudget::kDefaultMaxReads), budget.reads);
  UAM_ASSERT_EQ(static_cast<std::size_t>(uam::TerminalDrainBudget::kDefaultMaxReads), budget.bytes);
  UAM_ASSERT_EQ(static_cast<int>(uam::TerminalDrainBudget::kDefaultMaxReads), drained_reads);
  UAM_ASSERT(!budget.CanDrainMore());
}

UAM_TEST(TestTerminalDrainBudgetStopsAfterByteLimit) {
  uam::TerminalDrainBudget budget;
  budget.max_reads = 100;
  budget.max_bytes = 10;

  int drained_reads = 0;
  while (budget.CanDrainMore()) {
    budget.RecordRead(4);
    ++drained_reads;
  }

  UAM_ASSERT_EQ(static_cast<std::size_t>(3), budget.reads);
  UAM_ASSERT_EQ(static_cast<std::size_t>(12), budget.bytes);
  UAM_ASSERT_EQ(static_cast<int>(3), drained_reads);
  UAM_ASSERT(!budget.CanDrainMore());
}

UAM_TEST(TestTerminalHistorySyncGateOnlySyncsAfterStop) {
  UAM_ASSERT(!uam::ShouldSyncNativeHistoryAfterTerminalPoll(true));
  UAM_ASSERT(uam::ShouldSyncNativeHistoryAfterTerminalPoll(false));
}

UAM_TEST(TestTerminalBackgroundDrainBudgetIsSmaller) {
  const uam::TerminalDrainBudget foreground = uam::TerminalDrainBudgetForView(false);
  const uam::TerminalDrainBudget background = uam::TerminalDrainBudgetForView(true);

  UAM_ASSERT_EQ(uam::TerminalDrainBudget::kDefaultMaxReads, foreground.max_reads);
  UAM_ASSERT_EQ(uam::TerminalDrainBudget::kDefaultMaxBytes, foreground.max_bytes);
  UAM_ASSERT_EQ(uam::TerminalDrainBudget::kBackgroundMaxReads, background.max_reads);
  UAM_ASSERT_EQ(uam::TerminalDrainBudget::kBackgroundMaxBytes, background.max_bytes);
  UAM_ASSERT(background.max_reads < foreground.max_reads);
  UAM_ASSERT(background.max_bytes < foreground.max_bytes);
}

UAM_TEST(TestTerminalBackgroundPollGateHonorsInterval) {
  UAM_ASSERT(uam::ShouldPollBackgroundTerminalNow(1.00, 0.0, 0.10));
  UAM_ASSERT(!uam::ShouldPollBackgroundTerminalNow(1.05, 1.00, 0.10));
  UAM_ASSERT(uam::ShouldPollBackgroundTerminalNow(1.10, 1.00, 0.10));
}

UAM_TEST(TestTerminalTypographyPrefersReadabilityOnWindows) {
  const uam::TerminalTypographyConfig& typography = uam::TerminalTypographyConfigForPlatform();

#if defined(_WIN32)
  UAM_ASSERT(typography.cell_width_scale > 1.0f);
  UAM_ASSERT(typography.mono_font_size >= 14.0f);
  UAM_ASSERT(!typography.readable_mono_font_candidates.empty());
  UAM_ASSERT(!typography.fallback_font_candidates.empty());
  UAM_ASSERT_EQ(std::string("C:/Windows/Fonts/consolab.ttf"),
                std::string(typography.readable_mono_font_candidates.front()));
  UAM_ASSERT_EQ(std::string("C:/Windows/Fonts/segoeui.ttf"),
                std::string(typography.fallback_font_candidates.front()));
#else
  UAM_ASSERT_EQ(1.0f, typography.cell_width_scale);
  UAM_ASSERT_EQ(13.5f, typography.mono_font_size);
  UAM_ASSERT(!typography.fallback_font_candidates.empty());
#endif
}

UAM_TEST(TestChatFolderStoreRoundTripPreservesCustomFolderMetadata) {
  TempDir data_root("uam-folders");

  std::vector<ChatFolder> folders;
  ChatFolder general;
  general.id = "folder-default";
  general.title = "General";
  general.directory = (data_root.root / "root").generic_string();
  general.collapsed = false;
  folders.push_back(general);

  ChatFolder gemini;
  gemini.id = "folder-gemini";
  gemini.title = "Gemini Project";
  gemini.directory = (data_root.root / "gemini-workspace").generic_string();
  gemini.collapsed = true;
  folders.push_back(gemini);

  UAM_ASSERT(ChatFolderStore::Save(data_root.root, folders));

  const std::vector<ChatFolder> loaded = ChatFolderStore::Load(data_root.root);
  UAM_ASSERT_EQ(static_cast<std::size_t>(2), loaded.size());

  const ChatFolder* loaded_general = nullptr;
  const ChatFolder* loaded_gemini = nullptr;
  for (const ChatFolder& folder : loaded) {
    if (folder.id == general.id) {
      loaded_general = &folder;
    } else if (folder.id == gemini.id) {
      loaded_gemini = &folder;
    }
  }

  UAM_ASSERT(loaded_general != nullptr);
  UAM_ASSERT(loaded_gemini != nullptr);
  UAM_ASSERT_EQ(std::string("General"), loaded_general->title);
  UAM_ASSERT_EQ(general.directory, loaded_general->directory);
  UAM_ASSERT(loaded_general->collapsed == false);
  UAM_ASSERT_EQ(std::string("Gemini Project"), loaded_gemini->title);
  UAM_ASSERT_EQ(gemini.directory, loaded_gemini->directory);
  UAM_ASSERT(loaded_gemini->collapsed == true);
}

UAM_TEST(TestPromoteDraftChatToNativePersistsCustomTitleAndFolderAndHidesDuplicate) {
  TempDir data_root("uam-chat-promote");

  ChatSession draft;
  draft.id = "chat-draft-001";
  draft.title = "My Renamed Chat";
  draft.folder_id = "folder-42";
  draft.created_at = "2026-03-18 10:00:00";
  draft.updated_at = "2026-03-18 10:05:00";
  draft.messages.push_back(Message{MessageRole::User, "hello", draft.updated_at});
  UAM_ASSERT(ChatRepository::SaveChat(data_root.root, draft));

  ChatSession native;
  native.id = "session-123";
  native.uses_native_session = true;
  native.native_session_id = "session-123";
  native.title = "Original Gemini Title";
  native.folder_id = "folder-default";
  native.created_at = "2026-03-18 10:00:01";
  native.updated_at = "2026-03-18 10:06:00";
  native.messages.push_back(Message{MessageRole::User, "hello", native.updated_at});

  UAM_ASSERT(ChatRepository::PromoteDraftChatToNative(data_root.root, draft, native));

  const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(data_root.root);
  const ChatSession* mirrored = nullptr;
  for (const ChatSession& chat : loaded) {
    if (chat.id == native.id) {
      mirrored = &chat;
      break;
    }
  }

  UAM_ASSERT(mirrored != nullptr);
  UAM_ASSERT(mirrored->uses_native_session);
  UAM_ASSERT_EQ(std::string("session-123"), mirrored->native_session_id);
  UAM_ASSERT_EQ(std::string("My Renamed Chat"), mirrored->title);
  UAM_ASSERT_EQ(std::string("folder-42"), mirrored->folder_id);
  UAM_ASSERT_EQ(static_cast<std::size_t>(1), mirrored->messages.size());
  UAM_ASSERT_EQ(std::string("hello"), mirrored->messages.front().content);

  const std::vector<ChatSession> native_chats = {
      native,
  };
  const std::vector<ChatSession> merged = uam::MergeNativeAndLocalChats(native_chats, loaded);
  UAM_ASSERT_EQ(static_cast<std::size_t>(1), merged.size());
  UAM_ASSERT_EQ(std::string("session-123"), merged.front().id);
  UAM_ASSERT_EQ(std::string("My Renamed Chat"), merged.front().title);
  UAM_ASSERT_EQ(std::string("folder-42"), merged.front().folder_id);
  UAM_ASSERT(merged.front().uses_native_session);
  UAM_ASSERT_EQ(std::string("session-123"), merged.front().native_session_id);

  const std::vector<ChatSession> fallback_only = uam::MergeNativeAndLocalChats({}, loaded);
  UAM_ASSERT_EQ(static_cast<std::size_t>(1), fallback_only.size());
  UAM_ASSERT_EQ(std::string("session-123"), fallback_only.front().id);
  UAM_ASSERT(fallback_only.front().uses_native_session);
  UAM_ASSERT_EQ(std::string("My Renamed Chat"), fallback_only.front().title);
  UAM_ASSERT_EQ(std::string("folder-42"), fallback_only.front().folder_id);
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
