#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <vterm.h>
#include "core/app_models.h"
#include "core/app_paths.h"
#include "core/chat_folder_store.h"
#include "core/chat_repository.h"
#include "core/chat_sync.h"
#include "core/frontend_actions.h"
#include "core/gemini_command_builder.h"
#include "core/provider_profile.h"
#include "core/provider_runtime.h"
#include "core/settings_store.h"
#include "core/terminal_polling.h"
#include "core/terminal_typography.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincontypes.h>
#endif

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace fs = std::filesystem;

#if defined(_WIN32)
static void ClosePseudoConsoleSafe(HPCON handle);
#endif

struct CliTerminalIoState {
#if defined(_WIN32)
  HANDLE pipe_input = INVALID_HANDLE_VALUE;
  HANDLE pipe_output = INVALID_HANDLE_VALUE;
#endif
  std::mutex mutex;
  std::deque<std::string> pending_input;
  std::deque<std::string> pending_output;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> output_closed{false};
  std::atomic<bool> read_failed{false};
  std::atomic<bool> write_failed{false};
};

struct CliTerminalState {
#if defined(_WIN32)
  HANDLE pipe_input = INVALID_HANDLE_VALUE;
  HANDLE pipe_output = INVALID_HANDLE_VALUE;
  PROCESS_INFORMATION process_info = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, 0, 0};
  HPCON pseudo_console = nullptr;
  LPPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;
#else
  int master_fd = -1;
  pid_t child_pid = -1;
#endif
  bool running = false;
  std::string attached_chat_id;
  std::string attached_session_id;
  std::vector<std::string> session_ids_before;
  std::vector<std::string> linked_files_snapshot;
  VTerm* vt = nullptr;
  VTermScreen* screen = nullptr;
  VTermState* state = nullptr;
  int rows = 24;
  int cols = 80;
  bool scroll_to_bottom = false;
  bool needs_full_refresh = true;
  bool should_launch = false;
  double last_sync_time_s = 0.0;
  double last_background_poll_time_s = 0.0;
  double last_text_cache_refresh_s = 0.0;
  std::vector<std::string> rendered_rows;
  std::shared_ptr<CliTerminalIoState> io_state;
  std::string last_error;
};

struct AppState {
  fs::path data_root;
  fs::path gemini_chats_dir;
  AppSettings settings;
  std::vector<ChatFolder> folders;
  std::vector<ProviderProfile> provider_profiles;
  uam::FrontendActionMap frontend_actions;

  std::vector<ChatSession> chats;
  int selected_chat_index = -1;

  std::string composer_text;
  std::string attach_file_input;
  std::string new_chat_folder_id;
  std::string new_folder_title_input;
  std::string new_folder_directory_input;
  std::string editing_chat_id;
  int editing_message_index = -1;
  std::string editing_message_text;
  bool open_edit_message_popup = false;
  std::string status_line;
  CenterViewMode center_view_mode = CenterViewMode::CliConsole;
  std::vector<std::unique_ptr<CliTerminalState>> cli_terminals;

  std::optional<PendingGeminiCall> pending_call;
  bool scroll_to_bottom = false;

  std::future<std::vector<ChatSession>> deferred_native_history_load;
  bool deferred_native_history_loading = false;
};

#if defined(_WIN32)
static bool StartCliTerminalWindows(AppState& app, CliTerminalState& terminal, const ChatSession& chat);
#else
static bool StartCliTerminalUnix(AppState& app, CliTerminalState& terminal, const ChatSession& chat);
#endif

static ImFont* g_font_ui = nullptr;
static ImFont* g_font_title = nullptr;
static ImFont* g_font_mono = nullptr;
static ImFont* g_font_mono_readable = nullptr;
static ImVector<ImWchar> g_terminal_glyph_ranges;

static constexpr const char* kDefaultFolderId = "folder-default";
static constexpr const char* kDefaultFolderTitle = "General";
static constexpr int kMinTerminalRows = 8;
static constexpr int kMinTerminalCols = 20;
static constexpr int kMaxTerminalRows = 120;
static constexpr int kMaxTerminalCols = 240;
static constexpr double kTerminalTextRefreshIntervalSeconds = 1.0 / 30.0;

static void SortChatsByRecent(std::vector<ChatSession>& chats);

static int ClampTerminalRows(const int rows) {
  return std::clamp(rows, kMinTerminalRows, kMaxTerminalRows);
}

static int ClampTerminalCols(const int cols) {
  return std::clamp(cols, kMinTerminalCols, kMaxTerminalCols);
}

static bool ShouldRefreshTerminalTextCache(const CliTerminalState& terminal, const double now_s) {
  if (!terminal.needs_full_refresh) {
    return false;
  }
  if (terminal.rendered_rows.empty() || !terminal.running) {
    return true;
  }
  return (now_s - terminal.last_text_cache_refresh_s) >= kTerminalTextRefreshIntervalSeconds;
}

static std::string Trim(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

static std::string TimestampNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &tt);
#else
  localtime_r(&tt, &tm_snapshot);
#endif
  std::ostringstream out;
  out << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
  return out.str();
}

static std::string NewSessionId() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> hex_digit(0, 15);
  std::ostringstream id;
  id << "chat-" << epoch_ms << "-";
  for (int i = 0; i < 6; ++i) {
    id << std::hex << hex_digit(rng);
  }
  return id.str();
}


static std::string ReadTextFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

static bool WriteTextFile(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    return false;
  }
  out << content;
  return out.good();
}

struct JsonValue {
  enum class Type {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
  };

  Type type = Type::Null;
  bool bool_value = false;
  double number_value = 0.0;
  std::string string_value;
  std::vector<JsonValue> array_value;
  std::unordered_map<std::string, JsonValue> object_value;

  const JsonValue* Find(const std::string& key) const {
    const auto it = object_value.find(key);
    return (it == object_value.end()) ? nullptr : &it->second;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  std::optional<JsonValue> Parse() {
    SkipWhitespace();
    JsonValue value = ParseValue();
    if (error_) {
      return std::nullopt;
    }
    SkipWhitespace();
    if (pos_ != input_.size()) {
      return std::nullopt;
    }
    return value;
  }

 private:
  JsonValue ParseValue() {
    SkipWhitespace();
    if (pos_ >= input_.size()) {
      error_ = true;
      return {};
    }
    const char ch = input_[pos_];
    if (ch == '{') {
      return ParseObject();
    }
    if (ch == '[') {
      return ParseArray();
    }
    if (ch == '"') {
      JsonValue out;
      out.type = JsonValue::Type::String;
      out.string_value = ParseString();
      return out;
    }
    if (ch == 't' || ch == 'f') {
      return ParseBool();
    }
    if (ch == 'n') {
      return ParseNull();
    }
    return ParseNumber();
  }

  JsonValue ParseObject() {
    JsonValue out;
    out.type = JsonValue::Type::Object;
    if (!Consume('{')) {
      error_ = true;
      return {};
    }
    SkipWhitespace();
    if (Consume('}')) {
      return out;
    }
    while (!error_) {
      SkipWhitespace();
      if (!Consume('"')) {
        error_ = true;
        break;
      }
      const std::string key = ParseStringBody();
      if (!Consume(':')) {
        error_ = true;
        break;
      }
      JsonValue value = ParseValue();
      out.object_value.emplace(key, std::move(value));
      SkipWhitespace();
      if (Consume('}')) {
        break;
      }
      if (!Consume(',')) {
        error_ = true;
        break;
      }
    }
    return out;
  }

  JsonValue ParseArray() {
    JsonValue out;
    out.type = JsonValue::Type::Array;
    if (!Consume('[')) {
      error_ = true;
      return {};
    }
    SkipWhitespace();
    if (Consume(']')) {
      return out;
    }
    while (!error_) {
      out.array_value.push_back(ParseValue());
      SkipWhitespace();
      if (Consume(']')) {
        break;
      }
      if (!Consume(',')) {
        error_ = true;
        break;
      }
    }
    return out;
  }

  JsonValue ParseBool() {
    JsonValue out;
    out.type = JsonValue::Type::Bool;
    if (MatchLiteral("true")) {
      out.bool_value = true;
      return out;
    }
    if (MatchLiteral("false")) {
      out.bool_value = false;
      return out;
    }
    error_ = true;
    return {};
  }

  JsonValue ParseNull() {
    JsonValue out;
    out.type = JsonValue::Type::Null;
    if (!MatchLiteral("null")) {
      error_ = true;
    }
    return out;
  }

  JsonValue ParseNumber() {
    JsonValue out;
    out.type = JsonValue::Type::Number;
    const std::size_t start = pos_;
    if (Peek() == '-') {
      ++pos_;
    }
    while (std::isdigit(static_cast<unsigned char>(Peek()))) {
      ++pos_;
    }
    if (Peek() == '.') {
      ++pos_;
      while (std::isdigit(static_cast<unsigned char>(Peek()))) {
        ++pos_;
      }
    }
    if (Peek() == 'e' || Peek() == 'E') {
      ++pos_;
      if (Peek() == '+' || Peek() == '-') {
        ++pos_;
      }
      while (std::isdigit(static_cast<unsigned char>(Peek()))) {
        ++pos_;
      }
    }
    const std::string token(input_.substr(start, pos_ - start));
    if (token.empty()) {
      error_ = true;
      return {};
    }
    try {
      out.number_value = std::stod(token);
    } catch (...) {
      error_ = true;
    }
    return out;
  }

  std::string ParseString() {
    if (!Consume('"')) {
      error_ = true;
      return {};
    }
    return ParseStringBody();
  }

  std::string ParseStringBody() {
    std::string out;
    while (pos_ < input_.size()) {
      const char ch = input_[pos_++];
      if (ch == '"') {
        return out;
      }
      if (ch == '\\') {
        if (pos_ >= input_.size()) {
          error_ = true;
          return {};
        }
        const char esc = input_[pos_++];
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            out.push_back(esc);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u':
            if (pos_ + 4 <= input_.size()) {
              // Keep unicode escape literal if we can't decode it safely here.
              out += "\\u";
              out.append(input_.substr(pos_, 4));
              pos_ += 4;
            } else {
              error_ = true;
            }
            break;
          default:
            error_ = true;
            break;
        }
        if (error_) {
          return {};
        }
      } else {
        out.push_back(ch);
      }
    }
    error_ = true;
    return {};
  }

  bool Consume(const char expected) {
    SkipWhitespace();
    if (pos_ < input_.size() && input_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool MatchLiteral(const char* literal) {
    const std::size_t len = std::strlen(literal);
    if (pos_ + len > input_.size()) {
      return false;
    }
    if (input_.substr(pos_, len) == literal) {
      pos_ += len;
      return true;
    }
    return false;
  }

  char Peek() const {
    return (pos_ < input_.size()) ? input_[pos_] : '\0';
  }

  void SkipWhitespace() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  }

  std::string_view input_;
  std::size_t pos_ = 0;
  bool error_ = false;
};

static std::optional<JsonValue> ParseJson(const std::string& text) {
  JsonParser parser(text);
  return parser.Parse();
}

static void AppendJsonEscapedString(const std::string& value, std::string& out) {
  out.push_back('"');
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
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
        if (ch < 0x20) {
          std::ostringstream esc;
          esc << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
          out += esc.str();
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
}

static void AppendJsonIndent(const int depth, std::string& out) {
  for (int i = 0; i < depth; ++i) {
    out += "  ";
  }
}

static void AppendJsonValue(const JsonValue& value, std::string& out, const int depth) {
  switch (value.type) {
    case JsonValue::Type::Null:
      out += "null";
      return;
    case JsonValue::Type::Bool:
      out += value.bool_value ? "true" : "false";
      return;
    case JsonValue::Type::Number: {
      std::ostringstream number;
      number << std::setprecision(15) << value.number_value;
      out += number.str();
      return;
    }
    case JsonValue::Type::String:
      AppendJsonEscapedString(value.string_value, out);
      return;
    case JsonValue::Type::Array: {
      out += "[";
      if (!value.array_value.empty()) {
        out += "\n";
        for (std::size_t i = 0; i < value.array_value.size(); ++i) {
          AppendJsonIndent(depth + 1, out);
          AppendJsonValue(value.array_value[i], out, depth + 1);
          if (i + 1 < value.array_value.size()) {
            out += ",";
          }
          out += "\n";
        }
        AppendJsonIndent(depth, out);
      }
      out += "]";
      return;
    }
    case JsonValue::Type::Object: {
      out += "{";
      if (!value.object_value.empty()) {
        out += "\n";
        std::vector<std::string> keys;
        keys.reserve(value.object_value.size());
        for (const auto& pair : value.object_value) {
          keys.push_back(pair.first);
        }
        std::sort(keys.begin(), keys.end());
        for (std::size_t i = 0; i < keys.size(); ++i) {
          const auto it = value.object_value.find(keys[i]);
          if (it == value.object_value.end()) {
            continue;
          }
          AppendJsonIndent(depth + 1, out);
          AppendJsonEscapedString(it->first, out);
          out += ": ";
          AppendJsonValue(it->second, out, depth + 1);
          if (i + 1 < keys.size()) {
            out += ",";
          }
          out += "\n";
        }
        AppendJsonIndent(depth, out);
      }
      out += "}";
      return;
    }
  }
}

static std::string SerializeJson(const JsonValue& value) {
  std::string out;
  AppendJsonValue(value, out, 0);
  out.push_back('\n');
  return out;
}

static std::string JsonStringOrEmpty(const JsonValue* value) {
  if (value == nullptr || value->type != JsonValue::Type::String) {
    return "";
  }
  return value->string_value;
}

static std::string ExtractGeminiContentText(const JsonValue* value) {
  if (value == nullptr) {
    return "";
  }
  if (value->type == JsonValue::Type::String) {
    return value->string_value;
  }
  if (value->type == JsonValue::Type::Array) {
    std::ostringstream out;
    bool first = true;
    for (const JsonValue& item : value->array_value) {
      std::string piece;
      if (item.type == JsonValue::Type::String) {
        piece = item.string_value;
      } else if (item.type == JsonValue::Type::Object) {
        piece = JsonStringOrEmpty(item.Find("text"));
      }
      piece = Trim(piece);
      if (!piece.empty()) {
        if (!first) {
          out << "\n";
        }
        out << piece;
        first = false;
      }
    }
    return out.str();
  }
  if (value->type == JsonValue::Type::Object) {
    return JsonStringOrEmpty(value->Find("text"));
  }
  return "";
}

static std::string ExecuteCommandCaptureOutput(const std::string& command) {
  const std::string full_command = command + " 2>&1";
#if defined(_WIN32)
  FILE* pipe = _popen(full_command.c_str(), "r");
#else
  FILE* pipe = popen(full_command.c_str(), "r");
#endif
  if (pipe == nullptr) {
    std::ostringstream message;
    message << "Failed to launch Gemini CLI command";
    if (errno != 0) {
      message << " (" << std::strerror(errno) << ")";
    }
    message << ".";
    return message.str();
  }

  std::array<char, 4096> buffer{};
  std::string output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#if defined(_WIN32)
  const int close_code = _pclose(pipe);
#else
  const int close_code = pclose(pipe);
#endif
  int exit_code = close_code;
#if defined(__unix__) || defined(__APPLE__)
  if (WIFEXITED(close_code)) {
    exit_code = WEXITSTATUS(close_code);
  }
#endif

  if (output.empty()) {
    output = "(Gemini CLI returned no output.)";
  }
  if (exit_code != 0) {
    output += "\n\n[Gemini CLI exited with code " + std::to_string(exit_code) + "]";
  }

  return output;
}

static fs::path SettingsFilePath(const AppState& app) {
  return AppPaths::SettingsFilePath(app.data_root);
}

static fs::path ChatsRootPath(const AppState& app) {
  return AppPaths::ChatsRootPath(app.data_root);
}

static fs::path ChatPath(const AppState& app, const ChatSession& chat) {
  return AppPaths::ChatPath(app.data_root, chat.id);
}

static fs::path DefaultDataRootPath() {
  if (const char* home = std::getenv("HOME")) {
#if defined(__APPLE__)
    return fs::path(home) / "Library" / "Application Support" / "Universal Agent Manager";
#else
    return fs::path(home) / ".universal_agent_manager";
#endif
  }
  std::error_code ec;
  const fs::path temp = fs::temp_directory_path(ec);
  if (!ec) {
    return temp / "universal_agent_manager_data";
  }
  return fs::path("data");
}

static fs::path TempFallbackDataRootPath() {
  std::error_code ec;
  const fs::path temp = fs::temp_directory_path(ec);
  if (!ec) {
    return temp / "universal_agent_manager_data";
  }
  return fs::path("data");
}

static bool EnsureDataRootLayout(const fs::path& data_root, std::string* error_out) {
  std::error_code ec;
  fs::create_directories(data_root, ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create data root '" + data_root.string() + "': " + ec.message();
    }
    return false;
  }
  fs::create_directories(data_root / "chats", ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create chats dir '" + (data_root / "chats").string() + "': " + ec.message();
    }
    return false;
  }
  return true;
}

static std::optional<fs::path> ResolveGeminiProjectTmpDir(const fs::path& project_root) {
  return AppPaths::ResolveGeminiProjectTmpDir(project_root);
}

static std::optional<ChatSession> ParseGeminiSessionFile(const fs::path& file_path, const ProviderProfile& provider) {
  const std::string file_text = ReadTextFile(file_path);
  if (file_text.empty()) {
    return std::nullopt;
  }
  const std::optional<JsonValue> root_opt = ParseJson(file_text);
  if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object) {
    return std::nullopt;
  }
  const JsonValue& root = root_opt.value();
  const std::string session_id = JsonStringOrEmpty(root.Find("sessionId"));
  if (session_id.empty()) {
    return std::nullopt;
  }

  ChatSession chat;
  chat.id = session_id;
  chat.native_session_id = session_id;
  chat.uses_native_session = true;
  chat.created_at = JsonStringOrEmpty(root.Find("startTime"));
  chat.updated_at = JsonStringOrEmpty(root.Find("lastUpdated"));
  if (chat.created_at.empty()) {
    chat.created_at = TimestampNow();
  }
  if (chat.updated_at.empty()) {
    chat.updated_at = chat.created_at;
  }

  const JsonValue* messages = root.Find("messages");
  if (messages != nullptr && messages->type == JsonValue::Type::Array) {
    for (const JsonValue& raw_message : messages->array_value) {
      if (raw_message.type != JsonValue::Type::Object) {
        continue;
      }
      const std::string type = JsonStringOrEmpty(raw_message.Find("type"));
      const std::string timestamp = JsonStringOrEmpty(raw_message.Find("timestamp"));
      const std::string content = Trim(ExtractGeminiContentText(raw_message.Find("content")));
      if (content.empty()) {
        continue;
      }

      Message message;
      message.role = ProviderRuntime::RoleFromNativeType(provider, type);
      message.content = content;
      message.created_at = timestamp.empty() ? chat.updated_at : timestamp;
      chat.messages.push_back(std::move(message));
    }
  }

  chat.title = "Session " + chat.created_at;
  for (const Message& message : chat.messages) {
    if (message.role == MessageRole::User) {
      std::string title = Trim(message.content);
      if (title.size() > 48) {
        title = title.substr(0, 45) + "...";
      }
      if (!title.empty()) {
        chat.title = title;
      }
      break;
    }
  }
  return chat;
}

static std::vector<ChatSession> LoadNativeGeminiChats(const fs::path& chats_dir, const ProviderProfile& provider) {
  std::vector<ChatSession> chats;
  if (chats_dir.empty() || !fs::exists(chats_dir) || !fs::is_directory(chats_dir)) {
    return chats;
  }

  std::error_code ec;
  for (const auto& item : fs::directory_iterator(chats_dir, ec)) {
    if (ec || !item.is_regular_file() || item.path().extension() != ".json") {
      continue;
    }
    const auto parsed = ParseGeminiSessionFile(item.path(), provider);
    if (parsed.has_value()) {
      chats.push_back(parsed.value());
    }
  }

  SortChatsByRecent(chats);
  return chats;
}

static std::vector<std::string> SessionIdsFromChats(const std::vector<ChatSession>& chats) {
  std::vector<std::string> ids;
  ids.reserve(chats.size());
  for (const ChatSession& chat : chats) {
    if (chat.uses_native_session && !chat.native_session_id.empty()) {
      ids.push_back(chat.native_session_id);
    }
  }
  return ids;
}

static std::string NewFolderId() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::ostringstream id;
  id << "folder-" << epoch_ms;
  return id.str();
}

static int FindFolderIndexById(const AppState& app, const std::string& folder_id) {
  for (int i = 0; i < static_cast<int>(app.folders.size()); ++i) {
    if (app.folders[i].id == folder_id) {
      return i;
    }
  }
  return -1;
}

static ChatFolder* FindFolderById(AppState& app, const std::string& folder_id) {
  const int idx = FindFolderIndexById(app, folder_id);
  return (idx >= 0) ? &app.folders[idx] : nullptr;
}

static const ChatFolder* FindFolderById(const AppState& app, const std::string& folder_id) {
  const int idx = FindFolderIndexById(app, folder_id);
  return (idx >= 0) ? &app.folders[idx] : nullptr;
}

static int CountChatsInFolder(const AppState& app, const std::string& folder_id);

static void EnsureDefaultFolder(AppState& app) {
  if (FindFolderIndexById(app, kDefaultFolderId) >= 0) {
    return;
  }
  ChatFolder folder;
  folder.id = kDefaultFolderId;
  folder.title = kDefaultFolderTitle;
  folder.directory = fs::current_path().string();
  folder.collapsed = false;
  app.folders.push_back(std::move(folder));
}

static void EnsureNewChatFolderSelection(AppState& app) {
  EnsureDefaultFolder(app);
  if (app.new_chat_folder_id.empty() || FindFolderById(app, app.new_chat_folder_id) == nullptr) {
    app.new_chat_folder_id = kDefaultFolderId;
  }
}

static void NormalizeChatFolderAssignments(AppState& app) {
  EnsureDefaultFolder(app);
  for (ChatSession& chat : app.chats) {
    if (chat.folder_id.empty() || FindFolderById(app, chat.folder_id) == nullptr) {
      chat.folder_id = kDefaultFolderId;
    }
  }
  bool any_expanded_with_chats = false;
  for (const ChatFolder& folder : app.folders) {
    if (!folder.collapsed && CountChatsInFolder(app, folder.id) > 0) {
      any_expanded_with_chats = true;
      break;
    }
  }
  if (!any_expanded_with_chats) {
    for (ChatFolder& folder : app.folders) {
      if (CountChatsInFolder(app, folder.id) > 0) {
        folder.collapsed = false;
      }
    }
  }
  EnsureNewChatFolderSelection(app);
}

static void SaveFolders(const AppState& app) {
  ChatFolderStore::Save(app.data_root, app.folders);
}

static void SaveProviders(const AppState& app) {
  ProviderProfileStore::Save(app.data_root, app.provider_profiles);
}

static fs::path ProviderProfileFilePath(const AppState& app) {
  return app.data_root / "providers.txt";
}

static fs::path FrontendActionFilePath(const AppState& app) {
  return app.data_root / "frontend_actions.txt";
}

static ProviderProfile* ActiveProvider(AppState& app) {
  ProviderProfile* found = ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
  if (found != nullptr) {
    return found;
  }
  ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
  app.settings.active_provider_id = "gemini";
  return ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
}

static const ProviderProfile* ActiveProvider(const AppState& app) {
  return ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
}

static const ProviderProfile& ActiveProviderOrDefault(const AppState& app) {
  const ProviderProfile* profile = ActiveProvider(app);
  if (profile != nullptr) {
    return *profile;
  }
  static const ProviderProfile fallback = ProviderProfileStore::DefaultGeminiProfile();
  return fallback;
}

static bool ActiveProviderUsesGeminiHistory(const AppState& app) {
  const ProviderProfile* profile = ActiveProvider(app);
  return profile != nullptr && ProviderRuntime::SupportsGeminiJsonHistory(*profile);
}

static const uam::FrontendAction* FindFrontendAction(const AppState& app, const std::string& key) {
  return uam::FindAction(app.frontend_actions, key);
}

static bool FrontendActionVisible(const AppState& app, const std::string& key, const bool fallback_visible = true) {
  const uam::FrontendAction* action = FindFrontendAction(app, key);
  return (action == nullptr) ? fallback_visible : action->visible;
}

static std::string FrontendActionLabel(const AppState& app, const std::string& key, const std::string& fallback_label) {
  const uam::FrontendAction* action = FindFrontendAction(app, key);
  if (action == nullptr || Trim(action->label).empty()) {
    return fallback_label;
  }
  return action->label;
}

static void LoadFrontendActions(AppState& app) {
  std::string error;
  if (!uam::LoadFrontendActionMap(FrontendActionFilePath(app), app.frontend_actions, &error)) {
    app.frontend_actions = uam::DefaultFrontendActionMap();
    if (!uam::SaveFrontendActionMap(FrontendActionFilePath(app), app.frontend_actions, &error) && !error.empty()) {
      app.status_line = "Frontend action map reset, but saving failed: " + error;
    } else if (!error.empty()) {
      app.status_line = "Frontend action map was invalid and has been reset.";
    }
    return;
  }
  uam::NormalizeFrontendActionMap(app.frontend_actions);
}

static std::string FolderForNewChat(const AppState& app) {
  if (!app.new_chat_folder_id.empty()) {
    return app.new_chat_folder_id;
  }
  return kDefaultFolderId;
}

static int CountChatsInFolder(const AppState& app, const std::string& folder_id) {
  int count = 0;
  for (const ChatSession& chat : app.chats) {
    if (chat.folder_id == folder_id) {
      ++count;
    }
  }
  return count;
}

static std::string FolderTitleOrFallback(const ChatFolder& folder) {
  const std::string trimmed = Trim(folder.title);
  return trimmed.empty() ? "Untitled Folder" : trimmed;
}

static int FindChatIndexById(const AppState& app, const std::string& chat_id) {
  for (int i = 0; i < static_cast<int>(app.chats.size()); ++i) {
    if (app.chats[i].id == chat_id) {
      return i;
    }
  }
  return -1;
}

static ChatSession* SelectedChat(AppState& app) {
  if (app.selected_chat_index < 0 || app.selected_chat_index >= static_cast<int>(app.chats.size())) {
    return nullptr;
  }
  return &app.chats[app.selected_chat_index];
}

static void SortChatsByRecent(std::vector<ChatSession>& chats) {
  std::sort(chats.begin(), chats.end(), [](const ChatSession& a, const ChatSession& b) {
    return a.updated_at > b.updated_at;
  });
}

static void SaveSettings(const AppState& app) {
  SettingsStore::Save(SettingsFilePath(app), app.settings, app.center_view_mode);
}

static void LoadSettings(AppState& app) {
  SettingsStore::Load(SettingsFilePath(app), app.settings, app.center_view_mode);
}

static void ToggleYoloMode(AppState& app, const char* trigger_label) {
  app.settings.gemini_yolo_mode = !app.settings.gemini_yolo_mode;
  SaveSettings(app);
  app.status_line = std::string("YOLO mode ") +
                    (app.settings.gemini_yolo_mode ? "enabled" : "disabled") +
                    " (" + trigger_label + ").";
}

static void HandleGlobalHotkeys(AppState& app) {
  // Structured mode has no approve/disapprove controls, so provide keyboard
  // toggles for YOLO directly from the center pane.
  if (app.center_view_mode != CenterViewMode::Structured) {
    return;
  }

  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || ImGui::IsAnyItemActive()) {
    return;
  }

  const bool shift_tab =
      ImGui::IsKeyPressed(ImGuiKey_Tab, false) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt;
  const bool ctrl_y =
      ImGui::IsKeyPressed(ImGuiKey_Y, false) && io.KeyCtrl && !io.KeyAlt;

  if (shift_tab) {
    ToggleYoloMode(app, "Shift+Tab");
  } else if (ctrl_y) {
    ToggleYoloMode(app, "Ctrl+Y");
  }
}

static bool SaveChat(const AppState& app, const ChatSession& chat) {
  return ChatRepository::SaveChat(app.data_root, chat);
}

static std::vector<ChatSession> LoadChats(const AppState& app) {
  return ChatRepository::LoadLocalChats(app.data_root);
}

static ChatSession CreateNewChat(const std::string& folder_id) {
  ChatSession chat;
  chat.id = NewSessionId();
  chat.folder_id = folder_id;
  chat.created_at = TimestampNow();
  chat.updated_at = chat.created_at;
  chat.title = "Chat " + chat.created_at;
  return chat;
}

static void SelectChatById(AppState& app, const std::string& chat_id) {
  app.selected_chat_index = FindChatIndexById(app, chat_id);
}

static void AddMessage(ChatSession& chat, const MessageRole role, const std::string& text) {
  Message message;
  message.role = role;
  message.content = text;
  message.created_at = TimestampNow();
  chat.messages.push_back(std::move(message));
  chat.updated_at = TimestampNow();

  if (chat.messages.size() == 1 && role == MessageRole::User) {
    std::string maybe_title = Trim(text);
    if (maybe_title.size() > 48) {
      maybe_title = maybe_title.substr(0, 45) + "...";
    }
    if (!maybe_title.empty()) {
      chat.title = maybe_title;
    }
  }
}

static std::string BuildProviderPrompt(const ProviderProfile& provider,
                                       const std::string& user_prompt,
                                       const std::vector<std::string>& files) {
  return ProviderRuntime::BuildPrompt(provider, user_prompt, files);
}

static std::string BuildProviderCommand(
    const ProviderProfile& provider,
    const AppSettings& settings,
    const std::string& prompt,
    const std::vector<std::string>& files,
    const std::string& resume_session_id) {
  return ProviderRuntime::BuildCommand(provider, settings, prompt, files, resume_session_id);
}

static void SaveAndUpdateStatus(AppState& app, const ChatSession& chat, const std::string& success, const std::string& failure) {
  if (SaveChat(app, chat)) {
    app.status_line = success;
  } else {
    app.status_line = failure;
  }
}

static void ApplyLocalOverrides(AppState& app, std::vector<ChatSession>& native_chats) {
  std::vector<ChatSession> local_chats = LoadChats(app);
  app.chats = uam::MergeNativeAndLocalChats(std::move(native_chats), local_chats);
  NormalizeChatFolderAssignments(app);
}

static void RefreshGeminiChatsDir(AppState& app) {
  const auto tmp_dir = ResolveGeminiProjectTmpDir(fs::current_path());
  if (tmp_dir.has_value()) {
    app.gemini_chats_dir = tmp_dir.value() / "chats";
    std::error_code ec;
    fs::create_directories(app.gemini_chats_dir, ec);
  } else {
    app.gemini_chats_dir.clear();
  }
}

static std::string PickNewSessionId(
    const std::vector<ChatSession>& loaded_chats,
    const std::vector<std::string>& existing_ids) {
  std::unordered_set<std::string> seen(existing_ids.begin(), existing_ids.end());
  for (const ChatSession& chat : loaded_chats) {
    if (!chat.native_session_id.empty() && seen.find(chat.native_session_id) == seen.end()) {
      return chat.native_session_id;
    }
  }
  return loaded_chats.empty() ? "" : loaded_chats.front().native_session_id;
}

static std::optional<fs::path> FindNativeSessionFilePath(const AppState& app, const std::string& session_id) {
  if (session_id.empty() || app.gemini_chats_dir.empty() || !fs::exists(app.gemini_chats_dir)) {
    return std::nullopt;
  }
  std::error_code ec;
  for (const auto& item : fs::directory_iterator(app.gemini_chats_dir, ec)) {
    if (ec || !item.is_regular_file() || item.path().extension() != ".json") {
      continue;
    }
    const std::string file_text = ReadTextFile(item.path());
    if (file_text.empty()) {
      continue;
    }
    const std::optional<JsonValue> root_opt = ParseJson(file_text);
    if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object) {
      continue;
    }
    if (JsonStringOrEmpty(root_opt->Find("sessionId")) == session_id) {
      return item.path();
    }
  }
  return std::nullopt;
}

static MessageRole NativeMessageRoleFromType(const ProviderProfile& provider, const std::string& type) {
  return ProviderRuntime::RoleFromNativeType(provider, type);
}

static bool TruncateNativeSessionFromDisplayedMessage(const AppState& app,
                                                      const ChatSession& chat,
                                                      const int displayed_message_index,
                                                      std::string* error_out) {
  if (!chat.uses_native_session || chat.native_session_id.empty()) {
    if (error_out != nullptr) {
      *error_out = "Chat is not linked to a native Gemini session.";
    }
    return false;
  }

  const auto session_file = FindNativeSessionFilePath(app, chat.native_session_id);
  if (!session_file.has_value()) {
    if (error_out != nullptr) {
      *error_out = "Could not locate native Gemini session file.";
    }
    return false;
  }

  const std::string file_text = ReadTextFile(session_file.value());
  if (file_text.empty()) {
    if (error_out != nullptr) {
      *error_out = "Native Gemini session file is empty.";
    }
    return false;
  }
  const std::optional<JsonValue> root_opt = ParseJson(file_text);
  if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object) {
    if (error_out != nullptr) {
      *error_out = "Failed to parse native Gemini session JSON.";
    }
    return false;
  }
  JsonValue root = root_opt.value();

  auto messages_it = root.object_value.find("messages");
  if (messages_it == root.object_value.end() || messages_it->second.type != JsonValue::Type::Array) {
    if (error_out != nullptr) {
      *error_out = "Native Gemini session has no messages array.";
    }
    return false;
  }
  JsonValue& messages_array = messages_it->second;
  const ProviderProfile& provider = ActiveProviderOrDefault(app);

  std::vector<int> visible_raw_indices;
  std::vector<MessageRole> visible_roles;
  visible_raw_indices.reserve(messages_array.array_value.size());
  visible_roles.reserve(messages_array.array_value.size());
  for (int i = 0; i < static_cast<int>(messages_array.array_value.size()); ++i) {
    const JsonValue& raw_message = messages_array.array_value[i];
    if (raw_message.type != JsonValue::Type::Object) {
      continue;
    }
    const std::string content = Trim(ExtractGeminiContentText(raw_message.Find("content")));
    if (content.empty()) {
      continue;
    }
    visible_raw_indices.push_back(i);
    visible_roles.push_back(NativeMessageRoleFromType(provider, JsonStringOrEmpty(raw_message.Find("type"))));
  }

  if (displayed_message_index < 0 || displayed_message_index >= static_cast<int>(visible_raw_indices.size())) {
    if (error_out != nullptr) {
      *error_out = "Selected message no longer matches native session timeline.";
    }
    return false;
  }
  if (visible_roles[displayed_message_index] != MessageRole::User) {
    if (error_out != nullptr) {
      *error_out = "Only user messages can be edited.";
    }
    return false;
  }

  const int raw_cut_index = visible_raw_indices[displayed_message_index];
  messages_array.array_value.erase(messages_array.array_value.begin() + raw_cut_index, messages_array.array_value.end());

  if (!WriteTextFile(session_file.value(), SerializeJson(root))) {
    if (error_out != nullptr) {
      *error_out = "Failed to write updated native Gemini session.";
    }
    return false;
  }
  return true;
}

static void FreeCliTerminalVTerm(CliTerminalState& terminal) {
  if (terminal.vt != nullptr) {
    vterm_free(terminal.vt);
    terminal.vt = nullptr;
    terminal.screen = nullptr;
    terminal.state = nullptr;
  }
}

#if defined(_WIN32)
static void StartCliTerminalIoWorker(CliTerminalState& terminal) {
  auto io_state = std::make_shared<CliTerminalIoState>();
  io_state->pipe_input = terminal.pipe_input;
  io_state->pipe_output = terminal.pipe_output;
  terminal.io_state = io_state;

  std::thread([io_state]() {
    std::array<char, 8192> buffer{};
    while (!io_state->stop_requested.load(std::memory_order_acquire)) {
      bool did_work = false;

      while (!io_state->stop_requested.load(std::memory_order_acquire)) {
        std::string chunk;
        {
          std::lock_guard<std::mutex> lock(io_state->mutex);
          if (io_state->pending_input.empty()) {
            break;
          }
          chunk = std::move(io_state->pending_input.front());
          io_state->pending_input.pop_front();
        }
        if (chunk.empty()) {
          continue;
        }

        DWORD written = 0;
        if (!WriteFile(io_state->pipe_input, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr) ||
            written != chunk.size()) {
          io_state->write_failed.store(true, std::memory_order_release);
          io_state->stop_requested.store(true, std::memory_order_release);
          break;
        }
        did_work = true;
      }

      if (io_state->stop_requested.load(std::memory_order_acquire)) {
        break;
      }

      DWORD available = 0;
      if (!PeekNamedPipe(io_state->pipe_output, nullptr, 0, nullptr, &available, nullptr)) {
        const DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_INVALID_HANDLE) {
          io_state->output_closed.store(true, std::memory_order_release);
        } else {
          io_state->read_failed.store(true, std::memory_order_release);
        }
        break;
      }

      if (available > 0) {
        const DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));
        DWORD bytes_read = 0;
        if (!ReadFile(io_state->pipe_output, buffer.data(), to_read, &bytes_read, nullptr)) {
          const DWORD err = GetLastError();
          if (err == ERROR_BROKEN_PIPE || err == ERROR_INVALID_HANDLE) {
            io_state->output_closed.store(true, std::memory_order_release);
          } else {
            io_state->read_failed.store(true, std::memory_order_release);
          }
          break;
        }
        if (bytes_read > 0) {
          std::lock_guard<std::mutex> lock(io_state->mutex);
          io_state->pending_output.emplace_back(buffer.data(), buffer.data() + bytes_read);
          did_work = true;
        }
      }

      if (!did_work) {
        Sleep(10);
      }
    }
  }).detach();
}
#endif

static void CloseCliTerminalHandles(CliTerminalState& terminal) {
#if defined(_WIN32)
  if (terminal.io_state != nullptr) {
    terminal.io_state->stop_requested.store(true, std::memory_order_release);
  }
  if (terminal.pipe_input != INVALID_HANDLE_VALUE) {
    CloseHandle(terminal.pipe_input);
    terminal.pipe_input = INVALID_HANDLE_VALUE;
  }
  if (terminal.pipe_output != INVALID_HANDLE_VALUE) {
    CloseHandle(terminal.pipe_output);
    terminal.pipe_output = INVALID_HANDLE_VALUE;
  }
  if (terminal.pseudo_console != nullptr) {
    ClosePseudoConsoleSafe(terminal.pseudo_console);
    terminal.pseudo_console = nullptr;
  }
  if (terminal.attr_list != nullptr) {
    DeleteProcThreadAttributeList(terminal.attr_list);
    HeapFree(GetProcessHeap(), 0, terminal.attr_list);
    terminal.attr_list = nullptr;
  }
  if (terminal.process_info.hThread != INVALID_HANDLE_VALUE) {
    CloseHandle(terminal.process_info.hThread);
    terminal.process_info.hThread = INVALID_HANDLE_VALUE;
  }
  if (terminal.process_info.hProcess != INVALID_HANDLE_VALUE) {
    CloseHandle(terminal.process_info.hProcess);
    terminal.process_info.hProcess = INVALID_HANDLE_VALUE;
  }
  terminal.process_info.dwProcessId = 0;
  terminal.process_info.dwThreadId = 0;
  terminal.io_state.reset();
#else
  if (terminal.master_fd >= 0) {
    close(terminal.master_fd);
    terminal.master_fd = -1;
  }
  terminal.child_pid = -1;
#endif
}

static bool WriteToCliTerminal(CliTerminalState& terminal, const char* bytes, const size_t len) {
  if (bytes == nullptr || len == 0) {
    return true;
  }
#if defined(_WIN32)
  if (terminal.io_state == nullptr || terminal.io_state->stop_requested.load(std::memory_order_acquire)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(terminal.io_state->mutex);
    terminal.io_state->pending_input.emplace_back(bytes, bytes + len);
  }
  return true;
#else
  std::size_t offset = 0;
  while (offset < len) {
    const ssize_t written = write(terminal.master_fd, bytes + offset, len - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
#endif
}

static void WriteBytesToPty(const char* bytes, const size_t len, void* user) {
  if (user == nullptr || bytes == nullptr || len == 0) {
    return;
  }
  auto* terminal = static_cast<CliTerminalState*>(user);
  if (!WriteToCliTerminal(*terminal, bytes, len)) {
    terminal->last_error = "Failed to write to Gemini terminal.";
  }
}

static int OnVTermDamage(VTermRect, void* user) {
  if (user != nullptr) {
    static_cast<CliTerminalState*>(user)->needs_full_refresh = true;
  }
  return 1;
}

static int OnVTermMoveRect(VTermRect, VTermRect, void* user) {
  if (user != nullptr) {
    static_cast<CliTerminalState*>(user)->needs_full_refresh = true;
  }
  return 1;
}

static int OnVTermMoveCursor(VTermPos, VTermPos, int, void* user) {
  if (user != nullptr) {
    static_cast<CliTerminalState*>(user)->needs_full_refresh = true;
  }
  return 1;
}

static int OnVTermResize(int rows, int cols, void* user) {
  if (user != nullptr) {
    auto* terminal = static_cast<CliTerminalState*>(user);
    terminal->rows = rows;
    terminal->cols = cols;
    terminal->needs_full_refresh = true;
  }
  return 1;
}

static const VTermScreenCallbacks kVTermScreenCallbacks = {
    OnVTermDamage,
    OnVTermMoveRect,
    OnVTermMoveCursor,
    nullptr,
    nullptr,
    OnVTermResize,
    nullptr,
    nullptr,
    nullptr,
    nullptr};

static void StopCliTerminal(CliTerminalState& terminal, const bool clear_identity = false) {
#if defined(_WIN32)
  if (terminal.process_info.hProcess != INVALID_HANDLE_VALUE) {
    TerminateProcess(terminal.process_info.hProcess, 1);
    WaitForSingleObject(terminal.process_info.hProcess, 0);
  }
#else
  if (terminal.child_pid > 0) {
    kill(terminal.child_pid, SIGHUP);
    int status = 0;
    waitpid(terminal.child_pid, &status, WNOHANG);
    terminal.child_pid = -1;
  }
#endif

  CloseCliTerminalHandles(terminal);
  FreeCliTerminalVTerm(terminal);
  terminal.running = false;
  terminal.needs_full_refresh = true;
  terminal.last_background_poll_time_s = 0.0;
  terminal.last_text_cache_refresh_s = 0.0;
  terminal.rendered_rows.clear();
  if (clear_identity) {
    terminal.attached_chat_id.clear();
    terminal.attached_session_id.clear();
    terminal.session_ids_before.clear();
    terminal.linked_files_snapshot.clear();
    terminal.should_launch = false;
  }
}

static CliTerminalState* FindCliTerminalForChat(AppState& app, const std::string& chat_id) {
  for (auto& terminal : app.cli_terminals) {
    if (terminal != nullptr && terminal->attached_chat_id == chat_id) {
      return terminal.get();
    }
  }
  return nullptr;
}

static CliTerminalState& EnsureCliTerminalForChat(AppState& app, const ChatSession& chat) {
  if (CliTerminalState* existing = FindCliTerminalForChat(app, chat.id)) {
    if (existing->attached_session_id.empty() && chat.uses_native_session) {
      existing->attached_session_id = chat.native_session_id;
    }
    return *existing;
  }
  auto terminal = std::make_unique<CliTerminalState>();
  terminal->attached_chat_id = chat.id;
  terminal->attached_session_id = chat.uses_native_session ? chat.native_session_id : "";
  terminal->should_launch = (app.center_view_mode == CenterViewMode::CliConsole);
  app.cli_terminals.push_back(std::move(terminal));
  return *app.cli_terminals.back();
}

static void StopAndEraseCliTerminalForChat(AppState& app, const std::string& chat_id) {
  app.cli_terminals.erase(
      std::remove_if(app.cli_terminals.begin(), app.cli_terminals.end(),
                     [&](std::unique_ptr<CliTerminalState>& terminal) {
                       if (terminal == nullptr || terminal->attached_chat_id != chat_id) {
                         return false;
                       }
                       StopCliTerminal(*terminal, true);
                       return true;
                     }),
      app.cli_terminals.end());
}

static void StopAllCliTerminals(AppState& app, const bool clear_identity = true) {
  for (auto& terminal : app.cli_terminals) {
    if (terminal != nullptr) {
      StopCliTerminal(*terminal, clear_identity);
    }
  }
}

static void StopCliTerminalsExcept(AppState& app, const std::string& chat_id) {
  for (auto& terminal : app.cli_terminals) {
    if (terminal == nullptr || terminal->attached_chat_id == chat_id) {
      continue;
    }
    if (terminal->running) {
      StopCliTerminal(*terminal, false);
    }
    terminal->should_launch = false;
  }
}

static void MarkSelectedCliTerminalForLaunch(AppState& app) {
  ChatSession* selected = SelectedChat(app);
  if (selected == nullptr) {
    return;
  }
  CliTerminalState& terminal = EnsureCliTerminalForChat(app, *selected);
  StopCliTerminalsExcept(app, selected->id);
  terminal.should_launch = true;
}

static void SyncChatsFromNative(AppState& app, const std::string& preferred_chat_id, const bool preserve_selection = false) {
  const std::string selected_before = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
  const std::string previous_selected = !selected_before.empty() ? selected_before : preferred_chat_id;

  if (ActiveProviderUsesGeminiHistory(app)) {
    RefreshGeminiChatsDir(app);
    std::vector<ChatSession> native = LoadNativeGeminiChats(app.gemini_chats_dir, ActiveProviderOrDefault(app));
    ApplyLocalOverrides(app, native);
  } else {
    app.chats = LoadChats(app);
    NormalizeChatFolderAssignments(app);
  }

  if (preserve_selection && !selected_before.empty() && FindChatIndexById(app, selected_before) >= 0) {
    SelectChatById(app, selected_before);
  } else if (!preferred_chat_id.empty() && FindChatIndexById(app, preferred_chat_id) >= 0) {
    SelectChatById(app, preferred_chat_id);
  } else if (!previous_selected.empty() && FindChatIndexById(app, previous_selected) >= 0) {
    SelectChatById(app, previous_selected);
  } else if (!app.chats.empty()) {
    app.selected_chat_index = 0;
  } else {
    app.selected_chat_index = -1;
  }
}

static std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat) {
  return ProviderRuntime::BuildInteractiveArgv(ActiveProviderOrDefault(app), chat, app.settings);
}

#if defined(__unix__)
static bool SetFdNonBlocking(const int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

static bool StartCliTerminalForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, const int rows, const int cols) {
  StopCliTerminal(terminal);

  terminal.rows = ClampTerminalRows(rows);
  terminal.cols = ClampTerminalCols(cols);
  terminal.attached_chat_id = chat.id;
  terminal.attached_session_id = chat.uses_native_session ? chat.native_session_id : "";
  terminal.linked_files_snapshot = chat.linked_files;
  if (ActiveProviderUsesGeminiHistory(app)) {
    // Reuse the in-memory chat snapshot to avoid a blocking native history scan
    // on the UI thread during terminal startup.
    terminal.session_ids_before = SessionIdsFromChats(app.chats);
  } else {
    terminal.session_ids_before.clear();
  }
  terminal.last_error.clear();
  terminal.last_sync_time_s = ImGui::GetTime();
  terminal.last_text_cache_refresh_s = 0.0;
  terminal.rendered_rows.clear();

  terminal.vt = vterm_new(terminal.rows, terminal.cols);
  if (terminal.vt == nullptr) {
    terminal.last_error = "Failed to initialize libvterm.";
    StopCliTerminal(terminal, false);
    return false;
  }
  vterm_set_utf8(terminal.vt, 1);
  terminal.screen = vterm_obtain_screen(terminal.vt);
  terminal.state = vterm_obtain_state(terminal.vt);
  vterm_screen_set_callbacks(terminal.screen, &kVTermScreenCallbacks, &terminal);
  vterm_screen_set_damage_merge(terminal.screen, VTERM_DAMAGE_SCREEN);
  vterm_output_set_callback(terminal.vt, WriteBytesToPty, &terminal);
  vterm_screen_reset(terminal.screen, 1);

#if defined(_WIN32)
  if (!StartCliTerminalWindows(app, terminal, chat)) {
    terminal.last_error = terminal.last_error.empty() ? "Failed to start Gemini terminal." : terminal.last_error;
    StopCliTerminal(terminal, false);
    return false;
  }
#else
  if (!StartCliTerminalUnix(app, terminal, chat)) {
    terminal.last_error = terminal.last_error.empty() ? "Failed to start Gemini terminal." : terminal.last_error;
    StopCliTerminal(terminal, false);
    return false;
  }
#endif

  terminal.running = true;
  terminal.should_launch = false;
  terminal.needs_full_refresh = true;
  terminal.last_background_poll_time_s = 0.0;
  return true;
}

#if defined(__unix__)
static bool StartCliTerminalUnix(AppState& app, CliTerminalState& terminal, const ChatSession& chat) {
  int master_fd = -1;
  int slave_fd = -1;
  struct winsize ws {};
  ws.ws_row = static_cast<unsigned short>(terminal.rows);
  ws.ws_col = static_cast<unsigned short>(terminal.cols);

  if (openpty(&master_fd, &slave_fd, nullptr, nullptr, &ws) != 0) {
    terminal.last_error = "openpty failed.";
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    terminal.last_error = "fork failed.";
    close(master_fd);
    close(slave_fd);
    return false;
  }

  if (pid == 0) {
    setsid();
    ioctl(slave_fd, TIOCSCTTY, 0);
    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);
    close(master_fd);
    close(slave_fd);

    setenv("TERM", "xterm-256color", 1);
    const std::vector<std::string> argv_vec = BuildProviderInteractiveArgv(app, chat);
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_vec.size() + 1);
    for (const std::string& arg : argv_vec) {
      argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
    }
    argv_ptrs.push_back(nullptr);
    execvp(argv_ptrs[0], argv_ptrs.data());
    _exit(127);
  }

  close(slave_fd);
  terminal.master_fd = master_fd;
  terminal.child_pid = pid;
  SetFdNonBlocking(terminal.master_fd);
  return true;
}
#endif

#if defined(_WIN32)
using CreatePseudoConsoleFunc = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ResizePseudoConsoleFunc = HRESULT(WINAPI*)(HPCON, COORD);
using ClosePseudoConsoleFunc = void(WINAPI*)(HPCON);

static CreatePseudoConsoleFunc GetCreatePseudoConsoleFunc() {
  static CreatePseudoConsoleFunc func = reinterpret_cast<CreatePseudoConsoleFunc>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreatePseudoConsole"));
  return func;
}

static ResizePseudoConsoleFunc GetResizePseudoConsoleFunc() {
  static ResizePseudoConsoleFunc func = reinterpret_cast<ResizePseudoConsoleFunc>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ResizePseudoConsole"));
  return func;
}

static ClosePseudoConsoleFunc GetClosePseudoConsoleFunc() {
  static ClosePseudoConsoleFunc func = reinterpret_cast<ClosePseudoConsoleFunc>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole"));
  return func;
}

static void ClosePseudoConsoleSafe(HPCON handle) {
  const auto close_proc = GetClosePseudoConsoleFunc();
  if (close_proc != nullptr && handle != nullptr) {
    close_proc(handle);
  }
}

static void ResizePseudoConsoleSafe(HPCON handle, COORD size) {
  const auto resize_proc = GetResizePseudoConsoleFunc();
  if (resize_proc != nullptr && handle != nullptr) {
    resize_proc(handle, size);
  }
}

static std::string QuoteWindowsArg(const std::string& arg) {
  if (arg.empty()) {
    return "\"\"";
  }
  const bool needs_quotes = (arg.find_first_of(" \t\"") != std::string::npos);
  if (!needs_quotes) {
    return arg;
  }
  std::string result = "\"";
  int backslashes = 0;
  for (char ch : arg) {
    if (ch == '\\') {
      backslashes++;
    } else if (ch == '"') {
      result.append(backslashes * 2 + 1, '\\');
      result.push_back('"');
      backslashes = 0;
    } else {
      if (backslashes > 0) {
        result.append(backslashes, '\\');
        backslashes = 0;
      }
      result.push_back(ch);
    }
  }
  if (backslashes > 0) {
    result.append(backslashes * 2, '\\');
  }
  result.push_back('"');
  return result;
}

static std::string BuildWindowsCommandLine(const std::vector<std::string>& argv) {
  std::ostringstream out;
  bool first = true;
  for (const std::string& arg : argv) {
    if (!first) {
      out << ' ';
    }
    out << QuoteWindowsArg(arg);
    first = false;
  }
  return out.str();
}

static bool StartCliTerminalWindows(AppState& app, CliTerminalState& terminal, const ChatSession& chat) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE pipe_pty_in = INVALID_HANDLE_VALUE;
  HANDLE pipe_pty_out = INVALID_HANDLE_VALUE;
  HANDLE pipe_con_in = INVALID_HANDLE_VALUE;
  HANDLE pipe_con_out = INVALID_HANDLE_VALUE;

  if (!CreatePipe(&pipe_pty_in, &pipe_con_out, &sa, 0) ||
      !CreatePipe(&pipe_con_in, &pipe_pty_out, &sa, 0)) {
    terminal.last_error = "Failed to create ConPTY pipes.";
    CloseHandle(pipe_pty_in);
    CloseHandle(pipe_pty_out);
    CloseHandle(pipe_con_in);
    CloseHandle(pipe_con_out);
    return false;
  }

  const COORD size{static_cast<SHORT>(terminal.cols), static_cast<SHORT>(terminal.rows)};
  HPCON pseudo_console = nullptr;
  const auto create_pseudo_console = GetCreatePseudoConsoleFunc();
  if (create_pseudo_console == nullptr ||
      create_pseudo_console(size, pipe_con_in, pipe_con_out, 0, &pseudo_console) != S_OK) {
    terminal.last_error = "CreatePseudoConsole failed.";
    CloseHandle(pipe_pty_in);
    CloseHandle(pipe_pty_out);
    CloseHandle(pipe_con_in);
    CloseHandle(pipe_con_out);
    return false;
  }
  CloseHandle(pipe_con_in);
  CloseHandle(pipe_con_out);

  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  terminal.attr_list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attr_size));
  if (terminal.attr_list == nullptr ||
      !InitializeProcThreadAttributeList(terminal.attr_list, 1, 0, &attr_size)) {
    terminal.last_error = "Failed to initialize attribute list.";
    ClosePseudoConsoleSafe(pseudo_console);
    CloseHandle(pipe_pty_in);
    CloseHandle(pipe_pty_out);
    return false;
  }

  if (!UpdateProcThreadAttribute(terminal.attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                 pseudo_console, sizeof(HPCON), nullptr, nullptr)) {
    terminal.last_error = "Failed to attach pseudo console.";
    DeleteProcThreadAttributeList(terminal.attr_list);
    HeapFree(GetProcessHeap(), 0, terminal.attr_list);
    terminal.attr_list = nullptr;
    ClosePseudoConsoleSafe(pseudo_console);
    CloseHandle(pipe_pty_in);
    CloseHandle(pipe_pty_out);
    return false;
  }

  STARTUPINFOEX si{};
  si.StartupInfo.cb = sizeof(si);
  si.lpAttributeList = terminal.attr_list;
  PROCESS_INFORMATION pi{};

  const std::vector<std::string> argv_vec = BuildProviderInteractiveArgv(app, chat);
  const std::string gemini_invocation = BuildWindowsCommandLine(argv_vec);
  const char* comspec_env = std::getenv("ComSpec");
  const std::string comspec = (comspec_env != nullptr && *comspec_env != '\0') ? comspec_env : "C:\\Windows\\System32\\cmd.exe";
  std::string command_line = QuoteWindowsArg(comspec) + " /d /s /c \"\"chcp 65001>nul && " + gemini_invocation + "\"\"";
  std::vector<char> cmd_mutable(command_line.begin(), command_line.end());
  cmd_mutable.push_back(0);

  if (!CreateProcessA(comspec.c_str(),
                      cmd_mutable.data(),
                      nullptr,
                      nullptr,
                      FALSE,
                      EXTENDED_STARTUPINFO_PRESENT,
                      nullptr,
                      nullptr,
                      &si.StartupInfo,
                      &pi)) {
    terminal.last_error = "CreateProcess for Gemini failed (Win32 error " + std::to_string(GetLastError()) + ").";
    DeleteProcThreadAttributeList(terminal.attr_list);
    HeapFree(GetProcessHeap(), 0, terminal.attr_list);
    terminal.attr_list = nullptr;
    ClosePseudoConsoleSafe(pseudo_console);
    CloseHandle(pipe_pty_in);
    CloseHandle(pipe_pty_out);
    return false;
  }

  terminal.pipe_input = pipe_pty_out;
  terminal.pipe_output = pipe_pty_in;
  terminal.process_info = pi;
  terminal.pseudo_console = pseudo_console;
  StartCliTerminalIoWorker(terminal);
  return true;
}
#endif

static void RefreshCliTerminalTextCache(CliTerminalState& terminal) {
  if (terminal.screen == nullptr || terminal.rows <= 0 || terminal.cols <= 0) {
    terminal.needs_full_refresh = false;
    return;
  }

  const VTermRect rect{0, terminal.rows, 0, terminal.cols};
  const std::size_t buffer_size =
      static_cast<std::size_t>(terminal.rows) * (static_cast<std::size_t>(terminal.cols) * 4 + 1) + 1;
  std::string buffer(buffer_size, '\0');
  const std::size_t text_len = vterm_screen_get_text(terminal.screen, buffer.data(), buffer.size(), rect);

  terminal.rendered_rows.clear();
  terminal.rendered_rows.reserve(static_cast<std::size_t>(terminal.rows));

  std::string current_line;
  current_line.reserve(static_cast<std::size_t>(terminal.cols) * 2);
  for (std::size_t i = 0; i < text_len; ++i) {
    const char ch = buffer[i];
    if (ch == '\n') {
      while (!current_line.empty() && current_line.back() == ' ') {
        current_line.pop_back();
      }
      terminal.rendered_rows.push_back(std::move(current_line));
      current_line = std::string{};
      current_line.reserve(static_cast<std::size_t>(terminal.cols) * 2);
      continue;
    }
    current_line.push_back(ch);
  }
  while (!current_line.empty() && current_line.back() == ' ') {
    current_line.pop_back();
  }
  terminal.rendered_rows.push_back(std::move(current_line));

  while (terminal.rendered_rows.size() < static_cast<std::size_t>(terminal.rows)) {
    terminal.rendered_rows.push_back(std::string{});
  }
  terminal.needs_full_refresh = false;
}

static void ResizeCliTerminal(CliTerminalState& terminal, const int rows, const int cols) {
  const int safe_rows = ClampTerminalRows(rows);
  const int safe_cols = ClampTerminalCols(cols);
  if (terminal.rows == safe_rows && terminal.cols == safe_cols) {
    return;
  }
  terminal.rows = safe_rows;
  terminal.cols = safe_cols;
  if (terminal.vt != nullptr) {
    vterm_set_size(terminal.vt, terminal.rows, terminal.cols);
  }
#if defined(__unix__)
  if (terminal.master_fd >= 0) {
    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(terminal.rows);
    ws.ws_col = static_cast<unsigned short>(terminal.cols);
    ioctl(terminal.master_fd, TIOCSWINSZ, &ws);
  }
#elif defined(_WIN32)
  if (terminal.pseudo_console != nullptr) {
    COORD size{static_cast<SHORT>(terminal.cols), static_cast<SHORT>(terminal.rows)};
    ResizePseudoConsoleSafe(terminal.pseudo_console, size);
  }
#endif
}

#if defined(_WIN32)
static std::ptrdiff_t ReadCliTerminalOutput(CliTerminalState& terminal, char* buffer, const size_t buffer_size) {
  if (terminal.pipe_output == INVALID_HANDLE_VALUE) {
    return -1;
  }
  DWORD available = 0;
  if (!PeekNamedPipe(terminal.pipe_output, nullptr, 0, nullptr, &available, nullptr)) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE) {
      return 0;
    }
    return -1;
  }
  if (available == 0) {
    return -2;
  }
  const DWORD to_read = static_cast<DWORD>(std::min<size_t>(buffer_size, available));
  DWORD bytes_read = 0;
  if (!ReadFile(terminal.pipe_output, buffer, to_read, &bytes_read, nullptr)) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE) {
      return 0;
    }
    return -1;
  }
  if (bytes_read == 0) {
    return -2;
  }
  return static_cast<std::ptrdiff_t>(bytes_read);
}
#endif

static void PollCliTerminal(AppState& app,
                            CliTerminalState& terminal,
                            const bool preserve_selection,
                            const bool background_poll) {
  // Drain only a bounded amount of output per frame so one busy terminal cannot freeze the UI.
  uam::TerminalDrainBudget drain_budget = uam::TerminalDrainBudgetForView(background_poll);

#if defined(_WIN32)
  if (!terminal.running || terminal.pipe_output == INVALID_HANDLE_VALUE || terminal.vt == nullptr) {
    return;
  }
  if (terminal.io_state != nullptr) {
    while (drain_budget.CanDrainMore()) {
      std::string chunk;
      {
        std::lock_guard<std::mutex> lock(terminal.io_state->mutex);
        if (terminal.io_state->pending_output.empty()) {
          break;
        }
        chunk = std::move(terminal.io_state->pending_output.front());
        terminal.io_state->pending_output.pop_front();
      }
      if (chunk.empty()) {
        continue;
      }
      vterm_input_write(terminal.vt, chunk.data(), chunk.size());
      terminal.needs_full_refresh = true;
      drain_budget.RecordRead(chunk.size());
    }

    if (terminal.io_state->write_failed.load(std::memory_order_acquire)) {
      StopCliTerminal(terminal);
      terminal.should_launch = false;
      app.status_line = "Gemini terminal write failed.";
      return;
    }
    if (terminal.io_state->read_failed.load(std::memory_order_acquire)) {
      StopCliTerminal(terminal);
      terminal.should_launch = false;
      app.status_line = "Gemini terminal read failed.";
      return;
    }
    if (terminal.io_state->output_closed.load(std::memory_order_acquire)) {
      StopCliTerminal(terminal);
      terminal.should_launch = false;
      app.status_line = "Gemini terminal exited.";
      return;
    }
  }

  if (terminal.process_info.hProcess != INVALID_HANDLE_VALUE &&
      WaitForSingleObject(terminal.process_info.hProcess, 0) == WAIT_OBJECT_0) {
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    app.status_line = "Gemini terminal exited.";
  }
#else
  if (!terminal.running || terminal.master_fd < 0 || terminal.vt == nullptr) {
    return;
  }

  char buffer[8192];
  while (drain_budget.CanDrainMore()) {
    const ssize_t read_bytes = read(terminal.master_fd, buffer, sizeof(buffer));
    if (read_bytes > 0) {
      vterm_input_write(terminal.vt, buffer, static_cast<std::size_t>(read_bytes));
      terminal.needs_full_refresh = true;
      drain_budget.RecordRead(static_cast<std::size_t>(read_bytes));
      continue;
    }
    if (read_bytes == 0) {
      StopCliTerminal(terminal);
      terminal.should_launch = false;
      app.status_line = "Gemini terminal exited.";
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    app.status_line = "Gemini terminal read failed.";
    break;
  }

  int status = 0;
  if (terminal.child_pid > 0 && waitpid(terminal.child_pid, &status, WNOHANG) > 0) {
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    app.status_line = "Gemini terminal exited.";
  }
#endif

  if (ShouldRefreshTerminalTextCache(terminal, ImGui::GetTime())) {
    RefreshCliTerminalTextCache(terminal);
    terminal.last_text_cache_refresh_s = ImGui::GetTime();
  }

  const double now = ImGui::GetTime();
  if (!uam::ShouldSyncNativeHistoryAfterTerminalPoll(terminal.running)) {
    return;
  }

  if (now - terminal.last_sync_time_s > 1.25) {
    terminal.last_sync_time_s = now;
    if (ActiveProviderUsesGeminiHistory(app)) {
      const std::string previous_chat_id = terminal.attached_chat_id;
      const std::string previous_session_id = terminal.attached_session_id;
      std::string preferred_id = previous_session_id.empty() ? previous_chat_id : previous_session_id;
      SyncChatsFromNative(app, preferred_id, preserve_selection);

      if (terminal.attached_session_id.empty()) {
        const std::string discovered = PickNewSessionId(app.chats, terminal.session_ids_before);
        if (!discovered.empty()) {
          terminal.attached_session_id = discovered;
          terminal.attached_chat_id = discovered;
          const int previous_index = FindChatIndexById(app, previous_chat_id);
          const int native_index = FindChatIndexById(app, discovered);
          if (previous_index >= 0 && native_index >= 0 && previous_chat_id != discovered) {
            ChatSession native_chat = app.chats[native_index];
            if (!ChatRepository::PromoteDraftChatToNative(app.data_root, app.chats[previous_index], native_chat)) {
              app.status_line = "Gemini session linked, but legacy draft cleanup failed.";
            }
          }
          app.chats.erase(std::remove_if(app.chats.begin(), app.chats.end(),
                                         [&](const ChatSession& c) {
                                           return !c.uses_native_session && c.id == previous_chat_id;
                                         }),
                          app.chats.end());
          SelectChatById(app, discovered);
        }
      }
    } else {
      SyncChatsFromNative(app, terminal.attached_chat_id, preserve_selection);
    }
  }
}

static void RefreshChatHistory(AppState& app) {
  const std::string selected_id = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
  SyncChatsFromNative(app, selected_id, true);
  app.status_line = "Chat history refreshed.";
}

static void PollAllCliTerminals(AppState& app) {
  const std::string selected_chat_id = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
  const bool terminal_view_visible = (app.center_view_mode == CenterViewMode::CliConsole);
  const double now = ImGui::GetTime();
  for (auto& terminal : app.cli_terminals) {
    if (terminal == nullptr) {
      continue;
    }
    const bool selected_terminal = !selected_chat_id.empty() && terminal->attached_chat_id == selected_chat_id;
    const bool background_poll = (!terminal_view_visible || !selected_terminal);
    if (background_poll) {
      if (!uam::ShouldPollBackgroundTerminalNow(now,
                                                terminal->last_background_poll_time_s,
                                                uam::HiddenTerminalPollIntervalSeconds())) {
        continue;
      }
      terminal->last_background_poll_time_s = now;
    } else {
      terminal->last_background_poll_time_s = now;
    }
    const bool preserve_selection = !selected_chat_id.empty() && terminal->attached_chat_id != selected_chat_id;
    PollCliTerminal(app, *terminal, preserve_selection, background_poll);
  }

  app.cli_terminals.erase(
      std::remove_if(app.cli_terminals.begin(), app.cli_terminals.end(),
                     [&](const std::unique_ptr<CliTerminalState>& terminal) {
                       if (terminal == nullptr) {
                         return true;
                       }
                       if (terminal->running || terminal->should_launch) {
                         return false;
                       }
                       return terminal->attached_chat_id != selected_chat_id;
                     }),
      app.cli_terminals.end());
}

static void StartGeminiRequest(AppState& app) {
  if (app.pending_call.has_value()) {
    app.status_line = "Gemini command already running.";
    return;
  }

  ChatSession* chat = SelectedChat(app);
  if (chat == nullptr) {
    app.status_line = "Select or create a chat first.";
    return;
  }

  const std::string prompt_text = Trim(app.composer_text);
  if (prompt_text.empty()) {
    app.status_line = "Prompt is empty.";
    return;
  }

  AddMessage(*chat, MessageRole::User, prompt_text);
  SaveAndUpdateStatus(app, *chat, "Prompt queued for provider runtime.", "Saved message locally, but failed to persist chat data.");

  const ProviderProfile& provider = ActiveProviderOrDefault(app);
  AppSettings runtime_settings = app.settings;
  const bool force_structured_yolo =
      ProviderRuntime::ShouldForceYoloForStructuredMode(provider, runtime_settings, app.center_view_mode);
  if (force_structured_yolo) {
    runtime_settings.gemini_yolo_mode = true;
  }
  std::vector<ChatSession> native_before;
  if (ActiveProviderUsesGeminiHistory(app)) {
    RefreshGeminiChatsDir(app);
    native_before = LoadNativeGeminiChats(app.gemini_chats_dir, provider);
  }
  const std::string resume_session_id =
      (ActiveProviderUsesGeminiHistory(app) && chat->uses_native_session) ? chat->native_session_id : "";
  const std::string provider_prompt = BuildProviderPrompt(provider, prompt_text, chat->linked_files);
  const std::string command = BuildProviderCommand(provider, runtime_settings, provider_prompt, chat->linked_files, resume_session_id);
  const std::string chat_id = chat->id;

  PendingGeminiCall pending;
  pending.chat_id = chat_id;
  pending.resume_session_id = resume_session_id;
  pending.session_ids_before = SessionIdsFromChats(native_before);
  pending.command_preview = command;
  pending.completed = std::make_shared<std::atomic<bool>>(false);
  pending.output = std::make_shared<std::string>();
  {
    std::shared_ptr<std::atomic<bool>> completed = pending.completed;
    std::shared_ptr<std::string> output = pending.output;
    std::thread([command, completed, output]() {
      *output = ExecuteCommandCaptureOutput(command);
      completed->store(true, std::memory_order_release);
    }).detach();
  }
  app.pending_call = std::move(pending);

  app.composer_text.clear();
  app.scroll_to_bottom = true;
  if (force_structured_yolo) {
    app.status_line = "Prompt queued. Structured mode auto-enabled --yolo because approve/disapprove UI is not available.";
  }
}

static void ClearEditMessageState(AppState& app) {
  app.editing_chat_id.clear();
  app.editing_message_index = -1;
  app.editing_message_text.clear();
  app.open_edit_message_popup = false;
}

static void BeginEditMessage(AppState& app, const ChatSession& chat, const int message_index) {
  if (message_index < 0 || message_index >= static_cast<int>(chat.messages.size())) {
    return;
  }
  if (chat.messages[message_index].role != MessageRole::User) {
    return;
  }
  app.editing_chat_id = chat.id;
  app.editing_message_index = message_index;
  app.editing_message_text = chat.messages[message_index].content;
  app.open_edit_message_popup = true;
}

static bool ContinueFromEditedUserMessage(AppState& app, ChatSession& chat) {
  if (app.pending_call.has_value()) {
    if (app.pending_call->chat_id != chat.id) {
      app.status_line = "Cannot edit this chat while Gemini is running in another chat.";
      return false;
    }
    app.pending_call.reset();
    app.status_line = "Stopped current run for this chat and continuing from the edited message.";
  }
  if (app.editing_chat_id != chat.id) {
    app.status_line = "Edit target no longer matches selected chat.";
    return false;
  }
  const int message_index = app.editing_message_index;
  if (message_index < 0 || message_index >= static_cast<int>(chat.messages.size())) {
    app.status_line = "Selected message index is no longer valid.";
    return false;
  }
  if (chat.messages[message_index].role != MessageRole::User) {
    app.status_line = "Only user messages can be edited.";
    return false;
  }

  const std::string prompt_text = Trim(app.editing_message_text);
  if (prompt_text.empty()) {
    app.status_line = "Edited message cannot be empty.";
    return false;
  }

  if (CliTerminalState* terminal = FindCliTerminalForChat(app, chat.id)) {
    if (terminal->running) {
      StopCliTerminal(*terminal, false);
      terminal->should_launch = false;
    }
  }

  if (ActiveProviderUsesGeminiHistory(app) && chat.uses_native_session && !chat.native_session_id.empty()) {
    std::string native_error;
    if (!TruncateNativeSessionFromDisplayedMessage(app, chat, message_index, &native_error)) {
      app.status_line = "Failed to trim native Gemini session: " + native_error;
      return false;
    }
  }

  chat.messages.erase(chat.messages.begin() + message_index, chat.messages.end());
  chat.updated_at = TimestampNow();
  if (message_index == 0) {
    std::string title = prompt_text;
    if (title.size() > 48) {
      title = title.substr(0, 45) + "...";
    }
    chat.title = title;
  }
  SaveAndUpdateStatus(app, chat, "Chat rewound to edited message.", "Chat rewound in UI, but failed to save chat data.");

  app.composer_text = prompt_text;
  StartGeminiRequest(app);
  return true;
}

static void PollPendingGeminiCall(AppState& app) {
  if (!app.pending_call.has_value()) {
    return;
  }

  if (app.pending_call->completed == nullptr || app.pending_call->output == nullptr) {
    app.pending_call.reset();
    return;
  }
  if (!app.pending_call->completed->load(std::memory_order_acquire)) {
    return;
  }
  const std::string output = *app.pending_call->output;
  const std::string pending_chat_id = app.pending_call->chat_id;
  const int pending_chat_index = FindChatIndexById(app, pending_chat_id);
  ChatSession pending_chat_snapshot;
  if (pending_chat_index >= 0) {
    pending_chat_snapshot = app.chats[pending_chat_index];
  }

  if (!ActiveProviderUsesGeminiHistory(app)) {
    if (pending_chat_index >= 0) {
      AddMessage(app.chats[pending_chat_index], MessageRole::Assistant, output);
      SaveChat(app, app.chats[pending_chat_index]);
      app.status_line = "Provider response appended to local chat history.";
      app.scroll_to_bottom = true;
    } else {
      app.status_line = "Provider command completed, but chat no longer exists.";
    }
    app.pending_call.reset();
    return;
  }

  RefreshGeminiChatsDir(app);
  std::vector<ChatSession> native_after = LoadNativeGeminiChats(app.gemini_chats_dir, ActiveProviderOrDefault(app));
  ApplyLocalOverrides(app, native_after);

  std::string selected_id = app.pending_call->resume_session_id;
  if (selected_id.empty()) {
    selected_id = PickNewSessionId(native_after, app.pending_call->session_ids_before);
  }

  if (!selected_id.empty()) {
    SelectChatById(app, selected_id);
    const int selected_index = FindChatIndexById(app, selected_id);
    bool cleanup_failed = false;
    if (selected_index >= 0 && pending_chat_index >= 0) {
      const bool promoted = ChatRepository::PromoteDraftChatToNative(app.data_root,
                                                                      pending_chat_snapshot,
                                                                      app.chats[selected_index]);
      if (!promoted) {
        app.status_line = "Provider response synced from native session, but legacy draft cleanup failed.";
        cleanup_failed = true;
      }
    }
    if (selected_id != pending_chat_id) {
      app.chats.erase(
          std::remove_if(app.chats.begin(), app.chats.end(), [&](const ChatSession& chat) { return chat.id == pending_chat_id; }),
          app.chats.end());
      SelectChatById(app, selected_id);
    }
    app.scroll_to_bottom = true;
    if (!cleanup_failed) {
      app.status_line = "Provider response synced from native session.";
    }
  } else {
    const int fallback_index = FindChatIndexById(app, pending_chat_id);
    if (fallback_index >= 0) {
      AddMessage(app.chats[fallback_index], MessageRole::System, output);
      app.status_line = "Provider command completed, but no native session was detected.";
      app.scroll_to_bottom = true;
    } else {
      app.status_line = "Provider command completed, but no native session was detected.";
    }
  }

  app.pending_call.reset();
}

static void RemoveCurrentChat(AppState& app) {
  ChatSession* chat = SelectedChat(app);
  if (chat == nullptr) {
    return;
  }
  if (app.pending_call.has_value() && app.pending_call->chat_id == chat->id) {
    app.status_line = "Cannot delete a chat while Gemini is still running for it.";
    return;
  }
  const std::string id = chat->id;
  const std::string native_session_id = chat->uses_native_session ? chat->native_session_id : "";
  std::error_code local_delete_ec;
  fs::remove_all(ChatPath(app, *chat), local_delete_ec);
  std::error_code native_delete_ec;
  bool native_delete_attempted = false;
  if (ActiveProviderUsesGeminiHistory(app) && !native_session_id.empty()) {
    RefreshGeminiChatsDir(app);
    if (const auto native_file = FindNativeSessionFilePath(app, native_session_id); native_file.has_value()) {
      native_delete_attempted = true;
      fs::remove(native_file.value(), native_delete_ec);
    }
  }

  app.chats.erase(app.chats.begin() + app.selected_chat_index);
  if (app.chats.empty()) {
    app.selected_chat_index = -1;
  } else if (app.selected_chat_index >= static_cast<int>(app.chats.size())) {
    app.selected_chat_index = static_cast<int>(app.chats.size()) - 1;
  }

  if (app.pending_call.has_value() && app.pending_call->chat_id == id) {
    app.pending_call.reset();
  }
  if (app.editing_chat_id == id) {
    ClearEditMessageState(app);
  }
  StopAndEraseCliTerminalForChat(app, id);

  if (local_delete_ec) {
    app.status_line = "Chat removed from UI, but deleting local history failed.";
  } else if (native_delete_attempted && native_delete_ec) {
    app.status_line = "Chat removed from UI, but deleting native Gemini history failed.";
  } else {
    app.status_line = "Chat deleted.";
  }
}

static void CreateAndSelectChat(AppState& app) {
  ChatSession chat = CreateNewChat(FolderForNewChat(app));
  const std::string id = chat.id;
  app.chats.push_back(chat);
  SortChatsByRecent(app.chats);
  SelectChatById(app, id);
  if (app.center_view_mode == CenterViewMode::CliConsole) {
    MarkSelectedCliTerminalForLaunch(app);
  }
  if (!SaveChat(app, app.chats[app.selected_chat_index])) {
    app.status_line = "Created chat in memory, but failed to persist.";
  } else {
    app.status_line = "New chat created.";
  }
}

static ImVec4 Rgb(const int r, const int g, const int b, const float a = 1.0f) {
  return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
}

namespace ui {
constexpr float kSpace4 = 4.0f;
constexpr float kSpace8 = 8.0f;
constexpr float kSpace10 = 10.0f;
constexpr float kSpace12 = 12.0f;
constexpr float kSpace16 = 16.0f;
constexpr float kSpace20 = 20.0f;
constexpr float kSpace24 = 24.0f;
constexpr float kSpace32 = 32.0f;

constexpr float kSidebarWidth = 292.0f;
constexpr float kRightPanelWidth = 336.0f;

constexpr float kRadiusSmall = 8.0f;
constexpr float kRadiusPanel = 14.0f;
constexpr float kRadiusInput = 12.0f;

const ImVec4 kMainBackground = Rgb(8, 12, 17, 1.0f);
const ImVec4 kPrimarySurface = Rgb(12, 17, 24, 0.95f);
const ImVec4 kSecondarySurface = Rgb(16, 22, 31, 0.95f);
const ImVec4 kElevatedSurface = Rgb(22, 30, 42, 0.98f);
const ImVec4 kInputSurface = Rgb(10, 15, 22, 0.94f);
const ImVec4 kBorder = Rgb(255, 255, 255, 0.08f);
const ImVec4 kBorderStrong = Rgb(130, 171, 255, 0.35f);
const ImVec4 kShadow = Rgb(0, 0, 0, 0.36f);
const ImVec4 kShadowSoft = Rgb(0, 0, 0, 0.20f);

const ImVec4 kTextPrimary = Rgb(234, 240, 248, 1.0f);
const ImVec4 kTextSecondary = Rgb(178, 189, 203, 1.0f);
const ImVec4 kTextMuted = Rgb(132, 145, 160, 1.0f);

const ImVec4 kAccent = Rgb(96, 160, 255, 1.0f);
const ImVec4 kAccentSoft = Rgb(96, 160, 255, 0.24f);
const ImVec4 kSuccess = Rgb(34, 197, 94, 1.0f);
const ImVec4 kError = Rgb(255, 107, 107, 1.0f);
const ImVec4 kWarning = Rgb(245, 158, 11, 1.0f);
const ImVec4 kTransparent = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
}  // namespace ui

enum class PanelTone {
  Primary,
  Secondary,
  Elevated
};

enum class ButtonKind {
  Primary,
  Ghost,
  Accent
};

static ImFont* TryLoadFont(ImGuiIO& io,
                           const float size,
                           const std::vector<const char*>& paths,
                           const ImFontConfig* config = nullptr,
                           const ImWchar* glyph_ranges = nullptr) {
  for (const char* path : paths) {
    if (path != nullptr && fs::exists(path)) {
      if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, size, config, glyph_ranges)) {
        return font;
      }
    }
  }
  return nullptr;
}

static ImFont* TryLoadFont(ImGuiIO& io,
                           const float size,
                           std::initializer_list<const char*> paths,
                           const ImFontConfig* config = nullptr,
                           const ImWchar* glyph_ranges = nullptr) {
  return TryLoadFont(io, size, std::vector<const char*>{paths}, config, glyph_ranges);
}

static const ImVector<ImWchar>& TerminalGlyphRanges(ImGuiIO& io) {
  if (!g_terminal_glyph_ranges.empty()) {
    return g_terminal_glyph_ranges;
  }

  static const ImWchar kBoxDrawingRanges[] = {0x2500, 0x259F, 0};
  static const ImWchar kDingbatsRanges[] = {0x2600, 0x26FF, 0};
  static const ImWchar kMathSymbolsRanges[] = {0x2200, 0x22FF, 0};

  ImFontGlyphRangesBuilder builder;
  builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
  builder.AddRanges(io.Fonts->GetGlyphRangesGreek());
  builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
  builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
  builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
  builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
  builder.AddRanges(io.Fonts->GetGlyphRangesThai());
  builder.AddRanges(io.Fonts->GetGlyphRangesVietnamese());
  builder.AddRanges(kBoxDrawingRanges);
  builder.AddRanges(kDingbatsRanges);
  builder.AddRanges(kMathSymbolsRanges);
  builder.BuildRanges(&g_terminal_glyph_ranges);
  return g_terminal_glyph_ranges;
}

static bool MergeFontFallbacks(ImGuiIO& io,
                               ImFont* target_font,
                               const float size,
                               const std::vector<const char*>& fallback_paths,
                               const ImWchar* glyph_ranges) {
  if (target_font == nullptr) {
    return false;
  }

  ImFontConfig config;
  config.MergeMode = true;
  config.PixelSnapH = true;
  config.DstFont = target_font;

  bool merged_any = false;
  for (const char* path : fallback_paths) {
    if (path == nullptr || !fs::exists(path)) {
      continue;
    }
    if (io.Fonts->AddFontFromFileTTF(path, size, &config, glyph_ranges) != nullptr) {
      merged_any = true;
    }
  }
  return merged_any;
}

static void ConfigureFonts(ImGuiIO& io, const float dpi_scale = 1.0f) {
  const float scale = std::clamp(dpi_scale, 1.0f, 2.25f);
  const uam::TerminalTypographyConfig& terminal_typography = uam::TerminalTypographyConfigForPlatform();
  const ImVector<ImWchar>& terminal_ranges = TerminalGlyphRanges(io);
  g_font_ui = TryLoadFont(io, 14.0f * scale, {
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/arial.ttf",
      "/Library/Fonts/Inter-Regular.ttf",
      "/Library/Fonts/Inter Variable.ttf",
      "/System/Library/Fonts/Supplemental/Helvetica.ttc",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
  });
  g_font_title = TryLoadFont(io, 19.0f * scale, {
      "C:/Windows/Fonts/seguisb.ttf",
      "C:/Windows/Fonts/segoeuib.ttf",
      "/Library/Fonts/Inter-SemiBold.ttf",
      "/Library/Fonts/Inter-Bold.ttf",
      "/System/Library/Fonts/Supplemental/Helvetica Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"
  });
  g_font_mono = TryLoadFont(io,
                            terminal_typography.mono_font_size * scale,
                            terminal_typography.mono_font_candidates,
                            nullptr,
                            terminal_ranges.Data);
  g_font_mono_readable = TryLoadFont(io,
                                     terminal_typography.mono_font_size * scale,
                                     terminal_typography.readable_mono_font_candidates,
                                     nullptr,
                                     terminal_ranges.Data);
  if (g_font_mono_readable == nullptr) {
    g_font_mono_readable = g_font_mono;
  }

  if (!terminal_typography.fallback_font_candidates.empty()) {
    MergeFontFallbacks(io,
                       g_font_mono,
                       terminal_typography.mono_font_size * scale,
                       terminal_typography.fallback_font_candidates,
                       terminal_ranges.Data);
    if (g_font_mono_readable != g_font_mono) {
      MergeFontFallbacks(io,
                         g_font_mono_readable,
                         terminal_typography.mono_font_size * scale,
                         terminal_typography.fallback_font_candidates,
                         terminal_ranges.Data);
    }
  }

  if (g_font_ui != nullptr) {
    io.FontDefault = g_font_ui;
  }
}

static void BeginDeferredNativeHistoryLoad(AppState& app) {
  if (!ActiveProviderUsesGeminiHistory(app) || app.gemini_chats_dir.empty() || app.deferred_native_history_loading) {
    return;
  }

  const ProviderProfile provider = ActiveProviderOrDefault(app);
  const fs::path chats_dir = app.gemini_chats_dir;
  app.deferred_native_history_load =
      std::async(std::launch::async, [chats_dir, provider]() { return LoadNativeGeminiChats(chats_dir, provider); });
  app.deferred_native_history_loading = true;
}

static void PollDeferredNativeHistoryLoad(AppState& app) {
  if (!app.deferred_native_history_loading) {
    return;
  }
  if (!app.deferred_native_history_load.valid()) {
    app.deferred_native_history_loading = false;
    return;
  }

  if (app.deferred_native_history_load.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return;
  }

  const std::string selected_before = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
  try {
    std::vector<ChatSession> native = app.deferred_native_history_load.get();
    ApplyLocalOverrides(app, native);
    if (!selected_before.empty() && FindChatIndexById(app, selected_before) >= 0) {
      SelectChatById(app, selected_before);
    } else if (app.selected_chat_index < 0 && !app.chats.empty()) {
      app.selected_chat_index = 0;
    }
    if (app.status_line.empty() || app.status_line.find("not found yet") != std::string::npos) {
      app.status_line = "Chat history loaded.";
    }
  } catch (...) {
    app.status_line = "Failed to load native Gemini history in background.";
  }
  app.deferred_native_history_loading = false;
}

static float DetectUiScale(SDL_Window* window) {
  int display_index = 0;
  if (window != nullptr) {
    const int window_display = SDL_GetWindowDisplayIndex(window);
    if (window_display >= 0) {
      display_index = window_display;
    }
  }

  float ddpi = 96.0f;
  float hdpi = 96.0f;
  float vdpi = 96.0f;
  if (SDL_GetDisplayDPI(display_index, &ddpi, &hdpi, &vdpi) == 0 && ddpi > 0.0f) {
    return std::clamp(ddpi / 96.0f, 1.0f, 2.25f);
  }
  return 1.0f;
}

static void PushFontIfAvailable(const ImFont* font) {
  if (font != nullptr) {
    ImGui::PushFont(const_cast<ImFont*>(font));
  }
}

static void PopFontIfAvailable(const ImFont* font) {
  if (font != nullptr) {
    ImGui::PopFont();
  }
}

static ImVec4 PanelColor(const PanelTone tone) {
  switch (tone) {
    case PanelTone::Primary:
      return ui::kPrimarySurface;
    case PanelTone::Secondary:
      return ui::kSecondarySurface;
    case PanelTone::Elevated:
      return ui::kElevatedSurface;
  }
  return ui::kPrimarySurface;
}

static ImVec4 PanelStrokeColor(const PanelTone tone) {
  switch (tone) {
    case PanelTone::Primary:
      return ui::kBorderStrong;
    case PanelTone::Secondary:
      return ui::kBorder;
    case PanelTone::Elevated:
      return Rgb(255, 255, 255, 0.10f);
  }
  return ui::kBorder;
}

static void PushInputChrome(const float rounding = ui::kRadiusSmall) {
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::kInputSurface);
  ImGui::PushStyleColor(ImGuiCol_Border, ui::kBorder);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgb(16, 23, 33, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgb(20, 29, 40, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
}

static void PopInputChrome() {
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
}

static bool BeginPanel(const char* id, const ImVec2& size, const PanelTone tone, const bool border = true,
                       const ImGuiWindowFlags flags = 0, const ImVec2 padding = ImVec2(ui::kSpace16, ui::kSpace16),
                       const float rounding = ui::kRadiusPanel) {
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, PanelColor(tone));
  ImGui::PushStyleColor(ImGuiCol_Border, border ? PanelStrokeColor(tone) : ui::kTransparent);
  const bool is_open = ImGui::BeginChild(id, size, border, flags);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 min = ImGui::GetWindowPos();
  const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
  draw->AddRectFilled(ImVec2(min.x + 1.0f, min.y + 4.0f), ImVec2(max.x + 1.0f, max.y + 4.0f), ImGui::GetColorU32(ui::kShadowSoft), rounding);
  if (border) {
    draw->AddRect(min, max, ImGui::GetColorU32(PanelStrokeColor(tone)), rounding, 0, 1.0f);
  }
  if (tone == PanelTone::Primary) {
    draw->AddRectFilledMultiColor(
        min,
        ImVec2(max.x, min.y + 2.0f),
        ImGui::GetColorU32(Rgb(130, 171, 255, 0.35f)),
        ImGui::GetColorU32(Rgb(130, 171, 255, 0.10f)),
        ImGui::GetColorU32(ui::kTransparent),
        ImGui::GetColorU32(ui::kTransparent));
  }
  return is_open;
}

static void EndPanel() {
  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
}

static bool DrawButton(const char* label, const ImVec2& size, const ButtonKind kind) {
  ImVec4 bg = ui::kElevatedSurface;
  ImVec4 bg_hover = Rgb(31, 42, 57, 1.0f);
  ImVec4 bg_active = Rgb(35, 47, 64, 1.0f);
  ImVec4 border = ui::kBorder;
  ImVec4 text = ui::kTextPrimary;

  if (kind == ButtonKind::Primary) {
    bg = Rgb(84, 147, 255, 0.94f);
    bg_hover = Rgb(100, 160, 255, 1.0f);
    bg_active = Rgb(71, 133, 238, 1.0f);
    border = Rgb(140, 182, 255, 0.70f);
    text = Rgb(255, 255, 255, 1.0f);
  } else if (kind == ButtonKind::Accent) {
    bg = Rgb(48, 108, 201, 0.92f);
    bg_hover = Rgb(62, 121, 215, 1.0f);
    bg_active = Rgb(42, 98, 189, 1.0f);
    border = Rgb(116, 167, 255, 0.65f);
    text = Rgb(255, 255, 255, 1.0f);
  } else if (kind == ButtonKind::Ghost) {
    bg = ui::kTransparent;
    bg_hover = Rgb(255, 255, 255, 0.08f);
    bg_active = Rgb(255, 255, 255, 0.12f);
    border = Rgb(255, 255, 255, 0.10f);
  }

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(13.0f, 8.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, bg);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_hover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_active);
  ImGui::PushStyleColor(ImGuiCol_Border, border);
  ImGui::PushStyleColor(ImGuiCol_Text, text);
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(2);
  return clicked;
}

static void DrawSoftDivider() {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const ImU32 left = ImGui::GetColorU32(Rgb(255, 255, 255, 0.0f));
  const ImU32 center = ImGui::GetColorU32(Rgb(255, 255, 255, 0.12f));
  draw->AddRectFilledMultiColor(p, ImVec2(p.x + width, p.y + 1.0f), left, center, center, left);
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
}

static std::string CompactPreview(const std::string& text, const std::size_t max_len) {
  std::string compact = text;
  std::replace(compact.begin(), compact.end(), '\n', ' ');
  compact = Trim(compact);
  if (compact.size() <= max_len) {
    return compact;
  }
  if (max_len < 4) {
    return compact.substr(0, max_len);
  }
  return compact.substr(0, max_len - 3) + "...";
}

static void ApplyModernTheme() {
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(ui::kSpace20, ui::kSpace20);
  style.FramePadding = ImVec2(12.0f, 9.0f);
  style.CellPadding = ImVec2(ui::kSpace12, ui::kSpace8);
  style.ItemSpacing = ImVec2(ui::kSpace12, ui::kSpace10);
  style.ItemInnerSpacing = ImVec2(ui::kSpace8, ui::kSpace8);
  style.IndentSpacing = 18.0f;
  style.ScrollbarSize = 10.0f;
  style.GrabMinSize = 10.0f;

  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;
  style.TabBorderSize = 0.0f;

  style.WindowRounding = 10.0f;
  style.ChildRounding = ui::kRadiusPanel;
  style.FrameRounding = 10.0f;
  style.PopupRounding = ui::kRadiusPanel;
  style.ScrollbarRounding = 9.0f;
  style.GrabRounding = ui::kRadiusSmall;
  style.TabRounding = 10.0f;

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = ui::kTextPrimary;
  colors[ImGuiCol_TextDisabled] = ui::kTextMuted;
  colors[ImGuiCol_WindowBg] = ui::kMainBackground;
  colors[ImGuiCol_ChildBg] = ui::kPrimarySurface;
  colors[ImGuiCol_PopupBg] = Rgb(16, 22, 30, 0.98f);
  colors[ImGuiCol_Border] = ui::kBorder;
  colors[ImGuiCol_BorderShadow] = ui::kTransparent;
  colors[ImGuiCol_FrameBg] = ui::kInputSurface;
  colors[ImGuiCol_FrameBgHovered] = Rgb(17, 24, 34, 1.0f);
  colors[ImGuiCol_FrameBgActive] = Rgb(22, 31, 43, 1.0f);
  colors[ImGuiCol_TitleBg] = ui::kPrimarySurface;
  colors[ImGuiCol_TitleBgActive] = ui::kPrimarySurface;
  colors[ImGuiCol_MenuBarBg] = ui::kSecondarySurface;
  colors[ImGuiCol_ScrollbarBg] = ui::kTransparent;
  colors[ImGuiCol_ScrollbarGrab] = Rgb(255, 255, 255, 0.18f);
  colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(255, 255, 255, 0.28f);
  colors[ImGuiCol_ScrollbarGrabActive] = Rgb(255, 255, 255, 0.35f);
  colors[ImGuiCol_CheckMark] = ui::kAccent;
  colors[ImGuiCol_SliderGrab] = ui::kAccent;
  colors[ImGuiCol_SliderGrabActive] = Rgb(122, 176, 255, 1.0f);
  colors[ImGuiCol_Button] = ui::kElevatedSurface;
  colors[ImGuiCol_ButtonHovered] = Rgb(31, 42, 57, 1.0f);
  colors[ImGuiCol_ButtonActive] = Rgb(38, 50, 68, 1.0f);
  colors[ImGuiCol_Header] = Rgb(255, 255, 255, 0.05f);
  colors[ImGuiCol_HeaderHovered] = Rgb(255, 255, 255, 0.08f);
  colors[ImGuiCol_HeaderActive] = ui::kAccentSoft;
  colors[ImGuiCol_Separator] = ui::kBorder;
  colors[ImGuiCol_ResizeGrip] = Rgb(255, 255, 255, 0.10f);
  colors[ImGuiCol_ResizeGripHovered] = Rgb(96, 160, 255, 0.42f);
  colors[ImGuiCol_ResizeGripActive] = Rgb(96, 160, 255, 0.75f);
  colors[ImGuiCol_Tab] = ui::kSecondarySurface;
  colors[ImGuiCol_TabHovered] = Rgb(24, 34, 48, 1.0f);
  colors[ImGuiCol_TabActive] = ui::kElevatedSurface;
  colors[ImGuiCol_TabUnfocused] = ui::kSecondarySurface;
  colors[ImGuiCol_TabUnfocusedActive] = ui::kElevatedSurface;
  colors[ImGuiCol_TableHeaderBg] = ui::kSecondarySurface;
  colors[ImGuiCol_TableBorderStrong] = ui::kBorder;
  colors[ImGuiCol_TableBorderLight] = Rgb(255, 255, 255, 0.05f);
  colors[ImGuiCol_TableRowBg] = ui::kTransparent;
  colors[ImGuiCol_TableRowBgAlt] = Rgb(255, 255, 255, 0.03f);
  colors[ImGuiCol_TextSelectedBg] = Rgb(96, 160, 255, 0.32f);
  colors[ImGuiCol_DragDropTarget] = ui::kAccent;
  colors[ImGuiCol_NavCursor] = ui::kAccent;
  colors[ImGuiCol_NavWindowingHighlight] = Rgb(96, 160, 255, 0.65f);
  colors[ImGuiCol_ModalWindowDimBg] = Rgb(0, 0, 0, 0.45f);
}

static void DrawAmbientBackdrop(const ImVec2& pos, const ImVec2& size, const float time_s) {
  ImDrawList* draw = ImGui::GetBackgroundDrawList();
  draw->AddRectFilledMultiColor(
      pos,
      ImVec2(pos.x + size.x, pos.y + size.y),
      ImGui::GetColorU32(Rgb(8, 12, 17, 1.0f)),
      ImGui::GetColorU32(Rgb(10, 15, 22, 1.0f)),
      ImGui::GetColorU32(Rgb(7, 11, 16, 1.0f)),
      ImGui::GetColorU32(Rgb(6, 10, 15, 1.0f)));

  const float pulse = 0.70f + 0.30f * std::sin(time_s * 0.35f);
  draw->AddCircleFilled(ImVec2(pos.x + size.x * 0.16f, pos.y + size.y * 0.19f), size.x * 0.20f,
                        ImGui::GetColorU32(Rgb(96, 160, 255, 0.07f * pulse)), 72);
  draw->AddCircleFilled(ImVec2(pos.x + size.x * 0.86f, pos.y + size.y * 0.08f), size.x * 0.15f,
                        ImGui::GetColorU32(Rgb(90, 130, 255, 0.05f * pulse)), 60);
  draw->AddRectFilledMultiColor(
      ImVec2(pos.x, pos.y + size.y * 0.68f),
      ImVec2(pos.x + size.x, pos.y + size.y),
      ImGui::GetColorU32(ui::kTransparent),
      ImGui::GetColorU32(ui::kTransparent),
      ImGui::GetColorU32(Rgb(0, 0, 0, 0.35f)),
      ImGui::GetColorU32(Rgb(0, 0, 0, 0.35f)));
}

static bool DrawSidebarItem(const ChatSession& chat, const bool selected, const std::string& item_id) {
  const ImVec2 card_size(ImGui::GetContentRegionAvail().x, 52.0f);
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(item_id.c_str(), card_size);
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  ImVec4 bg_color = ui::kTransparent;
  ImVec4 border_color = ui::kTransparent;
  if (selected) {
    bg_color = Rgb(96, 160, 255, 0.20f);
    border_color = ui::kBorderStrong;
  } else if (hovered) {
    bg_color = Rgb(255, 255, 255, 0.07f);
    border_color = ui::kBorder;
  }
  const ImVec2 max(min.x + card_size.x, min.y + card_size.y);
  draw->AddRectFilled(min, max, ImGui::GetColorU32(bg_color), 10.0f);
  if (selected || hovered) {
    draw->AddRect(min, max, ImGui::GetColorU32(border_color), 10.0f);
  }
  if (selected) {
    draw->AddRectFilled(min, ImVec2(min.x + 3.0f, max.y), ImGui::GetColorU32(ui::kAccent), 10.0f, ImDrawFlags_RoundCornersLeft);
  }

  const std::string row_title = CompactPreview(Trim(chat.title).empty() ? chat.id : chat.title, 34);
  std::string row_subtitle = chat.updated_at;
  if (!chat.messages.empty()) {
    row_subtitle = CompactPreview(chat.messages.back().content, 46);
  }
  row_subtitle = CompactPreview(Trim(row_subtitle), 46);
  if (row_subtitle.empty()) {
    row_subtitle = "No messages yet";
  }

  draw->AddText(ImVec2(min.x + 10.0f, min.y + 7.0f), ImGui::GetColorU32(ui::kTextPrimary), row_title.c_str());
  draw->AddText(ImVec2(min.x + 10.0f, min.y + 29.0f), ImGui::GetColorU32(ui::kTextMuted), row_subtitle.c_str());
  ImGui::Dummy(ImVec2(0.0f, 6.0f));
  return clicked;
}

static bool DrawFolderHeaderItem(const ChatFolder& folder, const int chat_count) {
  const ImVec2 row_size(ImGui::GetContentRegionAvail().x, 30.0f);
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("folder_header", row_size);
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 max(min.x + row_size.x, min.y + row_size.y);
  if (hovered) {
    draw->AddRectFilled(min, max, ImGui::GetColorU32(Rgb(255, 255, 255, 0.05f)), 8.0f);
    draw->AddRect(min, max, ImGui::GetColorU32(ui::kBorder), 8.0f);
  }

  const std::string marker = folder.collapsed ? ">" : "v";
  const std::string title = FolderTitleOrFallback(folder);
  const std::string count_text = std::to_string(chat_count);
  const float count_w = ImGui::CalcTextSize(count_text.c_str()).x;
  const float count_pad = 8.0f;
  const float badge_w = count_w + (count_pad * 2.0f);
  const ImVec2 badge_min(max.x - badge_w - 8.0f, min.y + 6.0f);
  const ImVec2 badge_max(max.x - 8.0f, max.y - 6.0f);
  draw->AddRectFilled(badge_min, badge_max, ImGui::GetColorU32(Rgb(255, 255, 255, 0.09f)), 10.0f);

  draw->AddText(ImVec2(min.x + 8.0f, min.y + 7.0f), ImGui::GetColorU32(ui::kTextMuted), marker.c_str());
  draw->AddText(ImVec2(min.x + 30.0f, min.y + 7.0f), ImGui::GetColorU32(ui::kTextPrimary), CompactPreview(title, 24).c_str());
  draw->AddText(ImVec2(badge_min.x + count_pad, min.y + 7.0f), ImGui::GetColorU32(ui::kTextSecondary), count_text.c_str());
  return clicked;
}

static void DrawLeftPane(AppState& app) {
  BeginPanel("left_sidebar", ImVec2(0.0f, 0.0f), PanelTone::Secondary, true, 0, ImVec2(ui::kSpace16, ui::kSpace16));
  EnsureNewChatFolderSelection(app);
  PushFontIfAvailable(g_font_title);
  ImGui::TextColored(ui::kTextPrimary, "Universal Agent Manager");
  PopFontIfAvailable(g_font_title);
  ImGui::TextColored(ui::kTextMuted, "%zu chats across %zu folders", app.chats.size(), app.folders.size());
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
  DrawSoftDivider();

  const ChatFolder* create_folder = FindFolderById(app, app.new_chat_folder_id);
  const std::string create_folder_label = (create_folder != nullptr) ? FolderTitleOrFallback(*create_folder) : std::string(kDefaultFolderTitle);
  ImGui::TextColored(ui::kTextMuted, "New chat destination");
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##new_chat_in", create_folder_label.c_str())) {
    for (const ChatFolder& folder : app.folders) {
      const std::string folder_label = FolderTitleOrFallback(folder);
      const bool selected = (app.new_chat_folder_id == folder.id);
      if (ImGui::Selectable(folder_label.c_str(), selected)) {
        app.new_chat_folder_id = folder.id;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  const std::string new_chat_label = FrontendActionLabel(app, "create_chat", "+ New Chat");
  const std::string refresh_label = FrontendActionLabel(app, "refresh_history", "Refresh");
  const std::string delete_label = FrontendActionLabel(app, "delete_chat", "Del");

  const bool show_create_chat = FrontendActionVisible(app, "create_chat");
  const float row_width = ImGui::GetContentRegionAvail().x;
  if (show_create_chat) {
    if (DrawButton(new_chat_label.c_str(), ImVec2(row_width, 38.0f), ButtonKind::Primary)) {
      CreateAndSelectChat(app);
    }
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  }

  const bool show_refresh = FrontendActionVisible(app, "refresh_history");
  const float secondary_w = show_refresh ? std::max(90.0f, (row_width - ui::kSpace8) * 0.5f) : row_width;
  if (DrawButton("+ Folder", ImVec2(secondary_w, 34.0f), ButtonKind::Ghost)) {
    app.new_folder_title_input.clear();
    app.new_folder_directory_input = fs::current_path().string();
    ImGui::OpenPopup("new_folder_popup");
  }
  if (show_refresh) {
    ImGui::SameLine();
    if (DrawButton(refresh_label.c_str(), ImVec2(secondary_w, 34.0f), ButtonKind::Ghost)) {
      RefreshChatHistory(app);
    }
  }

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  const bool has_selected = (SelectedChat(app) != nullptr);
  if (!has_selected) {
    ImGui::BeginDisabled();
  }
  if (FrontendActionVisible(app, "delete_chat") &&
      DrawButton(delete_label.c_str(), ImVec2(row_width, 34.0f), ButtonKind::Ghost)) {
    RemoveCurrentChat(app);
  }
  if (!has_selected) {
    ImGui::EndDisabled();
  }

  if (ImGui::BeginPopupModal("new_folder_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextColored(ui::kTextPrimary, "Create chat folder");
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    ImGui::SetNextItemWidth(420.0f);
    PushInputChrome();
    ImGui::InputText("Title", &app.new_folder_title_input);
    ImGui::SetNextItemWidth(420.0f);
    ImGui::InputText("Directory", &app.new_folder_directory_input);
    PopInputChrome();
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    if (DrawButton("Create", ImVec2(96.0f, 32.0f), ButtonKind::Primary)) {
      const std::string folder_title = Trim(app.new_folder_title_input);
      const std::string folder_dir = Trim(app.new_folder_directory_input);
      if (folder_title.empty()) {
        app.status_line = "Folder title is required.";
      } else if (folder_dir.empty()) {
        app.status_line = "Folder directory is required.";
      } else {
        ChatFolder folder;
        folder.id = NewFolderId();
        folder.title = folder_title;
        folder.directory = folder_dir;
        folder.collapsed = false;
        app.folders.push_back(std::move(folder));
        app.new_chat_folder_id = app.folders.back().id;
        SaveFolders(app);
        app.status_line = "Chat folder created.";
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
  DrawSoftDivider();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::kSpace8, ui::kSpace8));

  for (ChatFolder& folder : app.folders) {
    const int chat_count = CountChatsInFolder(app, folder.id);
    ImGui::PushID(folder.id.c_str());
    if (DrawFolderHeaderItem(folder, chat_count)) {
      app.new_chat_folder_id = folder.id;
      folder.collapsed = !folder.collapsed;
      SaveFolders(app);
    }
    if (!folder.collapsed) {
      ImGui::Indent(10.0f);
      for (int i = 0; i < static_cast<int>(app.chats.size()); ++i) {
        if (app.chats[i].folder_id != folder.id) {
          continue;
        }
        if (DrawSidebarItem(app.chats[i], i == app.selected_chat_index, "session_" + app.chats[i].id)) {
          app.selected_chat_index = i;
          if (app.center_view_mode == CenterViewMode::CliConsole) {
            MarkSelectedCliTerminalForLaunch(app);
          }
        }
      }
      if (chat_count == 0) {
        ImGui::TextColored(ui::kTextMuted, "No chats in folder");
      }
      ImGui::Unindent(10.0f);
    }
    ImGui::PopID();
  }

  ImGui::PopStyleVar();
  EndPanel();
}

static void DrawMessageBubble(AppState& app, ChatSession& chat, const int message_index, const float content_width) {
  if (message_index < 0 || message_index >= static_cast<int>(chat.messages.size())) {
    return;
  }
  const Message& message = chat.messages[message_index];
  const bool is_user = (message.role == MessageRole::User);
  const bool is_assistant = (message.role == MessageRole::Assistant);
  const float max_width = content_width * 0.74f;
  const float pad_x = 16.0f;
  const float pad_y = 14.0f;
  const float text_wrap_width = max_width - (pad_x * 2.0f);
  const ImVec2 text_size = ImGui::CalcTextSize(message.content.c_str(), nullptr, false, text_wrap_width);
  const float header_h = ImGui::GetTextLineHeight();
  const float meta_h = ImGui::GetTextLineHeight();
  const float bubble_w = std::max(220.0f, std::min(max_width, text_size.x + pad_x * 2.0f));
  const float bubble_h = pad_y + header_h + 8.0f + text_size.y + 10.0f + meta_h + pad_y;

  ImVec2 cursor = ImGui::GetCursorScreenPos();
  const float bubble_x = is_user ? (cursor.x + content_width - bubble_w) : cursor.x;
  const ImVec2 min(bubble_x, cursor.y);
  const ImVec2 max(bubble_x + bubble_w, cursor.y + bubble_h);

  const ImVec4 bg = is_user ? Rgb(63, 114, 205, 0.42f) : (is_assistant ? Rgb(16, 24, 35, 0.98f) : Rgb(64, 45, 16, 0.34f));
  const ImVec4 border = is_user ? Rgb(140, 182, 255, 0.52f) : (is_assistant ? ui::kBorder : Rgb(245, 158, 11, 0.45f));
  const ImVec4 role = is_assistant ? ui::kSuccess : (is_user ? ui::kAccent : ui::kWarning);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(min.x + 1.0f, min.y + 3.0f), ImVec2(max.x + 1.0f, max.y + 3.0f), ImGui::GetColorU32(ui::kShadow), 14.0f);
  draw->AddRectFilled(min, max, ImGui::GetColorU32(bg), 14.0f);
  draw->AddRect(min, max, ImGui::GetColorU32(border), 14.0f, 0, 1.1f);

  const char* role_label = is_user ? "You" : (is_assistant ? "Assistant" : "System");
  ImGui::SetCursorScreenPos(ImVec2(min.x + pad_x, min.y + pad_y));
  ImGui::TextColored(role, "%s", role_label);
  if (is_user) {
    const std::string edit_label = FrontendActionLabel(app, "edit_resubmit", "Edit");
    ImGui::SetCursorScreenPos(ImVec2(max.x - 80.0f, min.y + 7.0f));
    if (FrontendActionVisible(app, "edit_resubmit", true) &&
        DrawButton(edit_label.c_str(), ImVec2(70.0f, 24.0f), ButtonKind::Ghost)) {
      BeginEditMessage(app, chat, message_index);
    }
  }

  ImGui::SetCursorScreenPos(ImVec2(min.x + pad_x, min.y + pad_y + header_h + 2.0f));
  const float wrap_pos_x = ImGui::GetCursorPosX() + (bubble_w - (pad_x * 2.0f));
  ImGui::PushTextWrapPos(wrap_pos_x);
  ImGui::TextUnformatted(message.content.c_str());
  ImGui::PopTextWrapPos();

  ImGui::SetCursorScreenPos(ImVec2(min.x + pad_x, max.y - pad_y - meta_h));
  ImGui::TextColored(ui::kTextMuted, "%s", message.created_at.c_str());

  ImGui::SetCursorScreenPos(ImVec2(cursor.x, max.y + ui::kSpace16));
  ImGui::Dummy(ImVec2(content_width, 0.0f));
}

static void DrawCenterModeToggle(AppState& app) {
  ImGui::TextColored(ui::kTextMuted, "Mode");
  ImGui::SameLine();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
  const bool structured_active = (app.center_view_mode == CenterViewMode::Structured);
  if (DrawButton("Structured", ImVec2(104.0f, 30.0f), structured_active ? ButtonKind::Primary : ButtonKind::Ghost)) {
    if (!structured_active) {
      StopAllCliTerminals(app, false);
    }
    app.center_view_mode = CenterViewMode::Structured;
    SaveSettings(app);
  }
  ImGui::SameLine();
  const bool cli_active = (app.center_view_mode == CenterViewMode::CliConsole);
  if (DrawButton("Terminal", ImVec2(92.0f, 30.0f), cli_active ? ButtonKind::Primary : ButtonKind::Ghost)) {
    app.center_view_mode = CenterViewMode::CliConsole;
    MarkSelectedCliTerminalForLaunch(app);
    SaveSettings(app);
  }
  ImGui::PopStyleVar();
}

static VTermModifier ActiveVTermModifiers() {
  VTermModifier mod = VTERM_MOD_NONE;
  ImGuiIO& io = ImGui::GetIO();
  if (io.KeyCtrl) {
    mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
  }
  if (io.KeyShift) {
    mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
  }
  if (io.KeyAlt) {
    mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);
  }
  return mod;
}

static void FeedCliTerminalKeyboard(CliTerminalState& terminal) {
  if (terminal.vt == nullptr) {
    return;
  }
#if defined(_WIN32)
  if (terminal.pipe_input == INVALID_HANDLE_VALUE) {
    return;
  }
#else
  if (terminal.master_fd < 0) {
    return;
  }
#endif
  ImGuiIO& io = ImGui::GetIO();
  const VTermModifier mod = ActiveVTermModifiers();

  auto send_key = [&](const ImGuiKey imgui_key, const VTermKey vterm_key) {
    if (ImGui::IsKeyPressed(imgui_key, false)) {
      vterm_keyboard_key(terminal.vt, vterm_key, mod);
    }
  };

  send_key(ImGuiKey_Enter, VTERM_KEY_ENTER);
  send_key(ImGuiKey_KeypadEnter, VTERM_KEY_ENTER);
  send_key(ImGuiKey_Tab, VTERM_KEY_TAB);
  send_key(ImGuiKey_Backspace, VTERM_KEY_BACKSPACE);
  send_key(ImGuiKey_Escape, VTERM_KEY_ESCAPE);
  send_key(ImGuiKey_UpArrow, VTERM_KEY_UP);
  send_key(ImGuiKey_DownArrow, VTERM_KEY_DOWN);
  send_key(ImGuiKey_LeftArrow, VTERM_KEY_LEFT);
  send_key(ImGuiKey_RightArrow, VTERM_KEY_RIGHT);
  send_key(ImGuiKey_Home, VTERM_KEY_HOME);
  send_key(ImGuiKey_End, VTERM_KEY_END);
  send_key(ImGuiKey_PageUp, VTERM_KEY_PAGEUP);
  send_key(ImGuiKey_PageDown, VTERM_KEY_PAGEDOWN);
  send_key(ImGuiKey_Delete, VTERM_KEY_DEL);
  send_key(ImGuiKey_Insert, VTERM_KEY_INS);
  send_key(ImGuiKey_F1, static_cast<VTermKey>(VTERM_KEY_FUNCTION(1)));
  send_key(ImGuiKey_F2, static_cast<VTermKey>(VTERM_KEY_FUNCTION(2)));
  send_key(ImGuiKey_F3, static_cast<VTermKey>(VTERM_KEY_FUNCTION(3)));
  send_key(ImGuiKey_F4, static_cast<VTermKey>(VTERM_KEY_FUNCTION(4)));
  send_key(ImGuiKey_F5, static_cast<VTermKey>(VTERM_KEY_FUNCTION(5)));
  send_key(ImGuiKey_F6, static_cast<VTermKey>(VTERM_KEY_FUNCTION(6)));
  send_key(ImGuiKey_F7, static_cast<VTermKey>(VTERM_KEY_FUNCTION(7)));
  send_key(ImGuiKey_F8, static_cast<VTermKey>(VTERM_KEY_FUNCTION(8)));
  send_key(ImGuiKey_F9, static_cast<VTermKey>(VTERM_KEY_FUNCTION(9)));
  send_key(ImGuiKey_F10, static_cast<VTermKey>(VTERM_KEY_FUNCTION(10)));
  send_key(ImGuiKey_F11, static_cast<VTermKey>(VTERM_KEY_FUNCTION(11)));
  send_key(ImGuiKey_F12, static_cast<VTermKey>(VTERM_KEY_FUNCTION(12)));

  auto send_char = [&](const ImGuiKey imgui_key, const uint32_t ch) {
    if (ImGui::IsKeyPressed(imgui_key, false)) {
      vterm_keyboard_unichar(terminal.vt, ch, mod);
    }
  };

  if ((mod & (VTERM_MOD_CTRL | VTERM_MOD_ALT)) != 0) {
    send_char(ImGuiKey_A, 'a');
    send_char(ImGuiKey_B, 'b');
    send_char(ImGuiKey_C, 'c');
    send_char(ImGuiKey_D, 'd');
    send_char(ImGuiKey_E, 'e');
    send_char(ImGuiKey_F, 'f');
    send_char(ImGuiKey_G, 'g');
    send_char(ImGuiKey_H, 'h');
    send_char(ImGuiKey_I, 'i');
    send_char(ImGuiKey_J, 'j');
    send_char(ImGuiKey_K, 'k');
    send_char(ImGuiKey_L, 'l');
    send_char(ImGuiKey_M, 'm');
    send_char(ImGuiKey_N, 'n');
    send_char(ImGuiKey_O, 'o');
    send_char(ImGuiKey_P, 'p');
    send_char(ImGuiKey_Q, 'q');
    send_char(ImGuiKey_R, 'r');
    send_char(ImGuiKey_S, 's');
    send_char(ImGuiKey_T, 't');
    send_char(ImGuiKey_U, 'u');
    send_char(ImGuiKey_V, 'v');
    send_char(ImGuiKey_W, 'w');
    send_char(ImGuiKey_X, 'x');
    send_char(ImGuiKey_Y, 'y');
    send_char(ImGuiKey_Z, 'z');
    send_char(ImGuiKey_0, '0');
    send_char(ImGuiKey_1, '1');
    send_char(ImGuiKey_2, '2');
    send_char(ImGuiKey_3, '3');
    send_char(ImGuiKey_4, '4');
    send_char(ImGuiKey_5, '5');
    send_char(ImGuiKey_6, '6');
    send_char(ImGuiKey_7, '7');
    send_char(ImGuiKey_8, '8');
    send_char(ImGuiKey_9, '9');
    send_char(ImGuiKey_Space, ' ');
    send_char(ImGuiKey_Minus, '-');
    send_char(ImGuiKey_Equal, '=');
    send_char(ImGuiKey_LeftBracket, '[');
    send_char(ImGuiKey_RightBracket, ']');
    send_char(ImGuiKey_Backslash, '\\');
    send_char(ImGuiKey_Semicolon, ';');
    send_char(ImGuiKey_Apostrophe, '\'');
    send_char(ImGuiKey_Comma, ',');
    send_char(ImGuiKey_Period, '.');
    send_char(ImGuiKey_Slash, '/');
    send_char(ImGuiKey_GraveAccent, '`');
  }

  for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
    const ImWchar ch = io.InputQueueCharacters[i];
    if (ch >= 32 && ch != 127) {
      vterm_keyboard_unichar(terminal.vt, static_cast<uint32_t>(ch), VTERM_MOD_NONE);
    }
  }
}

static void DrawCliTerminalSurface(AppState& app, ChatSession& chat) {
  CliTerminalState& terminal = EnsureCliTerminalForChat(app, chat);
  const ImFont* terminal_font = (g_font_mono_readable != nullptr) ? g_font_mono_readable : g_font_mono;
  const float terminal_cell_width_scale = uam::TerminalCellWidthScale();
  if (!terminal.running && terminal.should_launch) {
    PushFontIfAvailable(terminal_font);
    const float mono_h = (terminal_font != nullptr ? terminal_font->FontSize : ImGui::GetTextLineHeight());
    const float mono_w = std::max(7.0f, ImGui::CalcTextSize("W").x * terminal_cell_width_scale);
    PopFontIfAvailable(terminal_font);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int rows = ClampTerminalRows(static_cast<int>(avail.y / mono_h) - 1);
    const int cols = ClampTerminalCols(static_cast<int>(avail.x / mono_w) - 1);
    if (!StartCliTerminalForChat(app, terminal, chat, rows, cols)) {
      ImGui::TextColored(ui::kError, "Failed to start Gemini terminal.");
      if (!terminal.last_error.empty()) {
        ImGui::TextColored(ui::kTextMuted, "%s", terminal.last_error.c_str());
      }
      return;
    }
  }
  if (!terminal.running) {
    ImGui::TextColored(ui::kWarning, "Gemini terminal is stopped.");
    if (DrawButton("Restart Terminal", ImVec2(130.0f, 34.0f), ButtonKind::Primary)) {
      terminal.should_launch = true;
    }
    if (!terminal.last_error.empty()) {
      ImGui::TextColored(ui::kTextMuted, "%s", terminal.last_error.c_str());
    }
    return;
  }

  PushFontIfAvailable(terminal_font);
  const float cell_h = (terminal_font != nullptr) ? terminal_font->FontSize + 1.0f : ImGui::GetTextLineHeight();
  const float cell_w = std::max(7.0f, ImGui::CalcTextSize("W").x * terminal_cell_width_scale);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const int rows = ClampTerminalRows(static_cast<int>(avail.y / cell_h));
  const int cols = ClampTerminalCols(static_cast<int>(avail.x / cell_w));
  ResizeCliTerminal(terminal, rows, cols);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 terminal_size(cell_w * terminal.cols, cell_h * terminal.rows);
  ImGui::InvisibleButton("embedded_cli_surface", terminal_size, ImGuiButtonFlags_None);
  const bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive() || ImGui::IsItemHovered();
  if (focused) {
    FeedCliTerminalKeyboard(terminal);
  }

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(origin.x + 1.0f, origin.y + 3.0f), ImVec2(origin.x + terminal_size.x + 1.0f, origin.y + terminal_size.y + 3.0f),
                      ImGui::GetColorU32(ui::kShadow), 10.0f);
  draw->AddRectFilled(origin, ImVec2(origin.x + terminal_size.x, origin.y + terminal_size.y), ImGui::GetColorU32(ui::kInputSurface), 10.0f);
  draw->AddRect(origin, ImVec2(origin.x + terminal_size.x, origin.y + terminal_size.y), ImGui::GetColorU32(ui::kBorder), 10.0f);

  const double now = ImGui::GetTime();
  if (ShouldRefreshTerminalTextCache(terminal, now)) {
    RefreshCliTerminalTextCache(terminal);
    terminal.last_text_cache_refresh_s = now;
  }

  if (!terminal.rendered_rows.empty()) {
    draw->PushClipRect(origin, ImVec2(origin.x + terminal_size.x, origin.y + terminal_size.y), true);
    const ImU32 text_color = ImGui::GetColorU32(ui::kTextPrimary);
    const int cached_rows = std::min<int>(terminal.rows, static_cast<int>(terminal.rendered_rows.size()));
    for (int row = 0; row < cached_rows; ++row) {
      const std::string& line = terminal.rendered_rows[static_cast<std::size_t>(row)];
      if (line.empty()) {
        continue;
      }
      draw->AddText(ImVec2(origin.x, origin.y + row * cell_h), text_color, line.c_str());
    }
    draw->PopClipRect();
  }

  if (terminal.state != nullptr) {
    VTermPos cursor{0, 0};
    vterm_state_get_cursorpos(terminal.state, &cursor);
    if (cursor.row >= 0 && cursor.row < terminal.rows && cursor.col >= 0 && cursor.col < terminal.cols) {
      const ImVec2 cursor_min(origin.x + cursor.col * cell_w, origin.y + cursor.row * cell_h);
      const ImVec2 cursor_max(cursor_min.x + cell_w, cursor_min.y + cell_h);
      draw->AddRect(cursor_min, cursor_max, ImGui::GetColorU32(ui::kAccent), 0.0f, 0, 1.2f);
    }
  }

  PopFontIfAvailable(terminal_font);
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  ImGui::TextColored(ui::kTextMuted, "Native Gemini terminal for this chat (Ctrl+Y and other shortcuts pass through).");
}

static void DrawInputContainer(AppState& app) {
  BeginPanel("input_container", ImVec2(0.0f, 148.0f), PanelTone::Secondary, true, 0, ImVec2(12.0f, 12.0f), ui::kRadiusInput);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 min = ImGui::GetWindowPos();
  const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
  draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 4.0f), ImVec2(max.x + 2.0f, max.y + 4.0f), ImGui::GetColorU32(ui::kShadow), ui::kRadiusInput);

  ImGui::TextColored(ui::kTextMuted, "Message");
  PushInputChrome(ui::kRadiusInput);
  ImGui::InputTextMultiline("##composer", &app.composer_text, ImVec2(-96.0f, 92.0f), ImGuiInputTextFlags_AllowTabInput);
  PopInputChrome();

  ImGui::SameLine();
  const bool can_send = !app.pending_call.has_value();
  const std::string send_label = FrontendActionLabel(app, "send_prompt", "Send");
  if (!can_send) {
    ImGui::BeginDisabled();
  }
  if (FrontendActionVisible(app, "send_prompt", true)) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 28.0f);
    if (DrawButton(send_label.c_str(), ImVec2(80.0f, 40.0f), ButtonKind::Accent)) {
      StartGeminiRequest(app);
    }
  }
  if (!can_send) {
    ImGui::EndDisabled();
  }

  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      ImGui::IsKeyPressed(ImGuiKey_Enter) &&
      ImGui::GetIO().KeyCtrl &&
      !app.pending_call.has_value()) {
    StartGeminiRequest(app);
  }
  EndPanel();
}

static void DrawStructuredProcessingIndicator(AppState& app, const ChatSession& chat) {
  if (!app.pending_call.has_value() || app.pending_call->chat_id != chat.id) {
    return;
  }

  BeginPanel("structured_processing_indicator", ImVec2(0.0f, 72.0f), PanelTone::Secondary, true, 0,
             ImVec2(ui::kSpace12, 10.0f), ui::kRadiusPanel);

  const int dots = static_cast<int>(ImGui::GetTime() * 2.5) % 4;
  std::string status = "Gemini is processing";
  status.append(static_cast<std::size_t>(dots), '.');
  ImGui::TextColored(ui::kAccent, "%s", status.c_str());
  ImGui::TextColored(ui::kTextMuted, "You can keep browsing chats while this runs.");

  const ImVec2 bar_min = ImGui::GetCursorScreenPos();
  const float bar_w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
  const float bar_h = 6.0f;
  const ImVec2 bar_max(bar_min.x + bar_w, bar_min.y + bar_h);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(bar_min, bar_max, ImGui::GetColorU32(Rgb(255, 255, 255, 0.10f)), 6.0f);

  const float cycle = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.9f, 1.0f);
  const float seg_w = bar_w * 0.28f;
  const float seg_x = bar_min.x + (bar_w + seg_w) * cycle - seg_w;
  const ImVec2 seg_min(std::max(bar_min.x, seg_x), bar_min.y);
  const ImVec2 seg_max(std::min(bar_max.x, seg_x + seg_w), bar_max.y);
  if (seg_max.x > seg_min.x) {
    draw->AddRectFilled(seg_min, seg_max, ImGui::GetColorU32(ui::kAccent), 6.0f);
  }
  ImGui::Dummy(ImVec2(0.0f, bar_h + 6.0f));
  if (DrawButton("Stop Run", ImVec2(108.0f, 30.0f), ButtonKind::Ghost)) {
    app.pending_call.reset();
    app.status_line = "Stopped current structured run.";
  }
  EndPanel();
}

static void DrawChatDetailPane(AppState& app, ChatSession& chat) {
  BeginPanel("main_chat_panel", ImVec2(0.0f, 0.0f), PanelTone::Primary, true, 0, ImVec2(ui::kSpace24, ui::kSpace24));
  PushInputChrome();
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::InputText("##chat_title", &chat.title)) {
    chat.updated_at = TimestampNow();
    SaveAndUpdateStatus(app, chat, "Chat title updated.", "Chat title changed in UI, but failed to save.");
  }
  PopInputChrome();

  ImGui::TextColored(ui::kTextMuted, "Ctrl+Enter to send");
  ImGui::SameLine();
  if (app.pending_call.has_value() && app.pending_call->chat_id == chat.id) {
    static constexpr const char kSpinnerFrames[4] = {'|', '/', '-', '\\'};
    const int spinner_index = static_cast<int>(ImGui::GetTime() * 8.0) & 3;
    ImGui::TextColored(ui::kWarning, "Gemini running %c", kSpinnerFrames[spinner_index]);
  } else if (app.pending_call.has_value()) {
    ImGui::TextColored(ui::kTextSecondary, "Gemini running in another chat");
  } else {
    ImGui::TextColored(ui::kSuccess, "Ready");
  }
  ImGui::SameLine();
  DrawCenterModeToggle(app);
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));

  if (app.center_view_mode == CenterViewMode::Structured) {
    const float input_area_h = 176.0f;
    BeginPanel("conversation_history", ImVec2(0.0f, -input_area_h), PanelTone::Primary, false, ImGuiWindowFlags_AlwaysVerticalScrollbar, ImVec2(0.0f, 0.0f));
    const float content_width = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < static_cast<int>(chat.messages.size()); ++i) {
      ImGui::PushID(i);
      DrawMessageBubble(app, chat, i, content_width);
      ImGui::PopID();
    }
    if (app.scroll_to_bottom) {
      ImGui::SetScrollHereY(1.0f);
      app.scroll_to_bottom = false;
    }
    EndPanel();

    if (app.open_edit_message_popup) {
      ImGui::OpenPopup("edit_user_message_popup");
      app.open_edit_message_popup = false;
    }
    if (ImGui::BeginPopupModal("edit_user_message_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      const bool editing_selected_chat = (app.editing_chat_id == chat.id);
      const bool valid_index =
          (app.editing_message_index >= 0 && app.editing_message_index < static_cast<int>(chat.messages.size()));
      const bool valid_target = editing_selected_chat && valid_index && chat.messages[app.editing_message_index].role == MessageRole::User;
      if (!valid_target) {
        ImGui::TextColored(ui::kWarning, "The selected message is no longer editable.");
      } else {
        ImGui::TextColored(ui::kTextPrimary, "Edit message and continue from this point");
        ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
        PushInputChrome();
        ImGui::InputTextMultiline("##edited_user_message", &app.editing_message_text, ImVec2(560.0f, 170.0f));
        PopInputChrome();
        ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
        ImGui::TextColored(ui::kTextMuted, "This removes all messages after this point and re-runs Gemini.");
      }

      ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
      if (valid_target) {
        if (DrawButton("Apply + Continue", ImVec2(150.0f, 32.0f), ButtonKind::Primary)) {
          if (ContinueFromEditedUserMessage(app, chat)) {
            ClearEditMessageState(app);
            ImGui::CloseCurrentPopup();
          }
        }
        ImGui::SameLine();
      }
      if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
        ClearEditMessageState(app);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    DrawStructuredProcessingIndicator(app, chat);
    if (!app.settings.gemini_yolo_mode &&
        ProviderRuntime::SupportsGeminiJsonHistory(ActiveProviderOrDefault(app))) {
      ImGui::TextColored(ui::kWarning,
                         "Structured mode auto-enables --yolo. Use Terminal mode for approve/disapprove prompts.");
      ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    }
    DrawInputContainer(app);
  } else {
    BeginPanel("cli_terminal_panel", ImVec2(0.0f, 0.0f), PanelTone::Secondary, true, 0, ImVec2(12.0f, 12.0f));
    DrawCliTerminalSurface(app, chat);
    EndPanel();
  }
  if (!app.status_line.empty()) {
    ImGui::TextColored(ui::kTextSecondary, "%s", app.status_line.c_str());
  }
  EndPanel();
}

static void DrawSectionHeader(const char* title) {
  PushFontIfAvailable(g_font_ui);
  ImGui::TextColored(ui::kTextSecondary, "%s", title);
  PopFontIfAvailable(g_font_ui);
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
}

static bool BeginSectionCard(const char* id, const float height = 0.0f) {
  return BeginPanel(id, ImVec2(0.0f, height), PanelTone::Elevated, true, 0, ImVec2(ui::kSpace12, ui::kSpace12));
}

static void DrawSessionSidePane(AppState& app, ChatSession& chat) {
  BeginPanel("right_settings", ImVec2(0.0f, 0.0f), PanelTone::Secondary, true, 0, ImVec2(ui::kSpace16, ui::kSpace16));

  DrawSectionHeader("Chat Folder");
  if (BeginSectionCard("folder_card")) {
    ChatFolder* active_folder = FindFolderById(app, chat.folder_id);
    const char* active_label = (active_folder != nullptr) ? active_folder->title.c_str() : "(Unassigned)";
    if (ImGui::BeginCombo("Folder", active_label)) {
      for (const ChatFolder& folder : app.folders) {
        const bool selected = (chat.folder_id == folder.id);
        if (ImGui::Selectable(folder.title.c_str(), selected)) {
          chat.folder_id = folder.id;
          chat.updated_at = TimestampNow();
          SaveAndUpdateStatus(app, chat, "Chat moved to folder.", "Moved chat in UI, but failed to save.");
          active_folder = FindFolderById(app, chat.folder_id);
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    if (active_folder != nullptr) {
      PushInputChrome();
      ImGui::InputText("Folder Title", &active_folder->title);
      ImGui::InputText("Folder Dir", &active_folder->directory);
      PopInputChrome();
      if (DrawButton("Save Folder", ImVec2(110.0f, 32.0f), ButtonKind::Ghost)) {
        SaveFolders(app);
        app.status_line = "Folder settings saved.";
      }
      ImGui::SameLine();
      const bool can_delete_folder = (active_folder->id != kDefaultFolderId);
      if (!can_delete_folder) {
        ImGui::BeginDisabled();
      }
      if (DrawButton("Delete Folder", ImVec2(118.0f, 32.0f), ButtonKind::Ghost)) {
        const std::string folder_id_to_delete = active_folder->id;
        bool all_chat_saves_ok = true;
        int moved_chat_count = 0;
        for (ChatSession& existing_chat : app.chats) {
          if (existing_chat.folder_id != folder_id_to_delete) {
            continue;
          }
          existing_chat.folder_id = kDefaultFolderId;
          if (!SaveChat(app, existing_chat)) {
            all_chat_saves_ok = false;
          }
          ++moved_chat_count;
        }
        if (chat.folder_id == folder_id_to_delete) {
          chat.folder_id = kDefaultFolderId;
        }
        const int folder_index = FindFolderIndexById(app, folder_id_to_delete);
        if (folder_index >= 0) {
          app.folders.erase(app.folders.begin() + folder_index);
        }
        EnsureDefaultFolder(app);
        if (app.new_chat_folder_id == folder_id_to_delete) {
          app.new_chat_folder_id = kDefaultFolderId;
        }
        SaveFolders(app);
        if (all_chat_saves_ok) {
          app.status_line = "Folder deleted. Moved " + std::to_string(moved_chat_count) + " chat(s) to General.";
        } else {
          app.status_line = "Folder deleted, but failed to persist one or more moved chats.";
        }
      }
      if (!can_delete_folder) {
        ImGui::EndDisabled();
      }
    }
  }
  EndPanel();

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
  DrawSectionHeader("Workspace Files");
  if (BeginSectionCard("workspace_card")) {
    int remove_index = -1;
    for (int i = 0; i < static_cast<int>(chat.linked_files.size()); ++i) {
      ImGui::PushID(i);
      ImGui::TextWrapped("%s", chat.linked_files[i].c_str());
      if (DrawButton("Detach", ImVec2(80.0f, 28.0f), ButtonKind::Ghost)) {
        remove_index = i;
      }
      DrawSoftDivider();
      ImGui::PopID();
    }
    if (remove_index >= 0) {
      chat.linked_files.erase(chat.linked_files.begin() + remove_index);
      chat.updated_at = TimestampNow();
      SaveAndUpdateStatus(app, chat, "File detached from chat.", "Detached file in UI, but failed to save chat.");
    }

    PushInputChrome();
    ImGui::InputText("File Path", &app.attach_file_input);
    PopInputChrome();

    if (DrawButton("Attach File", ImVec2(100.0f, 34.0f), ButtonKind::Ghost)) {
      const std::string path_text = Trim(app.attach_file_input);
      if (path_text.empty()) {
        app.status_line = "Provide a file path to attach.";
      } else if (!fs::exists(path_text)) {
        app.status_line = "Path does not exist: " + path_text;
      } else if (!fs::is_regular_file(path_text)) {
        app.status_line = "Path is not a regular file: " + path_text;
      } else {
        if (std::find(chat.linked_files.begin(), chat.linked_files.end(), path_text) == chat.linked_files.end()) {
          chat.linked_files.push_back(path_text);
          chat.updated_at = TimestampNow();
          SaveAndUpdateStatus(app, chat, "File attached.", "Attached file in UI, but failed to save chat.");
        } else {
          app.status_line = "File already attached to this chat.";
        }
        app.attach_file_input.clear();
      }
    }
  }
  EndPanel();

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
  DrawSectionHeader("Provider Runtime");
  if (BeginSectionCard("runtime_card")) {
    ImGui::TextColored(ui::kTextMuted, "Template placeholders: {prompt} {files} {resume} {flags}");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    ProviderProfile* active_provider = ActiveProvider(app);
    const char* active_provider_label =
        (active_provider != nullptr && !Trim(active_provider->title).empty()) ? active_provider->title.c_str() : "Gemini CLI";
    if (ImGui::BeginCombo("Provider", active_provider_label)) {
      for (const ProviderProfile& profile : app.provider_profiles) {
        const bool selected = (profile.id == app.settings.active_provider_id);
        const std::string provider_title = Trim(profile.title).empty() ? profile.id : profile.title;
        if (ImGui::Selectable(provider_title.c_str(), selected)) {
          app.settings.active_provider_id = profile.id;
          if (const ProviderProfile* selected_profile = ActiveProvider(app); selected_profile != nullptr) {
            app.settings.gemini_command_template = selected_profile->command_template;
          }
          RefreshChatHistory(app);
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    ImGui::Checkbox("YOLO mode (Ctrl+Y / Shift+Tab)", &app.settings.gemini_yolo_mode);
    PushInputChrome();
    ImGui::InputText("Extra Flags", &app.settings.gemini_extra_flags);
    PopInputChrome();

    PushInputChrome();
    ImGui::InputTextMultiline("Command Template", &app.settings.gemini_command_template, ImVec2(-1.0f, 88.0f));
    PopInputChrome();

    if (DrawButton("Save Settings", ImVec2(120.0f, 34.0f), ButtonKind::Primary)) {
      if (ProviderProfile* selected_profile = ActiveProvider(app); selected_profile != nullptr) {
        selected_profile->command_template = app.settings.gemini_command_template;
      }
      SaveSettings(app);
      SaveProviders(app);
      app.status_line = "Provider settings saved.";
    }

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
    ImGui::TextColored(ui::kTextMuted, "Profiles file: %s", ProviderProfileFilePath(app).string().c_str());
    ImGui::TextColored(ui::kTextMuted, "Action map file: %s", FrontendActionFilePath(app).string().c_str());
  }
  EndPanel();

  if (app.pending_call.has_value()) {
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
    DrawSectionHeader("Current Command");
    if (BeginSectionCard("command_preview_card", 128.0f)) {
      PushFontIfAvailable(g_font_mono);
      PushInputChrome();
      ImGui::InputTextMultiline("##cmd_preview", &app.pending_call->command_preview, ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
      PopInputChrome();
      PopFontIfAvailable(g_font_mono);
    }
    EndPanel();
  }

  EndPanel();
}

int main(int, char**) {
  AppState app;
  std::vector<fs::path> data_root_candidates;
  if (const char* data_dir_env = std::getenv("UAM_DATA_DIR")) {
    const std::string env_root = Trim(data_dir_env);
    if (!env_root.empty()) {
      data_root_candidates.push_back(fs::path(env_root));
    }
  }
  if (data_root_candidates.empty()) {
    std::error_code cwd_ec;
    const fs::path cwd = fs::current_path(cwd_ec);
    if (!cwd_ec) {
      data_root_candidates.push_back(cwd / "data");
    }
    data_root_candidates.push_back(DefaultDataRootPath());
  }
  data_root_candidates.push_back(TempFallbackDataRootPath());

  std::unordered_set<std::string> tried_roots;
  std::string last_data_root_error = "Unknown data directory initialization failure.";
  bool initialized_data_root = false;
  for (const fs::path& candidate_root : data_root_candidates) {
    if (candidate_root.empty()) {
      continue;
    }
    const std::string key = candidate_root.lexically_normal().string();
    if (tried_roots.find(key) != tried_roots.end()) {
      continue;
    }
    tried_roots.insert(key);

    std::string error;
    if (EnsureDataRootLayout(candidate_root, &error)) {
      app.data_root = candidate_root;
      initialized_data_root = true;
      break;
    }
    last_data_root_error = std::move(error);
  }

  if (!initialized_data_root) {
    std::fprintf(stderr, "Failed to initialize application data directories: %s\n", last_data_root_error.c_str());
    return 1;
  }
  LoadSettings(app);
  const bool had_provider_file = fs::exists(ProviderProfileFilePath(app));
  app.provider_profiles = ProviderProfileStore::Load(app.data_root);
  ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
  if (ActiveProvider(app) == nullptr && !app.provider_profiles.empty()) {
    app.settings.active_provider_id = app.provider_profiles.front().id;
  }
  if (ProviderProfile* active_profile = ActiveProvider(app); active_profile != nullptr) {
    if (!had_provider_file && !app.settings.gemini_command_template.empty()) {
      active_profile->command_template = app.settings.gemini_command_template;
    }
    app.settings.gemini_command_template = active_profile->command_template;
  }
  if (!had_provider_file) {
    SaveProviders(app);
  }
  LoadFrontendActions(app);
  app.folders = ChatFolderStore::Load(app.data_root);
  EnsureDefaultFolder(app);
  SaveFolders(app);
  if (ActiveProviderUsesGeminiHistory(app)) {
    RefreshGeminiChatsDir(app);
    app.chats = LoadChats(app);
    NormalizeChatFolderAssignments(app);
    BeginDeferredNativeHistoryLoad(app);
    if (app.gemini_chats_dir.empty()) {
      app.status_line = "Gemini native session directory not found yet. Run Gemini CLI in this project once.";
    } else if (app.deferred_native_history_loading) {
      app.status_line = "Loading native Gemini history in background...";
    }
  } else {
    app.chats = LoadChats(app);
    NormalizeChatFolderAssignments(app);
  }
  if (!app.chats.empty()) {
    app.selected_chat_index = 0;
  }
  // Avoid auto-starting terminals on startup; this keeps launch responsive even
  // in large Gemini histories and lets the user opt in per chat.

#if defined(_WIN32)
  SetProcessDPIAware();
#endif

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

#if defined(__APPLE__)
  const char* glsl_version = "#version 150";
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
  const char* glsl_version = "#version 130";
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  SDL_Window* window = SDL_CreateWindow(
      "Universal Agent Manager",
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      1440,
      860,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (window == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (gl_context == nullptr) {
    std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  const float ui_scale = DetectUiScale(window);
  ConfigureFonts(io, ui_scale);
  ApplyModernTheme();
  if (ui_scale > 1.01f) {
    ImGui::GetStyle().ScaleAllSizes(ui_scale);
  }

  ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL3_Init(glsl_version);

  bool done = false;
  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) {
        done = true;
      }
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(window)) {
        done = true;
      }
    }

    PollPendingGeminiCall(app);
    PollDeferredNativeHistoryLoad(app);
    PollAllCliTerminals(app);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    HandleGlobalHotkeys(app);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    DrawAmbientBackdrop(viewport->Pos, viewport->Size, static_cast<float>(ImGui::GetTime()));

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y));

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("UAM Gemini Manager", nullptr, window_flags);
    if (ImGui::BeginTable("layout_split", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("Chats", ImGuiTableColumnFlags_WidthFixed, ui::kSidebarWidth);
      ImGui::TableSetupColumn("Conversation", ImGuiTableColumnFlags_WidthStretch, 0.72f);
      ImGui::TableSetupColumn("Workspace", ImGuiTableColumnFlags_WidthFixed, ui::kRightPanelWidth);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      DrawLeftPane(app);

      ChatSession* selected = SelectedChat(app);
      ImGui::TableSetColumnIndex(1);
      if (selected == nullptr) {
        BeginPanel("empty_main", ImVec2(0.0f, 0.0f), PanelTone::Primary, true, 0, ImVec2(ui::kSpace24, ui::kSpace24));
        ImGui::TextWrapped("No chat selected. Create one from the left panel.");
        EndPanel();
      } else {
        DrawChatDetailPane(app, *selected);
      }

      ImGui::TableSetColumnIndex(2);
      if (selected != nullptr) {
        DrawSessionSidePane(app, *selected);
      } else {
        BeginPanel("empty_right", ImVec2(0.0f, 0.0f), PanelTone::Secondary, true, 0, ImVec2(ui::kSpace16, ui::kSpace16));
        ImGui::TextDisabled("Select a chat to view workspace settings.");
        EndPanel();
      }

      ImGui::EndTable();
    }

    ImGui::End();

    ImGui::Render();
    int display_w = 0;
    int display_h = 0;
    SDL_GL_GetDrawableSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(ui::kMainBackground.x, ui::kMainBackground.y, ui::kMainBackground.z, ui::kMainBackground.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  app.pending_call.reset();
  StopAllCliTerminals(app, true);

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
