#include "provider_runtime.h"

#include "gemini_command_builder.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

bool AnyTypeMatches(const std::vector<std::string>& types, const std::string& value) {
  for (const std::string& candidate : types) {
    if (EqualsIgnoreCase(candidate, value)) {
      return true;
    }
  }
  return false;
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

std::string JoinFlags(const std::vector<std::string>& flags) {
  std::ostringstream out;
  bool first = true;
  for (const std::string& flag : flags) {
    if (flag.empty()) {
      continue;
    }
    if (!first) {
      out << ' ';
    }
    out << flag;
    first = false;
  }
  return out.str();
}

AppSettings MergeProviderSettings(const ProviderProfile& profile, const AppSettings& settings) {
  AppSettings merged = settings;
  if (!profile.command_template.empty()) {
    merged.gemini_command_template = profile.command_template;
  }

  const std::string provider_flags = JoinFlags(profile.runtime_flags);
  if (!provider_flags.empty() && merged.gemini_extra_flags.empty()) {
    merged.gemini_extra_flags = provider_flags;
  } else if (!provider_flags.empty()) {
    merged.gemini_extra_flags = provider_flags + " " + merged.gemini_extra_flags;
  }
  return merged;
}

}  // namespace

std::string ProviderRuntime::BuildPrompt(const ProviderProfile&,
                                         const std::string& user_prompt,
                                         const std::vector<std::string>& files) {
  return GeminiCommandBuilder::BuildPrompt(user_prompt, files);
}

std::string ProviderRuntime::BuildCommand(const ProviderProfile& profile,
                                          const AppSettings& settings,
                                          const std::string& prompt,
                                          const std::vector<std::string>& files,
                                          const std::string& resume_session_id) {
  const AppSettings provider_settings = MergeProviderSettings(profile, settings);
  const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
  return GeminiCommandBuilder::BuildCommand(provider_settings, prompt, files, effective_resume_session_id);
}

std::vector<std::string> ProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile,
                                                               const ChatSession& chat,
                                                               const AppSettings& settings) {
  const AppSettings provider_settings = MergeProviderSettings(profile, settings);
  if ((profile.interactive_command.empty() || EqualsIgnoreCase(profile.interactive_command, "gemini")) &&
      (profile.resume_argument.empty() || profile.resume_argument == "--resume") &&
      profile.supports_resume) {
    return GeminiCommandBuilder::BuildInteractiveArgv(chat, provider_settings);
  }

  std::vector<std::string> argv = SplitShellWords(profile.interactive_command);
  if (argv.empty()) {
    argv.push_back("gemini");
  }
  if (provider_settings.gemini_yolo_mode) {
    argv.push_back("--yolo");
  }
  const std::vector<std::string> extra_flags = SplitShellWords(provider_settings.gemini_extra_flags);
  argv.insert(argv.end(), extra_flags.begin(), extra_flags.end());
  if (profile.supports_resume && chat.uses_native_session && !chat.native_session_id.empty() && !profile.resume_argument.empty()) {
    argv.push_back(profile.resume_argument);
    argv.push_back(chat.native_session_id);
  }
  return argv;
}

MessageRole ProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) {
  if (AnyTypeMatches(profile.user_message_types, native_type)) {
    return MessageRole::User;
  }
  if (AnyTypeMatches(profile.assistant_message_types, native_type)) {
    return MessageRole::Assistant;
  }
  return MessageRole::System;
}

bool ProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile& profile) {
  return EqualsIgnoreCase(profile.history_adapter, "gemini-cli-json");
}
