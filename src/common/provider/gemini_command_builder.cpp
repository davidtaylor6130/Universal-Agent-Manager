#include "gemini_command_builder.h"
#include "command_line_words.h"

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
#if defined(_WIN32)
  std::string escaped = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else if (ch == '%') {
      escaped += "%%";
    } else if (ch == '\r' || ch == '\n') {
      escaped.push_back(' ');
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('"');
  return escaped;
#else
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
#endif
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
  if (settings.provider_yolo_mode) {
    flags.push_back("--yolo");
  }
  const std::vector<std::string> extra_flags = SplitCommandLineWords(settings.provider_extra_flags);
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
  std::string command = "gemini -r \"{resume}\" {flags} -p {prompt}";

  const std::string resume_fragment = resume_session_id.empty() ? "" : (ShellEscape(resume_session_id));
  const std::string flags_fragment = BuildFlagsShell(settings);
  const std::string model_fragment = settings.selected_model_id.empty() ? "" : ShellEscape(settings.selected_model_id);

  command = ReplaceAll(command, "{prompt}", ShellEscape(prompt));
  command = ReplaceAll(command, "{files}", JoinShellEscapedFiles(files));
  command = ReplaceAll(command, "{resume}", resume_fragment);
  command = ReplaceAll(command, "{flags}", flags_fragment);
  command = ReplaceAll(command, "{model}", model_fragment);

  if (!command.find("{prompt}")) {
    command += " ";
    command += ShellEscape(prompt);
  }
  if (!command.find("{resume}")) {
    command += " ";
    command += resume_fragment;
  }
  if (!command.find("{flags}")) {
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
