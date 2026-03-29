#include "ollama_engine/engine_factory.h"
#include "ollama_engine/engine_interface.h"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string Trim(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs) {
  return ToLowerAscii(lhs) == ToLowerAscii(rhs);
}

bool WriteTextFile(const fs::path& path, const std::string& text, std::string* error_out = nullptr) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create directory for file '" + path.string() + "': " + ec.message();
    }
    return false;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    if (error_out != nullptr) {
      *error_out = "Failed to open file for writing: " + path.string();
    }
    return false;
  }
  out << text;
  if (!out.good()) {
    if (error_out != nullptr) {
      *error_out = "Failed to write file: " + path.string();
    }
    return false;
  }
  return true;
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::int64_t UnixEpochSecondsNow() {
  const auto now = std::chrono::system_clock::now();
  return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

std::int64_t UnixEpochMillisecondsNow() {
  const auto now = std::chrono::system_clock::now();
  return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

std::string TimestampIso8601Now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  gmtime_s(&tm_snapshot, &tt);
#else
  gmtime_r(&tt, &tm_snapshot);
#endif
  std::ostringstream out;
  out << std::put_time(&tm_snapshot, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string RandomHex(std::size_t digits) {
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> nibble(0, 15);
  std::string out;
  out.reserve(digits);
  for (std::size_t i = 0; i < digits; ++i) {
    const int value = nibble(rng);
    out.push_back(static_cast<char>(value < 10 ? ('0' + value) : ('a' + (value - 10))));
  }
  return out;
}

std::string BuildId(const std::string& prefix) {
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  return prefix + "-" + std::to_string(now_ms) + "-" + RandomHex(8);
}

int CountApproxTokens(const std::string& text) {
  std::istringstream in(text);
  int count = 0;
  std::string token;
  while (in >> token) {
    ++count;
  }
  return std::max(1, count);
}

enum class JsonType {
  Null,
  Bool,
  Number,
  String,
  Array,
  Object
};

struct JsonValue {
  JsonType type = JsonType::Null;
  bool bool_value = false;
  double number_value = 0.0;
  std::string string_value;
  std::vector<JsonValue> array_value;
  std::map<std::string, JsonValue> object_value;

  const JsonValue* Find(const std::string& key) const {
    const auto it = object_value.find(key);
    return (it == object_value.end()) ? nullptr : &it->second;
  }

  JsonValue* FindMutable(const std::string& key) {
    const auto it = object_value.find(key);
    return (it == object_value.end()) ? nullptr : &it->second;
  }
};

JsonValue MakeJsonNull() {
  JsonValue out;
  out.type = JsonType::Null;
  return out;
}

JsonValue MakeJsonBool(const bool value) {
  JsonValue out;
  out.type = JsonType::Bool;
  out.bool_value = value;
  return out;
}

JsonValue MakeJsonNumber(const double value) {
  JsonValue out;
  out.type = JsonType::Number;
  out.number_value = value;
  return out;
}

JsonValue MakeJsonString(const std::string& value) {
  JsonValue out;
  out.type = JsonType::String;
  out.string_value = value;
  return out;
}

JsonValue MakeJsonArray() {
  JsonValue out;
  out.type = JsonType::Array;
  return out;
}

JsonValue MakeJsonObject() {
  JsonValue out;
  out.type = JsonType::Object;
  return out;
}

class JsonParser {
 public:
  explicit JsonParser(const std::string_view input) : input_(input) {}

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
      JsonValue out = MakeJsonString(ParseString());
      if (error_) {
        return {};
      }
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
    JsonValue out = MakeJsonObject();
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
      if (error_) {
        break;
      }
      if (!Consume(':')) {
        error_ = true;
        break;
      }
      JsonValue value = ParseValue();
      if (error_) {
        break;
      }
      out.object_value[key] = std::move(value);
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
    JsonValue out = MakeJsonArray();
    if (!Consume('[')) {
      error_ = true;
      return {};
    }
    SkipWhitespace();
    if (Consume(']')) {
      return out;
    }
    while (!error_) {
      JsonValue value = ParseValue();
      if (error_) {
        break;
      }
      out.array_value.push_back(std::move(value));
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
    if (MatchLiteral("true")) {
      return MakeJsonBool(true);
    }
    if (MatchLiteral("false")) {
      return MakeJsonBool(false);
    }
    error_ = true;
    return {};
  }

  JsonValue ParseNull() {
    if (!MatchLiteral("null")) {
      error_ = true;
      return {};
    }
    return MakeJsonNull();
  }

  JsonValue ParseNumber() {
    JsonValue out = MakeJsonNumber(0.0);
    const std::size_t start = pos_;
    if (Peek() == '-') {
      ++pos_;
    }
    while (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
      ++pos_;
    }
    if (Peek() == '.') {
      ++pos_;
      while (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
        ++pos_;
      }
    }
    if (Peek() == 'e' || Peek() == 'E') {
      ++pos_;
      if (Peek() == '+' || Peek() == '-') {
        ++pos_;
      }
      while (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
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
      return {};
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
      if (ch != '\\') {
        out.push_back(ch);
        continue;
      }
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
        case 'u': {
          if (pos_ + 4 > input_.size()) {
            error_ = true;
            return {};
          }
          out += "\\u";
          out.append(input_.substr(pos_, 4));
          pos_ += 4;
          break;
        }
        default:
          error_ = true;
          return {};
      }
    }
    error_ = true;
    return {};
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

  bool Consume(const char ch) {
    SkipWhitespace();
    if (pos_ < input_.size() && input_[pos_] == ch) {
      ++pos_;
      return true;
    }
    return false;
  }

  char Peek() const {
    return (pos_ < input_.size()) ? input_[pos_] : '\0';
  }

  void SkipWhitespace() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_])) != 0) {
      ++pos_;
    }
  }

  std::string_view input_;
  std::size_t pos_ = 0;
  bool error_ = false;
};

std::optional<JsonValue> ParseJson(const std::string& text) {
  JsonParser parser(text);
  return parser.Parse();
}

void AppendEscapedJsonString(const std::string& value, std::string& out) {
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

void AppendJsonCompact(const JsonValue& value, std::string& out) {
  switch (value.type) {
    case JsonType::Null:
      out += "null";
      return;
    case JsonType::Bool:
      out += value.bool_value ? "true" : "false";
      return;
    case JsonType::Number: {
      std::ostringstream number;
      number << std::setprecision(15) << value.number_value;
      out += number.str();
      return;
    }
    case JsonType::String:
      AppendEscapedJsonString(value.string_value, out);
      return;
    case JsonType::Array: {
      out.push_back('[');
      for (std::size_t i = 0; i < value.array_value.size(); ++i) {
        if (i > 0) {
          out.push_back(',');
        }
        AppendJsonCompact(value.array_value[i], out);
      }
      out.push_back(']');
      return;
    }
    case JsonType::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& pair : value.object_value) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        AppendEscapedJsonString(pair.first, out);
        out.push_back(':');
        AppendJsonCompact(pair.second, out);
      }
      out.push_back('}');
      return;
    }
  }
}

std::string SerializeJsonCompact(const JsonValue& value) {
  std::string out;
  AppendJsonCompact(value, out);
  return out;
}

std::string JsonStringOrEmpty(const JsonValue* value) {
  if (value == nullptr || value->type != JsonType::String) {
    return "";
  }
  return value->string_value;
}

bool JsonBoolOrDefault(const JsonValue* value, const bool default_value) {
  if (value == nullptr || value->type != JsonType::Bool) {
    return default_value;
  }
  return value->bool_value;
}

int JsonIntOrDefault(const JsonValue* value, const int default_value) {
  if (value == nullptr || value->type != JsonType::Number) {
    return default_value;
  }
  if (value->number_value < static_cast<double>(std::numeric_limits<int>::min()) ||
      value->number_value > static_cast<double>(std::numeric_limits<int>::max())) {
    return default_value;
  }
  return static_cast<int>(value->number_value);
}

std::string ExtractContentText(const JsonValue* content) {
  if (content == nullptr) {
    return "";
  }
  if (content->type == JsonType::String) {
    return content->string_value;
  }
  if (content->type == JsonType::Object) {
    const JsonValue* text = content->Find("text");
    return (text != nullptr && text->type == JsonType::String) ? text->string_value : "";
  }
  if (content->type != JsonType::Array) {
    return "";
  }
  std::ostringstream out;
  bool first = true;
  for (const JsonValue& item : content->array_value) {
    std::string piece;
    if (item.type == JsonType::String) {
      piece = item.string_value;
    } else if (item.type == JsonType::Object) {
      const std::string item_type = ToLowerAscii(JsonStringOrEmpty(item.Find("type")));
      if (item_type.empty() || item_type == "text") {
        piece = JsonStringOrEmpty(item.Find("text"));
      }
    }
    piece = Trim(piece);
    if (piece.empty()) {
      continue;
    }
    if (!first) {
      out << "\n";
    }
    out << piece;
    first = false;
  }
  return out.str();
}

struct ToolDefinition {
  std::string name;
  std::string description;
  JsonValue parameters = MakeJsonObject();
};

std::vector<ToolDefinition> ParseToolDefinitions(const JsonValue* tools) {
  std::vector<ToolDefinition> out;
  if (tools == nullptr || tools->type != JsonType::Array) {
    return out;
  }
  out.reserve(tools->array_value.size());
  for (const JsonValue& entry : tools->array_value) {
    if (entry.type != JsonType::Object) {
      continue;
    }
    const JsonValue* function = entry.Find("function");
    if (function == nullptr || function->type != JsonType::Object) {
      continue;
    }
    ToolDefinition tool;
    tool.name = Trim(JsonStringOrEmpty(function->Find("name")));
    tool.description = Trim(JsonStringOrEmpty(function->Find("description")));
    if (tool.name.empty()) {
      continue;
    }
    if (const JsonValue* parameters = function->Find("parameters"); parameters != nullptr) {
      tool.parameters = *parameters;
    }
    out.push_back(std::move(tool));
  }
  return out;
}

std::string BuildTranscriptPrompt(const JsonValue& messages) {
  std::ostringstream out;
  const std::size_t total_messages = messages.array_value.size();
  constexpr std::size_t kMaxMessages = 14;
  const std::size_t start_index = (total_messages > kMaxMessages) ? (total_messages - kMaxMessages) : 0;

  for (std::size_t index = start_index; index < total_messages; ++index) {
    const JsonValue& entry = messages.array_value[index];
    if (entry.type != JsonType::Object) {
      continue;
    }
    std::string role = Trim(JsonStringOrEmpty(entry.Find("role")));
    if (role.empty()) {
      role = "user";
    }
    const std::string role_lower = ToLowerAscii(role);

    std::string content = Trim(ExtractContentText(entry.Find("content")));
    if (content.empty()) {
      const JsonValue* tool_calls = entry.Find("tool_calls");
      if (tool_calls != nullptr && tool_calls->type == JsonType::Array) {
        std::ostringstream synthesized;
        for (const JsonValue& call : tool_calls->array_value) {
          if (call.type != JsonType::Object) {
            continue;
          }
          const JsonValue* function = call.Find("function");
          if (function == nullptr || function->type != JsonType::Object) {
            continue;
          }
          const std::string name = Trim(JsonStringOrEmpty(function->Find("name")));
          const std::string arguments = Trim(JsonStringOrEmpty(function->Find("arguments")));
          if (name.empty() && arguments.empty()) {
            continue;
          }
          synthesized << "tool_call";
          if (!name.empty()) {
            synthesized << " name=" << name;
          }
          if (!arguments.empty()) {
            synthesized << " arguments=" << arguments;
          }
          synthesized << "\n";
        }
        content = Trim(synthesized.str());
      }
    }

    if (content.empty()) {
      continue;
    }
    const std::size_t per_message_limit = (role_lower == "system") ? 420 : 1400;
    if (content.size() > per_message_limit) {
      content = content.substr(0, per_message_limit) + "\n...[truncated]";
    }
    out << "[" << role << "] " << content << "\n";
  }
  std::string transcript = Trim(out.str());
  constexpr std::size_t kMaxTranscriptChars = 12000;
  if (transcript.size() > kMaxTranscriptChars) {
    const std::size_t cut = transcript.size() - kMaxTranscriptChars;
    const std::size_t next_newline = transcript.find('\n', cut);
    if (next_newline != std::string::npos && next_newline + 1 < transcript.size()) {
      transcript = transcript.substr(next_newline + 1);
    } else {
      transcript = transcript.substr(cut);
    }
  }
  return Trim(transcript);
}

std::string BuildToolEnvelopePrompt(const std::string& transcript, const std::vector<ToolDefinition>& tools) {
  std::ostringstream out;
  out << "You are a local tool-routing assistant.\n";
  out << "When tools are available, you MUST respond with strict JSON only.\n";
  out << "Allowed response envelope shapes:\n";
  out << "{\"type\":\"tool_call\",\"name\":\"<tool name>\",\"arguments\":{...}}\n";
  out << "{\"type\":\"text\",\"text\":\"<assistant text reply>\"}\n";
  out << "Do not include markdown fences and do not include extra keys.\n";
  out << "\nAvailable tools:\n";
  for (const ToolDefinition& tool : tools) {
    out << "- name: " << tool.name << "\n";
    if (!tool.description.empty()) {
      out << "  description: " << tool.description << "\n";
    }
    out << "  parameters: " << SerializeJsonCompact(tool.parameters) << "\n";
  }
  out << "\nConversation transcript:\n";
  out << transcript << "\n";
  out << "\nRespond with a single valid JSON object now.";
  return out.str();
}

std::string BuildToolEnvelopeRepairPrompt(const std::string& transcript,
                                          const std::vector<ToolDefinition>& tools,
                                          const std::string& invalid_output) {
  std::ostringstream out;
  out << "Your previous response was invalid JSON or incomplete.\n";
  out << "Return exactly ONE valid JSON object with no markdown.\n";
  out << "Allowed shapes only:\n";
  out << "{\"type\":\"tool_call\",\"name\":\"<tool name>\",\"arguments\":{...}}\n";
  out << "{\"type\":\"text\",\"text\":\"<assistant text reply>\"}\n";
  out << "\nAvailable tools:\n";
  for (const ToolDefinition& tool : tools) {
    out << "- name: " << tool.name << "\n";
    if (!tool.description.empty()) {
      out << "  description: " << tool.description << "\n";
    }
    out << "  parameters: " << SerializeJsonCompact(tool.parameters) << "\n";
  }
  out << "\nConversation transcript:\n";
  out << transcript << "\n";
  out << "\nInvalid previous output:\n";
  out << invalid_output << "\n";
  out << "\nReturn corrected JSON only.";
  return out.str();
}

std::string LatestUserMessageText(const JsonValue& messages) {
  if (messages.type != JsonType::Array) {
    return "";
  }
  for (auto it = messages.array_value.rbegin(); it != messages.array_value.rend(); ++it) {
    if (it->type != JsonType::Object) {
      continue;
    }
    const std::string role = ToLowerAscii(Trim(JsonStringOrEmpty(it->Find("role"))));
    if (role != "user") {
      continue;
    }
    const std::string content = Trim(ExtractContentText(it->Find("content")));
    if (!content.empty()) {
      return content;
    }
  }
  return "";
}

std::string StripMarkdownCodeFence(const std::string& text) {
  const std::string trimmed = Trim(text);
  if (trimmed.size() < 6 || trimmed.rfind("```", 0) != 0) {
    return trimmed;
  }
  const std::size_t first_newline = trimmed.find('\n');
  if (first_newline == std::string::npos) {
    return trimmed;
  }
  const std::size_t closing = trimmed.rfind("```");
  if (closing == std::string::npos || closing <= first_newline) {
    return trimmed;
  }
  return Trim(trimmed.substr(first_newline + 1, closing - first_newline - 1));
}

bool LooksLikeToolEnvelopeCandidate(const std::string& text) {
  const std::string candidate = Trim(StripMarkdownCodeFence(text));
  return !candidate.empty() && candidate.front() == '{';
}

std::string ReplaceAll(std::string value, const std::string& from, const std::string& to) {
  if (from.empty()) {
    return value;
  }
  std::size_t start = 0;
  while ((start = value.find(from, start)) != std::string::npos) {
    value.replace(start, from.size(), to);
    start += to.size();
  }
  return value;
}

std::optional<std::string> FindAllowedToolNameIgnoreCase(const std::set<std::string>& allowed_tool_names,
                                                         const std::string& desired_lower) {
  for (const std::string& candidate : allowed_tool_names) {
    if (ToLowerAscii(candidate) == desired_lower) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::string ShellSingleQuote(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (const char ch : value) {
    if (ch == '\'') {
      out += "'\"'\"'";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

std::string BuildHeuristicBashCommand(const std::string& user_request) {
  const std::string request = Trim(user_request);
  const std::string lower = ToLowerAscii(request);
  if (request.empty()) {
    return "pwd";
  }
  if ((lower.find("python") != std::string::npos || lower.find(".py") != std::string::npos) &&
      (lower.find("script") != std::string::npos || lower.find("file") != std::string::npos ||
       lower.find("run") != std::string::npos)) {
    return "cat > script.py << 'EOF'\nprint('Hello, World!')\nEOF\npython3 script.py";
  }
  if (lower.find("list") != std::string::npos &&
      (lower.find("file") != std::string::npos || lower.find("folder") != std::string::npos ||
       lower.find("directory") != std::string::npos)) {
    return "ls -la";
  }
  if (lower.find("current directory") != std::string::npos || lower.find("working directory") != std::string::npos ||
      lower == "pwd" || lower.find("where am i") != std::string::npos) {
    return "pwd";
  }
  if (lower.find("tree") != std::string::npos) {
    return "find . -maxdepth 2 -print";
  }
  if (lower.find("git status") != std::string::npos) {
    return "git status --short";
  }
  return "printf '%s\\n' " +
         ShellSingleQuote("Fallback: unable to derive safe executable command from request: " + request);
}

std::string NormalizeBashCommandString(const std::string& raw_command) {
  std::string command = Trim(StripMarkdownCodeFence(raw_command));
  if (command.rfind("$ ", 0) == 0) {
    command = Trim(command.substr(2));
  }
  if (command.rfind("echo ", 0) != 0) {
    return command;
  }

  std::string rest = Trim(command.substr(5));
  if (rest.size() < 2) {
    return command;
  }
  const char quote = rest.front();
  if (quote != '"' && quote != '\'') {
    return command;
  }

  std::size_t close_index = std::string::npos;
  bool escaped = false;
  for (std::size_t i = 1; i < rest.size(); ++i) {
    const char ch = rest[i];
    if (quote == '"' && ch == '\\' && !escaped) {
      escaped = true;
      continue;
    }
    if (ch == quote && !escaped) {
      close_index = i;
      break;
    }
    escaped = false;
  }
  if (close_index == std::string::npos) {
    return command;
  }

  std::string payload = rest.substr(1, close_index - 1);
  std::string remainder = Trim(rest.substr(close_index + 1));
  if (remainder.rfind(";", 0) == 0) {
    remainder = Trim(remainder.substr(1));
  }

  payload = ReplaceAll(payload, "\\n", "\n");
  payload = ReplaceAll(payload, "\\t", "\t");
  payload = ReplaceAll(payload, "\\\"", "\"");
  payload = ReplaceAll(payload, "\\'", "'");
  payload = ReplaceAll(payload, "\\\\", "\\");
  if (!remainder.empty()) {
    payload += "\n" + remainder;
  }
  payload = Trim(payload);
  const bool looks_like_script = payload.find('\n') != std::string::npos || payload.find("cat ") != std::string::npos ||
                                 payload.find("python") != std::string::npos || payload.find("node ") != std::string::npos ||
                                 payload.find("chmod ") != std::string::npos || payload.find("cd ") != std::string::npos;
  return looks_like_script ? payload : command;
}

std::string BuildBashRescuePrompt(const std::string& user_request, const std::string& previous_output) {
  std::ostringstream out;
  out << "You are preparing arguments for a bash tool call.\n";
  out << "Return ONLY one of the following:\n";
  out << "1) {\"command\":\"<shell command>\"}\n";
  out << "2) Raw shell command text\n";
  out << "Do not include markdown fences and do not include explanations.\n";
  out << "Never wrap commands with echo.\n";
  out << "Prefer a short, direct command sequence that can run as-is.\n";
  out << "\nUser request:\n";
  out << user_request << "\n";
  if (!Trim(previous_output).empty()) {
    out << "\nPrevious model output (may be invalid):\n";
    out << previous_output << "\n";
  }
  out << "\nReturn bash command now.";
  return out.str();
}

std::string ExtractBashCommandFromText(const std::string& raw_response) {
  const std::string stripped = Trim(StripMarkdownCodeFence(raw_response));
  if (stripped.empty()) {
    return "";
  }

  const std::optional<JsonValue> parsed = ParseJson(stripped);
  if (parsed.has_value() && parsed->type == JsonType::Object) {
    const JsonValue* command = parsed->Find("command");
    if (command != nullptr && command->type == JsonType::String) {
      return NormalizeBashCommandString(command->string_value);
    }
    const JsonValue* arguments = parsed->Find("arguments");
    if (arguments != nullptr) {
      if (arguments->type == JsonType::Object) {
        const JsonValue* nested_command = arguments->Find("command");
        if (nested_command != nullptr && nested_command->type == JsonType::String) {
          return NormalizeBashCommandString(nested_command->string_value);
        }
      } else if (arguments->type == JsonType::String) {
        const std::string nested = Trim(arguments->string_value);
        if (!nested.empty()) {
          if (const std::optional<JsonValue> nested_json = ParseJson(nested);
              nested_json.has_value() && nested_json->type == JsonType::Object) {
            const JsonValue* nested_command = nested_json->Find("command");
            if (nested_command != nullptr && nested_command->type == JsonType::String) {
              return NormalizeBashCommandString(nested_command->string_value);
            }
          }
          return NormalizeBashCommandString(nested);
        }
      }
    }
  }

  if (!stripped.empty() && (stripped.front() == '{' || stripped.front() == '[')) {
    return "";
  }
  return NormalizeBashCommandString(stripped);
}

struct ParsedToolEnvelope {
  bool is_tool_call = false;
  std::string tool_name;
  std::string tool_arguments_json;
  std::string text;
};

std::optional<ParsedToolEnvelope> SynthesizeFallbackToolCall(const std::set<std::string>& allowed_tool_names,
                                                             const std::string& user_request) {
  if (const std::optional<std::string> bash_tool = FindAllowedToolNameIgnoreCase(allowed_tool_names, "bash");
      bash_tool.has_value()) {
    ParsedToolEnvelope out;
    out.is_tool_call = true;
    out.tool_name = bash_tool.value();
    JsonValue args = MakeJsonObject();
    args.object_value["description"] = MakeJsonString("Execute fallback command derived from user request.");
    args.object_value["command"] = MakeJsonString(BuildHeuristicBashCommand(user_request));
    out.tool_arguments_json = SerializeJsonCompact(args);
    return out;
  }

  if (const std::optional<std::string> question_tool = FindAllowedToolNameIgnoreCase(allowed_tool_names, "question");
      question_tool.has_value()) {
    ParsedToolEnvelope out;
    out.is_tool_call = true;
    out.tool_name = question_tool.value();
    JsonValue args = MakeJsonObject();
    args.object_value["question"] =
        MakeJsonString("Please provide the exact command or concrete file changes to run.");
    out.tool_arguments_json = SerializeJsonCompact(args);
    return out;
  }

  if (!allowed_tool_names.empty()) {
    ParsedToolEnvelope out;
    out.is_tool_call = true;
    out.tool_name = *allowed_tool_names.begin();
    out.tool_arguments_json = "{}";
    return out;
  }

  return std::nullopt;
}

std::string BuildToolCallOnlyRepairPrompt(const std::string& transcript,
                                          const std::vector<ToolDefinition>& tools,
                                          const std::string& previous_output) {
  std::ostringstream out;
  out << "The user request requires action using available tools.\n";
  out << "You MUST return exactly one JSON object of this shape only:\n";
  out << "{\"type\":\"tool_call\",\"name\":\"<tool name>\",\"arguments\":{...}}\n";
  out << "Do not return type=text. Do not return markdown.\n";
  out << "\nAvailable tools:\n";
  for (const ToolDefinition& tool : tools) {
    out << "- name: " << tool.name << "\n";
    if (!tool.description.empty()) {
      out << "  description: " << tool.description << "\n";
    }
    out << "  parameters: " << SerializeJsonCompact(tool.parameters) << "\n";
  }
  out << "\nConversation transcript:\n";
  out << transcript << "\n";
  if (!Trim(previous_output).empty()) {
    out << "\nPrevious invalid output:\n";
    out << previous_output << "\n";
  }
  out << "\nReturn tool_call JSON only.";
  return out.str();
}

bool ToolListHasActionTools(const std::vector<ToolDefinition>& tools) {
  static const std::set<std::string> kActionTools = {"bash", "edit", "write", "task"};
  for (const ToolDefinition& tool : tools) {
    if (kActionTools.find(ToLowerAscii(Trim(tool.name))) != kActionTools.end()) {
      return true;
    }
  }
  return false;
}

std::string CollectUserTextLower(const JsonValue& messages) {
  if (messages.type != JsonType::Array) {
    return "";
  }
  std::ostringstream out;
  for (const JsonValue& entry : messages.array_value) {
    if (entry.type != JsonType::Object) {
      continue;
    }
    const std::string role = ToLowerAscii(Trim(JsonStringOrEmpty(entry.Find("role"))));
    if (role != "user") {
      continue;
    }
    const std::string content = ToLowerAscii(Trim(ExtractContentText(entry.Find("content"))));
    if (content.empty()) {
      continue;
    }
    out << content << "\n";
  }
  return out.str();
}

bool RequestLikelyNeedsToolCall(const JsonValue& messages, const std::vector<ToolDefinition>& tools) {
  if (!ToolListHasActionTools(tools)) {
    return false;
  }
  const std::string user_text = CollectUserTextLower(messages);
  if (user_text.empty()) {
    return false;
  }
  static const std::vector<std::string> kActionHints = {
      "make ",    "create ",  "write ",   "edit ",   "modify ", "update ",
      "run ",     "execute ", "script",   "file",    "folder",  "command",
      "terminal", "shell",    "python",   "save ",   "delete ", "rename "};
  for (const std::string& hint : kActionHints) {
    if (user_text.find(hint) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::optional<JsonValue> ParseJsonObjectCandidate(const std::string& text) {
  const std::string stripped = Trim(StripMarkdownCodeFence(text));
  if (stripped.empty()) {
    return std::nullopt;
  }
  if (const std::optional<JsonValue> full = ParseJson(stripped); full.has_value() && full->type == JsonType::Object) {
    return full;
  }
  const std::size_t first = stripped.find('{');
  const std::size_t last = stripped.rfind('}');
  if (first == std::string::npos || last == std::string::npos || last <= first) {
    return std::nullopt;
  }
  const std::string sliced = stripped.substr(first, last - first + 1);
  if (sliced == stripped) {
    return std::nullopt;
  }
  if (const std::optional<JsonValue> partial = ParseJson(sliced);
      partial.has_value() && partial->type == JsonType::Object) {
    return partial;
  }
  return std::nullopt;
}

JsonValue NormalizeToolArguments(const JsonValue* arguments, const std::string& tool_name) {
  auto apply_defaults = [&](JsonValue* value) {
    if (value == nullptr || value->type != JsonType::Object) {
      return;
    }
    if (ToLowerAscii(tool_name) != "bash") {
      return;
    }
    JsonValue* command = value->FindMutable("command");
    if ((command == nullptr || command->type != JsonType::String || Trim(command->string_value).empty())) {
      if (JsonValue* input = value->FindMutable("input"); input != nullptr && input->type == JsonType::String &&
                                                   !Trim(input->string_value).empty()) {
        value->object_value["command"] = MakeJsonString(NormalizeBashCommandString(input->string_value));
      }
      command = value->FindMutable("command");
    }
    if (command != nullptr && command->type == JsonType::String) {
      command->string_value = NormalizeBashCommandString(command->string_value);
    }
    JsonValue* description = value->FindMutable("description");
    if (description == nullptr || description->type != JsonType::String || Trim(description->string_value).empty()) {
      value->object_value["description"] = MakeJsonString("Execute requested shell command.");
    }
  };

  if (arguments == nullptr) {
    JsonValue out = MakeJsonObject();
    apply_defaults(&out);
    return out;
  }
  if (arguments->type == JsonType::Object) {
    JsonValue out = *arguments;
    apply_defaults(&out);
    return out;
  }
  if (arguments->type == JsonType::String) {
    const std::string raw = Trim(arguments->string_value);
    if (!raw.empty()) {
      if (const std::optional<JsonValue> parsed = ParseJson(raw);
          parsed.has_value() && parsed->type == JsonType::Object) {
        JsonValue out = parsed.value();
        apply_defaults(&out);
        return out;
      }
    }
    JsonValue wrapped = MakeJsonObject();
    if (ToLowerAscii(tool_name) == "bash") {
      wrapped.object_value["command"] = MakeJsonString(raw);
    } else {
      wrapped.object_value["input"] = MakeJsonString(raw);
    }
    apply_defaults(&wrapped);
    return wrapped;
  }

  JsonValue wrapped = MakeJsonObject();
  wrapped.object_value["value"] = *arguments;
  apply_defaults(&wrapped);
  return wrapped;
}

std::optional<ParsedToolEnvelope> ParseToolEnvelopeResponse(const std::string& raw_response,
                                                            const std::set<std::string>& allowed_tool_names) {
  const std::optional<JsonValue> parsed = ParseJsonObjectCandidate(raw_response);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  const JsonValue& root = parsed.value();
  const std::string type = ToLowerAscii(Trim(JsonStringOrEmpty(root.Find("type"))));
  if (type == "tool_call" || type == "tool") {
    const std::string tool_name = Trim(JsonStringOrEmpty(root.Find("name")));
    if (tool_name.empty()) {
      return std::nullopt;
    }
    if (!allowed_tool_names.empty() && allowed_tool_names.find(tool_name) == allowed_tool_names.end()) {
      return std::nullopt;
    }
    JsonValue arguments = NormalizeToolArguments(root.Find("arguments"), tool_name);
    ParsedToolEnvelope out;
    out.is_tool_call = true;
    out.tool_name = tool_name;
    out.tool_arguments_json = SerializeJsonCompact(arguments);
    return out;
  }
  if (type == "text" || type == "assistant") {
    const std::string text = Trim(JsonStringOrEmpty(root.Find("text")));
    if (text.empty()) {
      return std::nullopt;
    }
    ParsedToolEnvelope out;
    out.is_tool_call = false;
    out.text = text;
    return out;
  }

  auto parse_tool_with_name = [&](const std::string& tool_name) -> std::optional<ParsedToolEnvelope> {
    const std::string normalized_name = Trim(tool_name);
    if (normalized_name.empty()) {
      return std::nullopt;
    }
    if (!allowed_tool_names.empty() && allowed_tool_names.find(normalized_name) == allowed_tool_names.end()) {
      return std::nullopt;
    }
    JsonValue arguments = MakeJsonObject();
    if (const JsonValue* provided_arguments = root.Find("arguments"); provided_arguments != nullptr) {
      arguments = NormalizeToolArguments(provided_arguments, normalized_name);
    } else if (const JsonValue* provided_arguments = root.Find("args"); provided_arguments != nullptr) {
      arguments = NormalizeToolArguments(provided_arguments, normalized_name);
    }
    ParsedToolEnvelope out;
    out.is_tool_call = true;
    out.tool_name = normalized_name;
    out.tool_arguments_json = SerializeJsonCompact(arguments);
    return out;
  };

  if (const std::string direct_type = Trim(JsonStringOrEmpty(root.Find("type")));
      !direct_type.empty() &&
      (allowed_tool_names.empty() || allowed_tool_names.find(direct_type) != allowed_tool_names.end())) {
    if (const std::optional<ParsedToolEnvelope> parsed = parse_tool_with_name(direct_type); parsed.has_value()) {
      return parsed;
    }
  }
  if (const std::string tool_name = Trim(JsonStringOrEmpty(root.Find("tool"))); !tool_name.empty()) {
    if (const std::optional<ParsedToolEnvelope> parsed = parse_tool_with_name(tool_name); parsed.has_value()) {
      return parsed;
    }
  }
  if (const std::string tool_name = Trim(JsonStringOrEmpty(root.Find("name"))); !tool_name.empty()) {
    if (const std::optional<ParsedToolEnvelope> parsed = parse_tool_with_name(tool_name); parsed.has_value()) {
      return parsed;
    }
  }

  if (const JsonValue* tool_calls = root.Find("tool_calls");
      tool_calls != nullptr && tool_calls->type == JsonType::Array && !tool_calls->array_value.empty()) {
    const JsonValue& first = tool_calls->array_value.front();
    if (first.type == JsonType::Object) {
      const JsonValue* function = first.Find("function");
      std::string tool_name;
      const JsonValue* arguments_value = nullptr;
      if (function != nullptr && function->type == JsonType::Object) {
        tool_name = Trim(JsonStringOrEmpty(function->Find("name")));
        arguments_value = function->Find("arguments");
      } else {
        tool_name = Trim(JsonStringOrEmpty(first.Find("name")));
        if (tool_name.empty()) {
          tool_name = Trim(JsonStringOrEmpty(first.Find("tool")));
        }
        arguments_value = first.Find("arguments");
      }
      if (!tool_name.empty() &&
          (allowed_tool_names.empty() || allowed_tool_names.find(tool_name) != allowed_tool_names.end())) {
        ParsedToolEnvelope out;
        out.is_tool_call = true;
        out.tool_name = tool_name;
        out.tool_arguments_json = SerializeJsonCompact(NormalizeToolArguments(arguments_value, tool_name));
        return out;
      }
    }
  }

  return std::nullopt;
}

std::string NormalizeRequestedModel(const std::string& raw_model) {
  return Trim(raw_model);
}

std::string ModelBasename(const std::string& model_id) {
  if (model_id.empty()) {
    return "";
  }
  const std::size_t slash = model_id.find_last_of("/\\");
  if (slash == std::string::npos || slash + 1 >= model_id.size()) {
    return model_id;
  }
  return model_id.substr(slash + 1);
}

std::string ResolveRequestedModelAgainstAvailable(const std::string& raw_model,
                                                  const std::vector<std::string>& available_models) {
  const std::string requested = Trim(raw_model);
  if (requested.empty()) {
    return "";
  }

  if (std::find(available_models.begin(), available_models.end(), requested) != available_models.end()) {
    return requested;
  }

  // Some clients pass provider/model while others pass model only.
  const std::size_t first_slash = requested.find('/');
  if (first_slash != std::string::npos && first_slash + 1 < requested.size()) {
    const std::string without_provider = Trim(requested.substr(first_slash + 1));
    if (std::find(available_models.begin(), available_models.end(), without_provider) != available_models.end()) {
      return without_provider;
    }
  }

  // If clients pass only the filename, resolve it back to the full model id.
  const std::string requested_basename = ModelBasename(requested);
  if (!requested_basename.empty()) {
    for (const std::string& candidate : available_models) {
      if (ModelBasename(candidate) == requested_basename) {
        return candidate;
      }
    }
  }

  return requested;
}

struct ApiKeyConfig {
  std::string id;
  std::string name;
  std::string secret;
  bool enabled = true;
  int priority = 0;
  bool allow_all_models = true;
  std::vector<std::string> allowed_models;
};

struct BridgeServerConfig {
  std::string host = "127.0.0.1";
  int port = 0;
  fs::path model_folder = fs::current_path() / "models";
  std::string default_model;
  fs::path tls_cert;
  fs::path tls_key;
  std::vector<ApiKeyConfig> api_keys;
};

bool ModelSpecMatches(const std::string& allowed_model, const std::string& candidate_model) {
  const std::string allowed = Trim(allowed_model);
  const std::string candidate = Trim(candidate_model);
  if (allowed.empty() || candidate.empty()) {
    return false;
  }
  if (allowed == "*" || allowed == candidate) {
    return true;
  }
  return ModelBasename(allowed) == ModelBasename(candidate);
}

std::vector<std::string> ParseAllowedModelList(const JsonValue* value, bool* allow_all_models_out) {
  std::vector<std::string> models;
  bool allow_all = false;
  if (value == nullptr) {
    allow_all = true;
  } else if (value->type == JsonType::String) {
    const std::string model = Trim(value->string_value);
    if (model.empty() || model == "*") {
      allow_all = true;
    } else {
      models.push_back(model);
    }
  } else if (value->type == JsonType::Array) {
    for (const JsonValue& entry : value->array_value) {
      if (entry.type != JsonType::String) {
        continue;
      }
      const std::string model = Trim(entry.string_value);
      if (model.empty()) {
        continue;
      }
      if (model == "*") {
        allow_all = true;
        models.clear();
        break;
      }
      models.push_back(model);
    }
    if (models.empty() && !allow_all) {
      allow_all = true;
    }
  } else {
    allow_all = true;
  }
  if (allow_all_models_out != nullptr) {
    *allow_all_models_out = allow_all;
  }
  return models;
}

bool ParseApiKeyConfig(const JsonValue& value, ApiKeyConfig* config_out, std::string* error_out) {
  if (config_out == nullptr || value.type != JsonType::Object) {
    if (error_out != nullptr) {
      *error_out = "Each api_keys entry must be an object.";
    }
    return false;
  }

  ApiKeyConfig config;
  config.id = Trim(JsonStringOrEmpty(value.Find("id")));
  config.name = Trim(JsonStringOrEmpty(value.Find("name")));
  config.secret = Trim(JsonStringOrEmpty(value.Find("secret")));
  config.enabled = JsonBoolOrDefault(value.Find("enabled"), true);
  config.priority = JsonIntOrDefault(value.Find("priority"), 0);
  config.allowed_models = ParseAllowedModelList(value.Find("allowed_models"), &config.allow_all_models);

  if (config.id.empty()) {
    if (error_out != nullptr) {
      *error_out = "Each api_keys entry must include a non-empty id.";
    }
    return false;
  }
  if (config.secret.empty()) {
    if (error_out != nullptr) {
      *error_out = "API key '" + config.id + "' is missing secret.";
    }
    return false;
  }
  if (config.name.empty()) {
    config.name = config.id;
  }

  *config_out = std::move(config);
  return true;
}

bool ParseBridgeServerConfig(const JsonValue& root, BridgeServerConfig* config_out, std::string* error_out) {
  if (config_out == nullptr || root.type != JsonType::Object) {
    if (error_out != nullptr) {
      *error_out = "Bridge server config must be a JSON object.";
    }
    return false;
  }

  BridgeServerConfig config;
  if (const std::string host = Trim(JsonStringOrEmpty(root.Find("host"))); !host.empty()) {
    config.host = host;
  }
  config.port = JsonIntOrDefault(root.Find("port"), config.port);
  if (config.port < 0 || config.port > 65535) {
    if (error_out != nullptr) {
      *error_out = "Config port must be between 0 and 65535.";
    }
    return false;
  }

  const std::string model_folder = Trim(JsonStringOrEmpty(root.Find("model_folder")));
  if (!model_folder.empty()) {
    config.model_folder = fs::path(model_folder);
  }
  config.default_model = Trim(JsonStringOrEmpty(root.Find("default_model")));

  if (const JsonValue* tls = root.Find("tls"); tls != nullptr && tls->type == JsonType::Object) {
    const std::string cert = Trim(JsonStringOrEmpty(tls->Find("cert")));
    const std::string key = Trim(JsonStringOrEmpty(tls->Find("key")));
    if (!cert.empty()) {
      config.tls_cert = fs::path(cert);
    }
    if (!key.empty()) {
      config.tls_key = fs::path(key);
    }
  }
  if (config.tls_cert.empty()) {
    const std::string cert = Trim(JsonStringOrEmpty(root.Find("tls_cert")));
    if (!cert.empty()) {
      config.tls_cert = fs::path(cert);
    }
  }
  if (config.tls_key.empty()) {
    const std::string key = Trim(JsonStringOrEmpty(root.Find("tls_key")));
    if (!key.empty()) {
      config.tls_key = fs::path(key);
    }
  }

  std::set<std::string> seen_key_ids;
  const JsonValue* api_keys = root.Find("api_keys");
  if (api_keys == nullptr || api_keys->type != JsonType::Array || api_keys->array_value.empty()) {
    if (error_out != nullptr) {
      *error_out = "Config must include a non-empty api_keys array.";
    }
    return false;
  }
  for (const JsonValue& entry : api_keys->array_value) {
    ApiKeyConfig key;
    if (!ParseApiKeyConfig(entry, &key, error_out)) {
      return false;
    }
    if (!seen_key_ids.insert(key.id).second) {
      if (error_out != nullptr) {
        *error_out = "Duplicate api_keys id found: " + key.id;
      }
      return false;
    }
    config.api_keys.push_back(std::move(key));
  }

  *config_out = std::move(config);
  return true;
}

bool LoadBridgeServerConfigSource(const std::string& config_source,
                                  BridgeServerConfig* config_out,
                                  std::string* error_out) {
  if (config_out == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Internal config output state is null.";
    }
    return false;
  }

  const std::string trimmed_source = Trim(config_source);
  if (trimmed_source.empty()) {
    if (error_out != nullptr) {
      *error_out = "--config value cannot be empty.";
    }
    return false;
  }

  std::string json_text = trimmed_source;
  std::error_code ec;
  const fs::path candidate_path(trimmed_source);
  if (fs::exists(candidate_path, ec) && fs::is_regular_file(candidate_path, ec)) {
    json_text = ReadTextFile(candidate_path);
    if (json_text.empty()) {
      if (error_out != nullptr) {
        *error_out = "Failed to read config file: " + candidate_path.string();
      }
      return false;
    }
  }

  const std::optional<JsonValue> parsed = ParseJson(json_text);
  if (!parsed.has_value() || parsed->type != JsonType::Object) {
    if (error_out != nullptr) {
      *error_out = "Config must be valid JSON object.";
    }
    return false;
  }

  return ParseBridgeServerConfig(parsed.value(), config_out, error_out);
}

struct BridgeArgs {
  std::string host = "127.0.0.1";
  int port = 0;
  fs::path model_folder = fs::current_path() / "models";
  std::string default_model;
  std::string token;
  fs::path tls_cert;
  fs::path tls_key;
  fs::path ready_file;
  std::string config_source;
  fs::path log_file;
  bool admin_cli = false;
  bool config_mode = false;
  BridgeServerConfig config;
  bool host_set = false;
  bool port_set = false;
  bool model_folder_set = false;
  bool default_model_set = false;
  bool token_set = false;
  bool tls_cert_set = false;
  bool tls_key_set = false;
};

void PrintUsage() {
  std::cout << "uam_ollama_engine_bridge usage:\n"
               "  --host <ip-or-host>        (default: 127.0.0.1)\n"
               "  --port <port>              (default: 0, auto-assign)\n"
               "  --model-folder <path>\n"
               "  --default-model <model-name>\n"
               "  --token <bearer-token>\n"
               "  --config <path-or-json>\n"
               "  --log-file <jsonl-path>\n"
               "  --admin-cli\n"
               "  --tls-cert <pem-path>\n"
               "  --tls-key <pem-path>\n"
               "  --ready-file <json-path>\n"
               "  --help\n";
}

bool ParseInt(const std::string& text, int* value_out) {
  if (value_out == nullptr) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || errno != 0 || parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  *value_out = static_cast<int>(parsed);
  return true;
}

bool ParseArgs(int argc, char** argv, BridgeArgs* args_out, std::string* error_out) {
  if (args_out == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Internal argument parser error.";
    }
    return false;
  }
  BridgeArgs args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = (argv[i] != nullptr) ? argv[i] : "";
    auto read_value = [&](const std::string& flag_name) -> std::optional<std::string> {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        if (error_out != nullptr) {
          *error_out = "Missing value for " + flag_name + ".";
        }
        return std::nullopt;
      }
      ++i;
      return std::string(argv[i]);
    };

    if (flag == "--help" || flag == "-h") {
      PrintUsage();
      return false;
    }
    if (flag == "--host") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.host = Trim(value.value());
      args.host_set = true;
      if (args.host.empty()) {
        if (error_out != nullptr) {
          *error_out = "--host cannot be empty.";
        }
        return false;
      }
      continue;
    }
    if (flag == "--port") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      int parsed = 0;
      if (!ParseInt(Trim(value.value()), &parsed) || parsed < 0 || parsed > 65535) {
        if (error_out != nullptr) {
          *error_out = "Invalid --port value: " + value.value();
        }
        return false;
      }
      args.port = parsed;
      args.port_set = true;
      continue;
    }
    if (flag == "--model-folder") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.model_folder = fs::path(value.value());
      args.model_folder_set = true;
      continue;
    }
    if (flag == "--default-model") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.default_model = Trim(value.value());
      args.default_model_set = true;
      continue;
    }
    if (flag == "--token") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.token = value.value();
      args.token_set = true;
      continue;
    }
    if (flag == "--config") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.config_source = value.value();
      continue;
    }
    if (flag == "--log-file") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.log_file = fs::path(value.value());
      continue;
    }
    if (flag == "--admin-cli") {
      args.admin_cli = true;
      continue;
    }
    if (flag == "--tls-cert") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.tls_cert = fs::path(value.value());
      args.tls_cert_set = true;
      continue;
    }
    if (flag == "--tls-key") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.tls_key = fs::path(value.value());
      args.tls_key_set = true;
      continue;
    }
    if (flag == "--ready-file") {
      const auto value = read_value(flag);
      if (!value.has_value()) {
        return false;
      }
      args.ready_file = fs::path(value.value());
      continue;
    }

    if (error_out != nullptr) {
      *error_out = "Unknown argument: " + flag;
    }
    return false;
  }

  args.host = Trim(args.host);
  if (args.host.empty()) {
    if (error_out != nullptr) {
      *error_out = "Bridge host cannot be empty.";
    }
    return false;
  }
  if (!args.config_source.empty()) {
    if (args.token_set) {
      if (error_out != nullptr) {
        *error_out = "--token cannot be combined with --config.";
      }
      return false;
    }

    BridgeServerConfig config;
    if (!LoadBridgeServerConfigSource(args.config_source, &config, error_out)) {
      return false;
    }
    args.config_mode = true;
    args.config = config;
    if (!args.host_set) {
      args.host = config.host;
    }
    if (!args.port_set) {
      args.port = config.port;
    }
    if (!args.model_folder_set) {
      args.model_folder = config.model_folder;
    }
    if (!args.default_model_set) {
      args.default_model = config.default_model;
    }
    if (!args.tls_cert_set) {
      args.tls_cert = config.tls_cert;
    }
    if (!args.tls_key_set) {
      args.tls_key = config.tls_key;
    }
  }
  if ((args.tls_cert.empty() && !args.tls_key.empty()) || (!args.tls_cert.empty() && args.tls_key.empty())) {
    if (error_out != nullptr) {
      *error_out = "Both --tls-cert and --tls-key must be provided together.";
    }
    return false;
  }

  *args_out = std::move(args);
  return true;
}

JsonValue BuildErrorObject(const std::string& message,
                           const std::string& type,
                           const std::string& code,
                           const std::string& param = "") {
  JsonValue payload = MakeJsonObject();
  JsonValue error = MakeJsonObject();
  error.object_value["message"] = MakeJsonString(message);
  error.object_value["type"] = MakeJsonString(type);
  if (param.empty()) {
    error.object_value["param"] = MakeJsonNull();
  } else {
    error.object_value["param"] = MakeJsonString(param);
  }
  if (code.empty()) {
    error.object_value["code"] = MakeJsonNull();
  } else {
    error.object_value["code"] = MakeJsonString(code);
  }
  payload.object_value["error"] = std::move(error);
  return payload;
}

void SetJsonError(httplib::Response& response,
                  const int status,
                  const std::string& message,
                  const std::string& type,
                  const std::string& code,
                  const std::string& param = "") {
  response.status = status;
  response.set_content(SerializeJsonCompact(BuildErrorObject(message, type, code, param)), "application/json");
}

struct RequestAuthContext {
  bool config_mode = false;
  bool allow_all_models = true;
  std::string api_key_id;
  std::string api_key_name;
  int priority = 0;
  std::vector<std::string> allowed_models;
};

bool TryExtractBearerToken(const httplib::Request& request, std::string* token_out) {
  if (token_out == nullptr) {
    return false;
  }
  const std::string auth_header = request.get_header_value("Authorization");
  if (auth_header.size() <= 7) {
    return false;
  }
  if (!EqualsIgnoreCase(auth_header.substr(0, 7), "Bearer ")) {
    return false;
  }
  *token_out = auth_header.substr(7);
  return true;
}

const ApiKeyConfig* FindApiKeyBySecret(const std::vector<ApiKeyConfig>& api_keys, const std::string& secret) {
  for (const ApiKeyConfig& api_key : api_keys) {
    if (api_key.secret == secret) {
      return &api_key;
    }
  }
  return nullptr;
}

bool IsModelAllowedForAuth(const RequestAuthContext& auth, const std::string& model) {
  if (auth.allow_all_models) {
    return true;
  }
  for (const std::string& allowed_model : auth.allowed_models) {
    if (ModelSpecMatches(allowed_model, model)) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> FilterModelsForAuth(const RequestAuthContext& auth, const std::vector<std::string>& models) {
  if (auth.allow_all_models) {
    return models;
  }
  std::vector<std::string> filtered;
  for (const std::string& model : models) {
    if (IsModelAllowedForAuth(auth, model)) {
      filtered.push_back(model);
    }
  }
  return filtered;
}

bool AuthorizeRequest(const httplib::Request& request,
                      const BridgeArgs& args,
                      RequestAuthContext* auth_out,
                      std::string* error_out,
                      int* status_out,
                      std::string* code_out) {
  if (auth_out == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Internal auth output state is null.";
    }
    if (status_out != nullptr) {
      *status_out = 500;
    }
    if (code_out != nullptr) {
      *code_out = "internal_error";
    }
    return false;
  }

  RequestAuthContext auth;
  auth.config_mode = args.config_mode;

  if (!args.config_mode) {
    if (!args.token.empty()) {
      std::string bearer_token;
      if (!TryExtractBearerToken(request, &bearer_token) || bearer_token != args.token) {
        if (error_out != nullptr) {
          *error_out = "Missing or invalid bearer token.";
        }
        if (status_out != nullptr) {
          *status_out = 401;
        }
        if (code_out != nullptr) {
          *code_out = "unauthorized";
        }
        return false;
      }
    }
    *auth_out = std::move(auth);
    return true;
  }

  std::string bearer_token;
  if (!TryExtractBearerToken(request, &bearer_token)) {
    if (error_out != nullptr) {
      *error_out = "Missing bearer token.";
    }
    if (status_out != nullptr) {
      *status_out = 401;
    }
    if (code_out != nullptr) {
      *code_out = "unauthorized";
    }
    return false;
  }

  const ApiKeyConfig* api_key = FindApiKeyBySecret(args.config.api_keys, bearer_token);
  if (api_key == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Unknown API key.";
    }
    if (status_out != nullptr) {
      *status_out = 401;
    }
    if (code_out != nullptr) {
      *code_out = "unauthorized";
    }
    return false;
  }
  if (!api_key->enabled) {
    if (error_out != nullptr) {
      *error_out = "API key is disabled.";
    }
    if (status_out != nullptr) {
      *status_out = 403;
    }
    if (code_out != nullptr) {
      *code_out = "api_key_disabled";
    }
    return false;
  }

  auth.api_key_id = api_key->id;
  auth.api_key_name = api_key->name;
  auth.priority = api_key->priority;
  auth.allow_all_models = api_key->allow_all_models;
  auth.allowed_models = api_key->allowed_models;
  *auth_out = std::move(auth);
  return true;
}

struct CompletionResult {
  std::string request_id;
  std::string model;
  std::string assistant_text;
  std::optional<ParsedToolEnvelope> tool_call;
  std::string tool_call_id;
  std::int64_t created_unix = 0;
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

class BridgeRuntime {
 public:
  explicit BridgeRuntime(ollama_engine::EngineOptions options) : options_(std::move(options)) {}

  bool Initialize(const std::string& preferred_model, std::string* error_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_ = ollama_engine::CreateEngine(options_);
    if (!engine_) {
      if (error_out != nullptr) {
        *error_out = "Failed to create ollama_engine runtime.";
      }
      return false;
    }

    const std::vector<std::string> models = engine_->ListModels();
    if (models.empty()) {
      if (error_out != nullptr) {
        *error_out = "No models found in folder: " + options_.pPathModelFolder.string();
      }
      return false;
    }

    std::string model_to_load = preferred_model;
    if (model_to_load.empty()) {
      model_to_load = models.front();
    } else if (std::find(models.begin(), models.end(), model_to_load) == models.end()) {
      if (error_out != nullptr) {
        *error_out = "Default model not found in model folder: " + model_to_load;
      }
      return false;
    }

    std::string load_error;
    if (!engine_->Load(model_to_load, &load_error)) {
      if (error_out != nullptr) {
        *error_out = "Failed to load model '" + model_to_load + "': " +
                     (load_error.empty() ? std::string("unknown error") : load_error);
      }
      return false;
    }
    loaded_model_ = model_to_load;
    return true;
  }

  std::vector<std::string> ListModels(std::string* error_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) {
      if (error_out != nullptr) {
        *error_out = "Bridge runtime is not initialized.";
      }
      return {};
    }
    return engine_->ListModels();
  }

  std::string LoadedModel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_model_;
  }

  bool GenerateCompletion(const JsonValue& request, CompletionResult* result_out, std::string* error_out, int* status_out) {
    if (result_out == nullptr) {
      if (error_out != nullptr) {
        *error_out = "Internal output state is null.";
      }
      if (status_out != nullptr) {
        *status_out = 500;
      }
      return false;
    }
    const JsonValue* messages = request.Find("messages");
    if (messages == nullptr || messages->type != JsonType::Array || messages->array_value.empty()) {
      if (error_out != nullptr) {
        *error_out = "Request must include a non-empty messages array.";
      }
      if (status_out != nullptr) {
        *status_out = 400;
      }
      return false;
    }

    const std::vector<ToolDefinition> tools = ParseToolDefinitions(request.Find("tools"));
    const bool has_tools = !tools.empty();
    const bool require_tool_call = has_tools && RequestLikelyNeedsToolCall(*messages, tools);
    const std::string latest_user_message = LatestUserMessageText(*messages);

    const std::string conversation_transcript = BuildTranscriptPrompt(*messages);
    if (conversation_transcript.empty()) {
      if (error_out != nullptr) {
        *error_out = "Could not derive prompt text from messages.";
      }
      if (status_out != nullptr) {
        *status_out = 400;
      }
      return false;
    }

    std::string generation_prompt = conversation_transcript;
    if (has_tools) {
      generation_prompt = BuildToolEnvelopePrompt(conversation_transcript, tools);
    }

    const std::string requested_model = NormalizeRequestedModel(JsonStringOrEmpty(request.Find("model")));

    ollama_engine::SendMessageResponse engine_response;
    std::string effective_model;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!engine_) {
        if (error_out != nullptr) {
          *error_out = "Bridge runtime is not initialized.";
        }
        if (status_out != nullptr) {
          *status_out = 500;
        }
        return false;
      }

      std::string load_error;
      if (!requested_model.empty()) {
        if (!EnsureModelLoadedLocked(requested_model, &load_error)) {
          if (error_out != nullptr) {
            *error_out = load_error.empty() ? "Failed to load requested model." : load_error;
          }
          if (status_out != nullptr) {
            *status_out = 400;
          }
          return false;
        }
      }
      effective_model = loaded_model_;
      engine_response = engine_->SendMessage(generation_prompt);
    }

    if (!engine_response.pbOk) {
      if (error_out != nullptr) {
        *error_out = engine_response.pSError.empty() ? "Local engine request failed." : engine_response.pSError;
      }
      if (status_out != nullptr) {
        *status_out = 500;
      }
      return false;
    }

    CompletionResult completion;
    completion.request_id = BuildId("chatcmpl");
    completion.model = effective_model;
    completion.created_unix = UnixEpochSecondsNow();
    completion.prompt_tokens = CountApproxTokens(generation_prompt);

    std::string raw_text = Trim(engine_response.pSText);
    if (has_tools) {
      std::set<std::string> allowed_tool_names;
      for (const ToolDefinition& tool : tools) {
        allowed_tool_names.insert(tool.name);
      }
      std::optional<ParsedToolEnvelope> envelope = ParseToolEnvelopeResponse(raw_text, allowed_tool_names);
      if (!envelope.has_value() && LooksLikeToolEnvelopeCandidate(raw_text)) {
        const std::string repair_prompt = BuildToolEnvelopeRepairPrompt(conversation_transcript, tools, raw_text);
        ollama_engine::SendMessageResponse repair_response;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (engine_) {
            repair_response = engine_->SendMessage(repair_prompt);
          }
        }
        if (repair_response.pbOk) {
          const std::string repaired_text = Trim(repair_response.pSText);
          if (!repaired_text.empty()) {
            raw_text = repaired_text;
            completion.prompt_tokens += CountApproxTokens(repair_prompt);
            envelope = ParseToolEnvelopeResponse(raw_text, allowed_tool_names);
          }
        }
      }

      if (require_tool_call && (!envelope.has_value() || !envelope->is_tool_call)) {
        const std::string enforce_prompt = BuildToolCallOnlyRepairPrompt(conversation_transcript, tools, raw_text);
        ollama_engine::SendMessageResponse enforce_response;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (engine_) {
            enforce_response = engine_->SendMessage(enforce_prompt);
          }
        }
        if (enforce_response.pbOk) {
          const std::string enforced_text = Trim(enforce_response.pSText);
          if (!enforced_text.empty()) {
            raw_text = enforced_text;
            completion.prompt_tokens += CountApproxTokens(enforce_prompt);
            envelope = ParseToolEnvelopeResponse(raw_text, allowed_tool_names);
          }
        }
      }

      if (!envelope.has_value() && LooksLikeToolEnvelopeCandidate(raw_text)) {
        const std::string fallback_prompt =
            conversation_transcript + "\n\nReply to the user directly in plain text. Do not output JSON.";
        ollama_engine::SendMessageResponse fallback_response;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (engine_) {
            fallback_response = engine_->SendMessage(fallback_prompt);
          }
        }
        if (fallback_response.pbOk) {
          const std::string fallback_text = Trim(fallback_response.pSText);
          if (!fallback_text.empty()) {
            raw_text = fallback_text;
            completion.prompt_tokens += CountApproxTokens(fallback_prompt);
          }
        }
      }

      if (require_tool_call && (!envelope.has_value() || !envelope->is_tool_call) &&
          allowed_tool_names.find("bash") != allowed_tool_names.end()) {
        const std::string user_request_for_rescue =
            latest_user_message.empty() ? conversation_transcript : latest_user_message;
        const std::string rescue_prompt = BuildBashRescuePrompt(user_request_for_rescue, raw_text);
        ollama_engine::SendMessageResponse rescue_response;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (engine_) {
            rescue_response = engine_->SendMessage(rescue_prompt);
          }
        }
        if (rescue_response.pbOk) {
          const std::string rescued_text = Trim(rescue_response.pSText);
          if (!rescued_text.empty()) {
            completion.prompt_tokens += CountApproxTokens(rescue_prompt);
            if (const std::optional<ParsedToolEnvelope> rescued_envelope =
                    ParseToolEnvelopeResponse(rescued_text, allowed_tool_names);
                rescued_envelope.has_value() && rescued_envelope->is_tool_call) {
              raw_text = rescued_text;
              envelope = rescued_envelope;
            } else {
              const std::string bash_command = ExtractBashCommandFromText(rescued_text);
              if (!bash_command.empty()) {
                ParsedToolEnvelope synthesized;
                synthesized.is_tool_call = true;
                synthesized.tool_name = "bash";
                JsonValue args = MakeJsonObject();
                args.object_value["description"] = MakeJsonString("Execute requested shell command.");
                args.object_value["command"] = MakeJsonString(bash_command);
                synthesized.tool_arguments_json = SerializeJsonCompact(args);
                raw_text = rescued_text;
                envelope = std::move(synthesized);
              }
            }
          }
        }
      }

      if (!envelope.has_value()) {
        const std::string candidate = Trim(StripMarkdownCodeFence(raw_text));
        if (!candidate.empty() &&
            (candidate == "{" || candidate == "{\"" || candidate == "[" || candidate.size() <= 3)) {
          raw_text = "I could not generate a valid tool call. Please retry with a shorter request.";
        }
      }

      if (require_tool_call && (!envelope.has_value() || !envelope->is_tool_call)) {
        const std::string synthesis_seed = latest_user_message.empty() ? raw_text : latest_user_message;
        if (const std::optional<ParsedToolEnvelope> synthesized =
                SynthesizeFallbackToolCall(allowed_tool_names, synthesis_seed);
            synthesized.has_value()) {
          envelope = synthesized;
        }
      }

      if (require_tool_call && (!envelope.has_value() || !envelope->is_tool_call)) {
        raw_text = "I could not produce a valid tool call for this action request. "
                   "Please retry with a simpler command or use a stronger local model.";
      }

      if (envelope.has_value() && envelope->is_tool_call) {
        completion.tool_call = envelope;
        completion.tool_call_id = BuildId("call");
        completion.completion_tokens =
            CountApproxTokens(envelope->tool_name + " " + envelope->tool_arguments_json);
      } else if (envelope.has_value() && !envelope->is_tool_call) {
        completion.assistant_text = envelope->text;
        completion.completion_tokens = CountApproxTokens(completion.assistant_text);
      } else {
        completion.assistant_text = raw_text;
        completion.completion_tokens = CountApproxTokens(completion.assistant_text);
      }
    } else {
      completion.assistant_text = raw_text;
      completion.completion_tokens = CountApproxTokens(completion.assistant_text);
    }

    *result_out = std::move(completion);
    return true;
  }

 private:
  bool EnsureModelLoadedLocked(const std::string& model_name, std::string* error_out) {
    if (!engine_) {
      if (error_out != nullptr) {
        *error_out = "Bridge runtime is not initialized.";
      }
      return false;
    }
    const std::vector<std::string> models = engine_->ListModels();
    const std::string resolved_model = ResolveRequestedModelAgainstAvailable(model_name, models);
    if (loaded_model_ == resolved_model) {
      return true;
    }
    if (std::find(models.begin(), models.end(), resolved_model) == models.end()) {
      if (error_out != nullptr) {
        *error_out = "Requested model was not found: " + model_name;
      }
      return false;
    }
    std::string load_error;
    if (!engine_->Load(resolved_model, &load_error)) {
      if (error_out != nullptr) {
        *error_out = "Failed to load model '" + resolved_model + "': " +
                     (load_error.empty() ? std::string("unknown error") : load_error);
      }
      return false;
    }
    loaded_model_ = resolved_model;
    return true;
  }

  ollama_engine::EngineOptions options_;
  mutable std::mutex mutex_;
  std::unique_ptr<ollama_engine::EngineInterface> engine_;
  std::string loaded_model_;
};

struct QueuedCompletionResult {
  bool ok = false;
  CompletionResult completion;
  std::string error;
  int status = 500;
  int queue_wait_ms = 0;
  int latency_ms = 0;
};

struct PendingBridgeRequest {
  std::string request_id;
  JsonValue request;
  RequestAuthContext auth;
  std::string remote_address;
  std::string user_agent;
  std::uint64_t sequence = 0;
  std::int64_t enqueued_unix = 0;
  std::chrono::steady_clock::time_point enqueued_at{};
  std::promise<QueuedCompletionResult> promise;
};

struct ClientActivity {
  std::string api_key_id;
  std::string api_key_name;
  std::string remote_address;
  std::string user_agent;
  std::string last_model;
  std::int64_t last_request_unix = 0;
  std::size_t total_requests = 0;
  std::size_t auth_failures = 0;
  int last_prompt_tokens = 0;
  int last_completion_tokens = 0;
};

class BridgeServerCoordinator {
 public:
  BridgeServerCoordinator(ollama_engine::EngineOptions options, fs::path log_file)
      : runtime_(std::move(options)), log_file_(std::move(log_file)), started_at_(std::chrono::steady_clock::now()) {}

  ~BridgeServerCoordinator() {
    Stop();
  }

  bool Initialize(const std::string& preferred_model, std::string* error_out) {
    if (!runtime_.Initialize(preferred_model, error_out)) {
      return false;
    }
    worker_thread_ = std::thread([this]() { WorkerMain(); });
    return true;
  }

  void Stop() {
    const bool already_stopping = stop_requested_.exchange(true);
    std::vector<std::shared_ptr<PendingBridgeRequest>> cancelled;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      cancelled.swap(pending_requests_);
    }
    for (const auto& request : cancelled) {
      if (request == nullptr) {
        continue;
      }
      QueuedCompletionResult result;
      result.ok = false;
      result.status = 503;
      result.error = "Bridge server is shutting down.";
      request->promise.set_value(std::move(result));
    }
    queue_cv_.notify_all();
    if (!already_stopping && worker_thread_.joinable()) {
      worker_thread_.join();
    } else if (already_stopping && worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }

  bool StopRequested() const {
    return stop_requested_.load();
  }

  std::vector<std::string> ListModels(std::string* error_out) {
    return runtime_.ListModels(error_out);
  }

  std::string LoadedModel() const {
    return runtime_.LoadedModel();
  }

  bool ResolveAuthorizedModel(const RequestAuthContext& auth,
                              const std::string& requested_model,
                              std::string* resolved_model_out,
                              std::string* error_out,
                              int* status_out) {
    if (resolved_model_out == nullptr) {
      if (error_out != nullptr) {
        *error_out = "Internal model output state is null.";
      }
      if (status_out != nullptr) {
        *status_out = 500;
      }
      return false;
    }

    std::string list_error;
    const std::vector<std::string> models = runtime_.ListModels(&list_error);
    if (!list_error.empty()) {
      if (error_out != nullptr) {
        *error_out = list_error;
      }
      if (status_out != nullptr) {
        *status_out = 500;
      }
      return false;
    }

    if (auth.allow_all_models) {
      if (!requested_model.empty()) {
        *resolved_model_out = ResolveRequestedModelAgainstAvailable(requested_model, models);
      } else {
        resolved_model_out->clear();
      }
      return true;
    }

    if (!requested_model.empty()) {
      const std::string resolved_model = ResolveRequestedModelAgainstAvailable(requested_model, models);
      if (!IsModelAllowedForAuth(auth, resolved_model)) {
        if (error_out != nullptr) {
          *error_out = "Requested model is not allowed for this API key.";
        }
        if (status_out != nullptr) {
          *status_out = 403;
        }
        return false;
      }
      *resolved_model_out = resolved_model;
      return true;
    }

    const std::string loaded_model = runtime_.LoadedModel();
    if (!loaded_model.empty() && IsModelAllowedForAuth(auth, loaded_model)) {
      *resolved_model_out = loaded_model;
      return true;
    }
    for (const std::string& model : models) {
      if (IsModelAllowedForAuth(auth, model)) {
        *resolved_model_out = model;
        return true;
      }
    }

    if (error_out != nullptr) {
      *error_out = "No allowed models are available for this API key.";
    }
    if (status_out != nullptr) {
      *status_out = 403;
    }
    return false;
  }

  std::future<QueuedCompletionResult> EnqueueCompletion(JsonValue request,
                                                        const RequestAuthContext& auth,
                                                        const std::string& remote_address,
                                                        const std::string& user_agent) {
    auto pending = std::make_shared<PendingBridgeRequest>();
    pending->request_id = BuildId("chatcmpl");
    pending->request = std::move(request);
    pending->auth = auth;
    pending->remote_address = remote_address;
    pending->user_agent = user_agent;
    pending->enqueued_unix = UnixEpochSecondsNow();
    pending->enqueued_at = std::chrono::steady_clock::now();

    std::future<QueuedCompletionResult> future = pending->promise.get_future();
    std::size_t queue_depth = 0;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      pending->sequence = next_sequence_++;
      pending_requests_.push_back(pending);
      queue_depth = pending_requests_.size();
    }
    queue_cv_.notify_one();

    JsonValue event = MakeJsonObject();
    event.object_value["event"] = MakeJsonString("request_enqueued");
    event.object_value["timestamp"] = MakeJsonString(TimestampIso8601Now());
    event.object_value["timestamp_unix_ms"] = MakeJsonNumber(static_cast<double>(UnixEpochMillisecondsNow()));
    event.object_value["request_id"] = MakeJsonString(pending->request_id);
    event.object_value["api_key_id"] = MakeJsonString(auth.api_key_id);
    event.object_value["api_key_name"] = MakeJsonString(auth.api_key_name);
    event.object_value["priority"] = MakeJsonNumber(static_cast<double>(auth.priority));
    event.object_value["queue_depth"] = MakeJsonNumber(static_cast<double>(queue_depth));
    event.object_value["remote_address"] = MakeJsonString(remote_address);
    event.object_value["user_agent"] = MakeJsonString(user_agent);
    RecordEvent(std::move(event));
    return future;
  }

  void RecordAuthFailure(const std::string& remote_address,
                         const std::string& user_agent,
                         const std::string& message,
                         const std::string& code) {
    {
      std::lock_guard<std::mutex> lock(telemetry_mutex_);
      const std::string client_key = std::string("unauthenticated@") + remote_address;
      ClientActivity& client = clients_[client_key];
      client.api_key_id = "unauthenticated";
      client.api_key_name = "unauthenticated";
      client.remote_address = remote_address;
      client.user_agent = user_agent;
      client.last_request_unix = UnixEpochSecondsNow();
      ++client.auth_failures;
    }

    JsonValue event = MakeJsonObject();
    event.object_value["event"] = MakeJsonString("auth_failure");
    event.object_value["timestamp"] = MakeJsonString(TimestampIso8601Now());
    event.object_value["timestamp_unix_ms"] = MakeJsonNumber(static_cast<double>(UnixEpochMillisecondsNow()));
    event.object_value["remote_address"] = MakeJsonString(remote_address);
    event.object_value["user_agent"] = MakeJsonString(user_agent);
    event.object_value["message"] = MakeJsonString(message);
    event.object_value["code"] = MakeJsonString(code);
    RecordEvent(std::move(event));
  }

  void RecordEvent(JsonValue event) {
    if (event.type != JsonType::Object) {
      return;
    }
    if (event.Find("timestamp") == nullptr) {
      event.object_value["timestamp"] = MakeJsonString(TimestampIso8601Now());
    }
    if (event.Find("timestamp_unix_ms") == nullptr) {
      event.object_value["timestamp_unix_ms"] = MakeJsonNumber(static_cast<double>(UnixEpochMillisecondsNow()));
    }
    const std::string line = SerializeJsonCompact(event);

    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    recent_events_.push_back(line);
    static constexpr std::size_t kMaxRecentEvents = 64;
    while (recent_events_.size() > kMaxRecentEvents) {
      recent_events_.pop_front();
    }
    if (!log_file_.empty()) {
      std::error_code ec;
      if (!log_file_.parent_path().empty()) {
        fs::create_directories(log_file_.parent_path(), ec);
      }
      std::ofstream out(log_file_, std::ios::binary | std::ios::app);
      if (out.good()) {
        out << line << "\n";
      }
    }
  }

  std::size_t QueueDepth() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return pending_requests_.size();
  }

  std::string BuildStatusReport() const {
    const auto uptime_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at_).count();
    std::ostringstream out;
    out << "loaded_model=" << LoadedModel() << "\n";
    out << "queue_depth=" << QueueDepth() << "\n";
    out << "uptime_seconds=" << uptime_seconds << "\n";
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (active_request_ != nullptr) {
        out << "active_request=" << active_request_->request_id << " key=" << active_request_->auth.api_key_id
            << " priority=" << active_request_->auth.priority << "\n";
      } else {
        out << "active_request=(none)\n";
      }
    }
    return out.str();
  }

  std::vector<std::string> BuildQueueReport() const {
    std::vector<std::shared_ptr<PendingBridgeRequest>> queued;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      queued = pending_requests_;
    }
    std::sort(queued.begin(), queued.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs->auth.priority != rhs->auth.priority) {
        return lhs->auth.priority > rhs->auth.priority;
      }
      return lhs->sequence < rhs->sequence;
    });

    std::vector<std::string> lines;
    if (queued.empty()) {
      lines.push_back("queue is empty");
      return lines;
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& request : queued) {
      std::ostringstream out;
      out << request->request_id << " key=" << request->auth.api_key_id << " priority=" << request->auth.priority
          << " wait_ms="
          << std::chrono::duration_cast<std::chrono::milliseconds>(now - request->enqueued_at).count()
          << " remote=" << request->remote_address;
      lines.push_back(out.str());
    }
    return lines;
  }

  std::vector<std::string> BuildClientReport() const {
    std::vector<ClientActivity> clients;
    {
      std::lock_guard<std::mutex> lock(telemetry_mutex_);
      for (const auto& entry : clients_) {
        clients.push_back(entry.second);
      }
    }
    std::sort(clients.begin(), clients.end(), [](const ClientActivity& lhs, const ClientActivity& rhs) {
      return lhs.last_request_unix > rhs.last_request_unix;
    });

    std::vector<std::string> lines;
    if (clients.empty()) {
      lines.push_back("no clients recorded");
      return lines;
    }
    for (const ClientActivity& client : clients) {
      std::ostringstream out;
      out << client.api_key_id << " (" << client.api_key_name << ")"
          << " remote=" << client.remote_address << " requests=" << client.total_requests
          << " auth_failures=" << client.auth_failures << " last_model=" << client.last_model
          << " last_request_unix=" << client.last_request_unix;
      if (!client.user_agent.empty()) {
        out << " user_agent=" << client.user_agent;
      }
      lines.push_back(out.str());
    }
    return lines;
  }

  std::vector<std::string> RecentEventLines() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return std::vector<std::string>(recent_events_.begin(), recent_events_.end());
  }

 private:
  void WorkerMain() {
    for (;;) {
      std::shared_ptr<PendingBridgeRequest> request;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [&]() { return stop_requested_.load() || !pending_requests_.empty(); });
        if (stop_requested_.load() && pending_requests_.empty()) {
          break;
        }
        auto best_it = std::max_element(pending_requests_.begin(), pending_requests_.end(), [](const auto& lhs, const auto& rhs) {
          if (lhs->auth.priority != rhs->auth.priority) {
            return lhs->auth.priority < rhs->auth.priority;
          }
          return lhs->sequence > rhs->sequence;
        });
        request = *best_it;
        pending_requests_.erase(best_it);
        active_request_ = request;
      }

      const auto started_at = std::chrono::steady_clock::now();
      const int queue_wait_ms =
          static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(started_at - request->enqueued_at).count());
      JsonValue start_event = MakeJsonObject();
      start_event.object_value["event"] = MakeJsonString("request_start");
      start_event.object_value["request_id"] = MakeJsonString(request->request_id);
      start_event.object_value["api_key_id"] = MakeJsonString(request->auth.api_key_id);
      start_event.object_value["api_key_name"] = MakeJsonString(request->auth.api_key_name);
      start_event.object_value["priority"] = MakeJsonNumber(static_cast<double>(request->auth.priority));
      start_event.object_value["queue_wait_ms"] = MakeJsonNumber(static_cast<double>(queue_wait_ms));
      start_event.object_value["remote_address"] = MakeJsonString(request->remote_address);
      start_event.object_value["user_agent"] = MakeJsonString(request->user_agent);
      RecordEvent(std::move(start_event));

      QueuedCompletionResult result;
      result.queue_wait_ms = queue_wait_ms;
      CompletionResult completion;
      int status = 500;
      std::string error;
      const bool ok = runtime_.GenerateCompletion(request->request, &completion, &error, &status);
      const auto finished_at = std::chrono::steady_clock::now();
      result.ok = ok;
      result.status = status;
      result.error = error;
      result.latency_ms =
          static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(finished_at - started_at).count());
      if (ok) {
        completion.request_id = request->request_id;
        result.completion = completion;
      }
      UpdateClientActivity(*request, result);
      LogRequestCompletion(*request, result);
      request->promise.set_value(std::move(result));

      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (active_request_ != nullptr && active_request_->request_id == request->request_id) {
          active_request_.reset();
        }
      }
    }
  }

  void UpdateClientActivity(const PendingBridgeRequest& request, const QueuedCompletionResult& result) {
    if (request.auth.api_key_id.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    const std::string client_key = request.auth.api_key_id + "@" + request.remote_address;
    ClientActivity& client = clients_[client_key];
    client.api_key_id = request.auth.api_key_id;
    client.api_key_name = request.auth.api_key_name;
    client.remote_address = request.remote_address;
    client.user_agent = request.user_agent;
    client.last_request_unix = UnixEpochSecondsNow();
    ++client.total_requests;
    if (result.ok) {
      client.last_model = result.completion.model;
      client.last_prompt_tokens = result.completion.prompt_tokens;
      client.last_completion_tokens = result.completion.completion_tokens;
    }
  }

  void LogRequestCompletion(const PendingBridgeRequest& request, const QueuedCompletionResult& result) {
    JsonValue event = MakeJsonObject();
    event.object_value["event"] = MakeJsonString(result.ok ? "request_finish" : "request_fail");
    event.object_value["request_id"] = MakeJsonString(request.request_id);
    event.object_value["api_key_id"] = MakeJsonString(request.auth.api_key_id);
    event.object_value["api_key_name"] = MakeJsonString(request.auth.api_key_name);
    event.object_value["priority"] = MakeJsonNumber(static_cast<double>(request.auth.priority));
    event.object_value["queue_wait_ms"] = MakeJsonNumber(static_cast<double>(result.queue_wait_ms));
    event.object_value["latency_ms"] = MakeJsonNumber(static_cast<double>(result.latency_ms));
    event.object_value["remote_address"] = MakeJsonString(request.remote_address);
    event.object_value["user_agent"] = MakeJsonString(request.user_agent);
    event.object_value["status"] = MakeJsonNumber(static_cast<double>(result.status));
    if (result.ok) {
      event.object_value["model"] = MakeJsonString(result.completion.model);
      event.object_value["prompt_tokens"] = MakeJsonNumber(static_cast<double>(result.completion.prompt_tokens));
      event.object_value["completion_tokens"] = MakeJsonNumber(static_cast<double>(result.completion.completion_tokens));
      event.object_value["total_tokens"] =
          MakeJsonNumber(static_cast<double>(result.completion.prompt_tokens + result.completion.completion_tokens));
    } else {
      event.object_value["message"] = MakeJsonString(result.error.empty() ? "unknown error" : result.error);
    }
    RecordEvent(std::move(event));
  }

  BridgeRuntime runtime_;
  fs::path log_file_;
  std::chrono::steady_clock::time_point started_at_;
  std::atomic<bool> stop_requested_{false};
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::vector<std::shared_ptr<PendingBridgeRequest>> pending_requests_;
  std::shared_ptr<PendingBridgeRequest> active_request_;
  std::thread worker_thread_;
  std::uint64_t next_sequence_ = 1;

  mutable std::mutex telemetry_mutex_;
  std::deque<std::string> recent_events_;
  std::unordered_map<std::string, ClientActivity> clients_;
};

JsonValue BuildModelsResponse(const std::vector<std::string>& models) {
  JsonValue root = MakeJsonObject();
  root.object_value["object"] = MakeJsonString("list");
  JsonValue data = MakeJsonArray();
  const std::int64_t now = UnixEpochSecondsNow();
  for (const std::string& model : models) {
    JsonValue item = MakeJsonObject();
    item.object_value["id"] = MakeJsonString(model);
    item.object_value["object"] = MakeJsonString("model");
    item.object_value["created"] = MakeJsonNumber(static_cast<double>(now));
    item.object_value["owned_by"] = MakeJsonString("uam-local");
    data.array_value.push_back(std::move(item));
  }
  root.object_value["data"] = std::move(data);
  return root;
}

JsonValue BuildNonStreamingChatResponse(const CompletionResult& completion) {
  JsonValue root = MakeJsonObject();
  root.object_value["id"] = MakeJsonString(completion.request_id);
  root.object_value["object"] = MakeJsonString("chat.completion");
  root.object_value["created"] = MakeJsonNumber(static_cast<double>(completion.created_unix));
  root.object_value["model"] = MakeJsonString(completion.model);

  JsonValue choices = MakeJsonArray();
  JsonValue choice = MakeJsonObject();
  choice.object_value["index"] = MakeJsonNumber(0.0);
  JsonValue message = MakeJsonObject();
  message.object_value["role"] = MakeJsonString("assistant");

  if (completion.tool_call.has_value() && completion.tool_call->is_tool_call) {
    message.object_value["content"] = MakeJsonNull();
    JsonValue tool_calls = MakeJsonArray();
    JsonValue call = MakeJsonObject();
    call.object_value["id"] = MakeJsonString(completion.tool_call_id);
    call.object_value["type"] = MakeJsonString("function");
    JsonValue function = MakeJsonObject();
    function.object_value["name"] = MakeJsonString(completion.tool_call->tool_name);
    function.object_value["arguments"] = MakeJsonString(completion.tool_call->tool_arguments_json);
    call.object_value["function"] = std::move(function);
    tool_calls.array_value.push_back(std::move(call));
    message.object_value["tool_calls"] = std::move(tool_calls);
    choice.object_value["finish_reason"] = MakeJsonString("tool_calls");
  } else {
    message.object_value["content"] = MakeJsonString(completion.assistant_text);
    choice.object_value["finish_reason"] = MakeJsonString("stop");
  }

  choice.object_value["message"] = std::move(message);
  choices.array_value.push_back(std::move(choice));
  root.object_value["choices"] = std::move(choices);

  JsonValue usage = MakeJsonObject();
  usage.object_value["prompt_tokens"] = MakeJsonNumber(static_cast<double>(completion.prompt_tokens));
  usage.object_value["completion_tokens"] = MakeJsonNumber(static_cast<double>(completion.completion_tokens));
  usage.object_value["total_tokens"] =
      MakeJsonNumber(static_cast<double>(completion.prompt_tokens + completion.completion_tokens));
  root.object_value["usage"] = std::move(usage);

  return root;
}

JsonValue BuildChunkEnvelope(const CompletionResult& completion, JsonValue delta, const std::optional<std::string>& finish_reason) {
  JsonValue root = MakeJsonObject();
  root.object_value["id"] = MakeJsonString(completion.request_id);
  root.object_value["object"] = MakeJsonString("chat.completion.chunk");
  root.object_value["created"] = MakeJsonNumber(static_cast<double>(completion.created_unix));
  root.object_value["model"] = MakeJsonString(completion.model);

  JsonValue choices = MakeJsonArray();
  JsonValue choice = MakeJsonObject();
  choice.object_value["index"] = MakeJsonNumber(0.0);
  choice.object_value["delta"] = std::move(delta);
  if (finish_reason.has_value()) {
    choice.object_value["finish_reason"] = MakeJsonString(*finish_reason);
  } else {
    choice.object_value["finish_reason"] = MakeJsonNull();
  }
  choices.array_value.push_back(std::move(choice));
  root.object_value["choices"] = std::move(choices);
  return root;
}

std::string BuildStreamingSsePayload(const CompletionResult& completion) {
  std::ostringstream out;

  {
    JsonValue delta = MakeJsonObject();
    delta.object_value["role"] = MakeJsonString("assistant");
    out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(delta), std::nullopt)) << "\n\n";
  }

  if (completion.tool_call.has_value() && completion.tool_call->is_tool_call) {
    JsonValue delta = MakeJsonObject();
    JsonValue tool_calls = MakeJsonArray();
    JsonValue tool_call = MakeJsonObject();
    tool_call.object_value["index"] = MakeJsonNumber(0.0);
    tool_call.object_value["id"] = MakeJsonString(completion.tool_call_id);
    tool_call.object_value["type"] = MakeJsonString("function");
    JsonValue function = MakeJsonObject();
    function.object_value["name"] = MakeJsonString(completion.tool_call->tool_name);
    function.object_value["arguments"] = MakeJsonString(completion.tool_call->tool_arguments_json);
    tool_call.object_value["function"] = std::move(function);
    tool_calls.array_value.push_back(std::move(tool_call));
    delta.object_value["tool_calls"] = std::move(tool_calls);
    out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(delta), std::nullopt)) << "\n\n";

    JsonValue done_delta = MakeJsonObject();
    out << "data: "
        << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(done_delta), std::string("tool_calls")))
        << "\n\n";
    out << "data: [DONE]\n\n";
    return out.str();
  }

  static constexpr std::size_t kChunkSize = 160;
  const std::string text = completion.assistant_text;
  if (text.empty()) {
    JsonValue empty_delta = MakeJsonObject();
    empty_delta.object_value["content"] = MakeJsonString("");
    out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(empty_delta), std::nullopt))
        << "\n\n";
  } else {
    for (std::size_t offset = 0; offset < text.size(); offset += kChunkSize) {
      const std::string chunk = text.substr(offset, std::min(kChunkSize, text.size() - offset));
      JsonValue delta = MakeJsonObject();
      delta.object_value["content"] = MakeJsonString(chunk);
      out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(delta), std::nullopt)) << "\n\n";
    }
  }

  JsonValue done_delta = MakeJsonObject();
  out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(done_delta), std::string("stop")))
      << "\n\n";
  out << "data: [DONE]\n\n";
  return out.str();
}

bool TryReadAdminLine(std::string* line_out, const int timeout_ms) {
  if (line_out == nullptr) {
    return false;
  }
#if defined(_WIN32)
  HANDLE input_handle = GetStdHandle(STD_INPUT_HANDLE);
  if (input_handle == INVALID_HANDLE_VALUE || input_handle == nullptr) {
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    return false;
  }
  const DWORD wait_result = WaitForSingleObject(input_handle, static_cast<DWORD>(std::max(timeout_ms, 1)));
  if (wait_result != WAIT_OBJECT_0) {
    return false;
  }
#else
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(STDIN_FILENO, &read_set);
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
  const int ready = select(STDIN_FILENO + 1, &read_set, nullptr, nullptr, &timeout);
  if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_set)) {
    return false;
  }
#endif
  return static_cast<bool>(std::getline(std::cin, *line_out));
}

void PrintAdminLines(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    std::cout << line << "\n";
  }
}

template <typename StopFn>
void RunAdminCli(BridgeServerCoordinator& coordinator, const BridgeArgs& args, StopFn stop_server) {
  std::cout << "Admin CLI ready. Commands: help, status, models, keys, queue, clients, recent, quit\n";
  while (!coordinator.StopRequested()) {
    std::string line;
    if (!TryReadAdminLine(&line, 250)) {
      continue;
    }
    const std::string command = ToLowerAscii(Trim(line));
    if (command.empty()) {
      continue;
    }

    if (command == "help") {
      std::cout << "help    Show commands\n";
      std::cout << "status  Show runtime status\n";
      std::cout << "models  List available models\n";
      std::cout << "keys    List configured API keys\n";
      std::cout << "queue   Show queued requests\n";
      std::cout << "clients Show recent client activity\n";
      std::cout << "recent  Show recent log events\n";
      std::cout << "quit    Stop the bridge server\n";
      continue;
    }
    if (command == "status") {
      std::cout << coordinator.BuildStatusReport();
      continue;
    }
    if (command == "models") {
      std::string error;
      const std::vector<std::string> models = coordinator.ListModels(&error);
      if (!error.empty()) {
        std::cout << "error: " << error << "\n";
      } else if (models.empty()) {
        std::cout << "(no models found)\n";
      } else {
        PrintAdminLines(models);
      }
      continue;
    }
    if (command == "keys") {
      if (!args.config_mode) {
        std::cout << "legacy token mode\n";
        continue;
      }
      if (args.config.api_keys.empty()) {
        std::cout << "(no api keys configured)\n";
        continue;
      }
      for (const ApiKeyConfig& api_key : args.config.api_keys) {
        std::ostringstream out;
        out << api_key.id << " (" << api_key.name << ")"
            << " enabled=" << (api_key.enabled ? "true" : "false")
            << " priority=" << api_key.priority
            << " models=";
        if (api_key.allow_all_models) {
          out << "*";
        } else {
          for (std::size_t i = 0; i < api_key.allowed_models.size(); ++i) {
            if (i > 0) {
              out << ",";
            }
            out << api_key.allowed_models[i];
          }
        }
        std::cout << out.str() << "\n";
      }
      continue;
    }
    if (command == "queue") {
      PrintAdminLines(coordinator.BuildQueueReport());
      continue;
    }
    if (command == "clients") {
      PrintAdminLines(coordinator.BuildClientReport());
      continue;
    }
    if (command == "recent") {
      PrintAdminLines(coordinator.RecentEventLines());
      continue;
    }
    if (command == "quit") {
      JsonValue event = MakeJsonObject();
      event.object_value["event"] = MakeJsonString("admin_quit");
      coordinator.RecordEvent(std::move(event));
      stop_server();
      return;
    }

    std::cout << "unknown command: " << command << "\n";
  }
}

JsonValue BuildHealthPayload(const BridgeServerCoordinator& coordinator, const BridgeArgs& args) {
  JsonValue root = MakeJsonObject();
  root.object_value["ok"] = MakeJsonBool(true);
  root.object_value["status"] = MakeJsonString("ready");
  root.object_value["model"] = MakeJsonString(coordinator.LoadedModel());
  root.object_value["queue_depth"] = MakeJsonNumber(static_cast<double>(coordinator.QueueDepth()));
  root.object_value["auth_mode"] = MakeJsonString(args.config_mode ? "api_keys" : (args.token.empty() ? "open" : "token"));
  root.object_value["timestamp"] = MakeJsonNumber(static_cast<double>(UnixEpochSecondsNow()));
  return root;
}

bool WriteReadyFileSuccess(const BridgeArgs& args,
                           const std::string& endpoint,
                           const std::string& api_base,
                           const int port,
                           const std::string& model,
                           std::string* error_out) {
  if (args.ready_file.empty()) {
    return true;
  }
  JsonValue payload = MakeJsonObject();
  payload.object_value["ok"] = MakeJsonBool(true);
  payload.object_value["endpoint"] = MakeJsonString(endpoint);
  payload.object_value["api_base"] = MakeJsonString(api_base);
  payload.object_value["host"] = MakeJsonString(args.host);
  payload.object_value["port"] = MakeJsonNumber(static_cast<double>(port));
  payload.object_value["model"] = MakeJsonString(model);
  payload.object_value["token_set"] = MakeJsonBool(!args.token.empty() || args.config_mode);
  payload.object_value["auth_mode"] = MakeJsonString(args.config_mode ? "api_keys" : (!args.token.empty() ? "token" : "open"));
  payload.object_value["pid"] = MakeJsonNumber(static_cast<double>(
#if defined(_WIN32)
      _getpid()
#else
      getpid()
#endif
      ));
  return WriteTextFile(args.ready_file, SerializeJsonCompact(payload) + "\n", error_out);
}

void WriteReadyFileFailure(const BridgeArgs& args, const std::string& error_message) {
  if (args.ready_file.empty()) {
    return;
  }
  JsonValue payload = MakeJsonObject();
  payload.object_value["ok"] = MakeJsonBool(false);
  payload.object_value["error"] = MakeJsonString(error_message);
  std::string ignored;
  WriteTextFile(args.ready_file, SerializeJsonCompact(payload) + "\n", &ignored);
}

template <typename ServerT>
void AttachRoutes(ServerT& server, BridgeServerCoordinator& coordinator, const BridgeArgs& args) {
  server.Get("/healthz", [&](const httplib::Request&, httplib::Response& response) {
    response.set_content(SerializeJsonCompact(BuildHealthPayload(coordinator, args)), "application/json");
  });

  server.Get("/v1/models", [&](const httplib::Request& request, httplib::Response& response) {
    RequestAuthContext auth;
    std::string auth_error;
    std::string auth_code;
    int auth_status = 500;
    if (!AuthorizeRequest(request, args, &auth, &auth_error, &auth_status, &auth_code)) {
      coordinator.RecordAuthFailure(request.remote_addr, request.get_header_value("User-Agent"), auth_error, auth_code);
      SetJsonError(response, auth_status, auth_error, "invalid_request_error", auth_code);
      return;
    }

    std::string list_error;
    std::vector<std::string> models = coordinator.ListModels(&list_error);
    if (!list_error.empty()) {
      SetJsonError(response, 500, list_error, "server_error", "list_models_failed");
      return;
    }
    models = FilterModelsForAuth(auth, models);
    response.set_content(SerializeJsonCompact(BuildModelsResponse(models)), "application/json");
  });

  server.Post("/v1/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
    RequestAuthContext auth;
    std::string auth_error;
    std::string auth_code;
    int auth_status = 500;
    if (!AuthorizeRequest(request, args, &auth, &auth_error, &auth_status, &auth_code)) {
      coordinator.RecordAuthFailure(request.remote_addr, request.get_header_value("User-Agent"), auth_error, auth_code);
      SetJsonError(response, auth_status, auth_error, "invalid_request_error", auth_code);
      return;
    }

    const std::optional<JsonValue> parsed = ParseJson(request.body);
    if (!parsed.has_value() || parsed->type != JsonType::Object) {
      SetJsonError(response,
                   400,
                   "Request body must be valid JSON object.",
                   "invalid_request_error",
                   "invalid_json");
      return;
    }

    JsonValue request_body = parsed.value();
    const bool stream = JsonBoolOrDefault(request_body.Find("stream"), false);

    std::string resolved_model;
    std::string model_error;
    int model_status = 500;
    if (!coordinator.ResolveAuthorizedModel(auth,
                                            NormalizeRequestedModel(JsonStringOrEmpty(request_body.Find("model"))),
                                            &resolved_model,
                                            &model_error,
                                            &model_status)) {
      SetJsonError(response,
                   model_status,
                   model_error.empty() ? std::string("Model is not available for this request.") : model_error,
                   (model_status >= 500) ? "server_error" : "invalid_request_error",
                   (model_status == 403) ? "model_not_allowed" : "invalid_model");
      return;
    }
    if (!resolved_model.empty()) {
      request_body.object_value["model"] = MakeJsonString(resolved_model);
    }

    std::future<QueuedCompletionResult> future =
        coordinator.EnqueueCompletion(std::move(request_body), auth, request.remote_addr, request.get_header_value("User-Agent"));
    QueuedCompletionResult queued_result = future.get();
    if (!queued_result.ok) {
      const std::string code = (queued_result.status >= 500) ? "engine_failure" : "invalid_request";
      SetJsonError(response,
                   queued_result.status,
                   queued_result.error.empty() ? std::string("Unknown bridge generation failure.") : queued_result.error,
                   (queued_result.status >= 500) ? "server_error" : "invalid_request_error",
                   code);
      return;
    }

    if (stream) {
      response.set_header("Cache-Control", "no-cache");
      response.set_header("Connection", "keep-alive");
      response.set_content(BuildStreamingSsePayload(queued_result.completion), "text/event-stream");
      return;
    }

    response.set_content(SerializeJsonCompact(BuildNonStreamingChatResponse(queued_result.completion)), "application/json");
  });
}

template <typename ServerT>
int RunBoundBridgeServer(ServerT& server, BridgeServerCoordinator& coordinator, const BridgeArgs& args, const std::string& scheme) {
  AttachRoutes(server, coordinator, args);
  int bound_port = -1;
  if (args.port == 0) {
    bound_port = server.bind_to_any_port(args.host.c_str());
  } else if (server.bind_to_port(args.host.c_str(), args.port)) {
    bound_port = args.port;
  }
  if (bound_port <= 0) {
    const std::string error = "Failed to bind " + ToLowerAscii(scheme) + " bridge server to " + args.host + ":" +
                              std::to_string(args.port);
    WriteReadyFileFailure(args, error);
    std::cerr << error << "\n";
    return 1;
  }

  const std::string endpoint = scheme + "://" + args.host + ":" + std::to_string(bound_port);
  const std::string api_base = endpoint + "/v1";
  std::string ready_error;
  if (!WriteReadyFileSuccess(args, endpoint, api_base, bound_port, coordinator.LoadedModel(), &ready_error)) {
    std::cerr << ready_error << "\n";
    return 1;
  }

  JsonValue start_event = MakeJsonObject();
  start_event.object_value["event"] = MakeJsonString("server_start");
  start_event.object_value["endpoint"] = MakeJsonString(endpoint);
  start_event.object_value["api_base"] = MakeJsonString(api_base);
  start_event.object_value["model"] = MakeJsonString(coordinator.LoadedModel());
  start_event.object_value["auth_mode"] = MakeJsonString(args.config_mode ? "api_keys" : (!args.token.empty() ? "token" : "open"));
  coordinator.RecordEvent(std::move(start_event));

  std::thread admin_thread;
  if (args.admin_cli) {
    admin_thread = std::thread([&coordinator, &args, &server]() {
      RunAdminCli(coordinator, args, [&server]() { server.stop(); });
    });
  }

  const bool ok = server.listen_after_bind();
  coordinator.Stop();
  if (admin_thread.joinable()) {
    admin_thread.join();
  }

  JsonValue stop_event = MakeJsonObject();
  stop_event.object_value["event"] = MakeJsonString("server_stop");
  stop_event.object_value["status"] = MakeJsonString(ok ? "stopped" : "listen_failed");
  coordinator.RecordEvent(std::move(stop_event));

  if (!ok) {
    std::cerr << scheme << " bridge server exited unexpectedly.\n";
    return 1;
  }
  return 0;
}

int RunBridgeServer(const BridgeArgs& args) {
  ollama_engine::EngineOptions engine_options;
  engine_options.pPathModelFolder = args.model_folder;
  engine_options.piEmbeddingDimensions = 256;

  BridgeServerCoordinator coordinator(engine_options, args.log_file);
  std::string init_error;
  if (!coordinator.Initialize(args.default_model, &init_error)) {
    WriteReadyFileFailure(args, init_error);
    std::cerr << init_error << "\n";
    return 1;
  }

  if (!args.ready_file.empty()) {
    std::error_code ec;
    fs::remove(args.ready_file, ec);
  }

  const bool use_tls = !args.tls_cert.empty() && !args.tls_key.empty();
  const std::string scheme = use_tls ? "https" : "http";

  if (use_tls) {
    httplib::SSLServer server(args.tls_cert.string().c_str(), args.tls_key.string().c_str());
    if (!server.is_valid()) {
      const std::string error = "Failed to initialize HTTPS bridge server (invalid cert/key).";
      WriteReadyFileFailure(args, error);
      std::cerr << error << "\n";
      return 1;
    }
    return RunBoundBridgeServer(server, coordinator, args, scheme);
  }

  httplib::Server server;
  return RunBoundBridgeServer(server, coordinator, args, scheme);
}

}  // namespace

int main(int argc, char** argv) {
  BridgeArgs args;
  std::string parse_error;
  if (!ParseArgs(argc, argv, &args, &parse_error)) {
    if (!parse_error.empty()) {
      std::cerr << parse_error << "\n";
      PrintUsage();
      return 2;
    }
    return 0;
  }

  return RunBridgeServer(args);
}
