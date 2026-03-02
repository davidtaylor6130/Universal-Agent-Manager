#include "gemini_command_builder.h"

#include <cctype>
#include <sstream>

namespace {

std::string ReplaceAll(std::string src, const std::string& from, const std::string& to) {
  if (from.empty()) {
    return src;
  }
  std::size_t pos = 0;
  while ((pos = src.find(from, pos)) != std::string::npos) {
    src.replace(pos, from.size(), to);
    pos += to.size();
  }
  return src;
}

std::string ShellEscape(const std::string& value) {
  std::string escaped = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('\'');
  return escaped;
}

std::string JoinShellEscapedFiles(const std::vector<std::string>& files) {
  std::ostringstream out;
  bool first = true;
  for (const std::string& file : files) {
    if (!first) {
      out << ' ';
    }
    out << ShellEscape(file);
    first = false;
  }
  return out.str();
}

std::vector<std::string> SplitShellWords(const std::string& value) {
  std::vector<std::string> words;
  std::string current;
  bool escaping = false;
  char quote = '\0';
  for (char ch : value) {
    if (escaping) {
      current.push_back(ch);
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (quote != '\0') {
      if (ch == quote) {
        quote = '\0';
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (escaping) {
    current.push_back('\\');
  }
  if (!current.empty()) {
    words.push_back(current);
  }
  return words;
}

}  // namespace

std::string GeminiCommandBuilder::BuildPrompt(const std::string& user_prompt,
                                              const std::vector<std::string>& files) {
  std::ostringstream prompt;
  prompt << user_prompt;
  if (!files.empty()) {
    prompt << "\n\nReferenced files:\n";
    for (const std::string& file : files) {
      prompt << "- " << file << "\n";
    }
  }
  return prompt.str();
}

std::vector<std::string> GeminiCommandBuilder::BuildFlagsArgv(const AppSettings& settings) {
  std::vector<std::string> flags;
  if (settings.gemini_yolo_mode) {
    flags.push_back("--yolo");
  }
  const std::vector<std::string> extra_flags = SplitShellWords(settings.gemini_extra_flags);
  flags.insert(flags.end(), extra_flags.begin(), extra_flags.end());
  return flags;
}

std::string GeminiCommandBuilder::BuildFlagsShell(const AppSettings& settings) {
  const std::vector<std::string> flags = BuildFlagsArgv(settings);
  std::ostringstream out;
  bool first = true;
  for (const std::string& flag : flags) {
    if (!first) {
      out << ' ';
    }
    out << ShellEscape(flag);
    first = false;
  }
  return out.str();
}

std::string GeminiCommandBuilder::BuildCommand(const AppSettings& settings,
                                               const std::string& prompt,
                                               const std::vector<std::string>& files,
                                               const std::string& resume_session_id) {
  std::string command = settings.gemini_command_template;
  const bool has_prompt_placeholder = command.find("{prompt}") != std::string::npos;
  const bool has_resume_placeholder = command.find("{resume}") != std::string::npos;
  const bool has_flags_placeholder = command.find("{flags}") != std::string::npos;
  const std::string resume_fragment = resume_session_id.empty() ? "" : ("--resume " + ShellEscape(resume_session_id));
  const std::string flags_fragment = BuildFlagsShell(settings);

  command = ReplaceAll(command, "{prompt}", ShellEscape(prompt));
  command = ReplaceAll(command, "{files}", JoinShellEscapedFiles(files));
  command = ReplaceAll(command, "{resume}", resume_fragment);
  command = ReplaceAll(command, "{flags}", flags_fragment);

  if (!has_prompt_placeholder) {
    command += " ";
    command += ShellEscape(prompt);
  }
  if (!has_resume_placeholder && !resume_fragment.empty()) {
    command += " ";
    command += resume_fragment;
  }
  if (!has_flags_placeholder && !flags_fragment.empty()) {
    command += " ";
    command += flags_fragment;
  }

  return command;
}

std::vector<std::string> GeminiCommandBuilder::BuildInteractiveArgv(const ChatSession& chat,
                                                                    const AppSettings& settings) {
  std::vector<std::string> argv;
  argv.push_back("gemini");
  const std::vector<std::string> flags = BuildFlagsArgv(settings);
  argv.insert(argv.end(), flags.begin(), flags.end());
  if (chat.uses_native_session && !chat.native_session_id.empty()) {
    argv.push_back("--resume");
    argv.push_back(chat.native_session_id);
  }
  return argv;
}
