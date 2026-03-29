#include "chat_import_utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

std::string Trim(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string CompactTitle(std::string value, const std::size_t max_length) {
  value = Trim(value);
  if (max_length == 0 || value.size() <= max_length) {
    return value;
  }
  if (max_length <= 3) {
    return value.substr(0, max_length);
  }
  return value.substr(0, max_length - 3) + "...";
}

std::string StripPromptWrappers(std::string value) {
  value = Trim(value);
  if (value.empty()) {
    return value;
  }

  static constexpr const char* kUserPromptLabel = "User prompt:";
  const std::size_t user_prompt_pos = value.rfind(kUserPromptLabel);
  if (user_prompt_pos != std::string::npos) {
    value = value.substr(user_prompt_pos + std::char_traits<char>::length(kUserPromptLabel));
  }

  if (StartsWith(value, "@.gemini/gemini.md")) {
    const std::size_t newline = value.find('\n');
    if (newline != std::string::npos) {
      value = value.substr(newline + 1);
    } else {
      value.clear();
    }
  }

  std::istringstream lines(value);
  std::string line;
  while (std::getline(lines, line)) {
    line = Trim(line);
    if (!line.empty()) {
      return line;
    }
  }
  return Trim(value);
}

}  // namespace

namespace uam {

std::string BuildImportedChatTitle(const std::vector<Message>& messages,
                                   const std::string& created_at,
                                   const std::size_t max_length) {
  for (const Message& message : messages) {
    if (message.role != MessageRole::User) {
      continue;
    }
    const std::string prompt = StripPromptWrappers(message.content);
    if (!prompt.empty()) {
      return CompactTitle(prompt, max_length);
    }
  }

  const std::string fallback = created_at.empty() ? "Untitled Chat" : ("Session " + created_at);
  return CompactTitle(fallback, max_length);
}

std::string BuildFolderTitleFromProjectRoot(const std::filesystem::path& project_root) {
  const std::filesystem::path normalized = project_root.lexically_normal();
  std::string title = Trim(normalized.filename().string());
  if (!title.empty()) {
    return title;
  }
  title = Trim(normalized.stem().string());
  if (!title.empty()) {
    return title;
  }
  title = Trim(normalized.generic_string());
  return title.empty() ? "Imported Gemini" : title;
}

bool ImportedProjectRootExists(const std::filesystem::path& project_root) {
  if (project_root.empty()) {
    return false;
  }
  const std::filesystem::path normalized = project_root.lexically_normal();
  std::error_code ec;
  return std::filesystem::exists(normalized, ec) && !ec &&
         std::filesystem::is_directory(normalized, ec) && !ec;
}

std::filesystem::path ResolveImportedProjectRootOrFallback(const std::filesystem::path& project_root,
                                                           const std::filesystem::path& fallback_root) {
  if (ImportedProjectRootExists(project_root)) {
    return project_root.lexically_normal();
  }
  if (fallback_root.empty()) {
    return {};
  }
  return fallback_root.lexically_normal();
}

}  // namespace uam
