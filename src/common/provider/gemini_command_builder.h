#pragma once

#include "app_models.h"

#include <string>
#include <vector>

/// <summary>
/// Builds Gemini CLI prompt and command strings from app/runtime settings.
/// </summary>
class GeminiCommandBuilder {
public:
  static std::string BuildPrompt(const std::string& user_prompt, const std::vector<std::string>& files);
  static std::string BuildCommand(const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id);
  static std::vector<std::string> BuildInteractiveArgv(const ChatSession& chat, const AppSettings& settings);

 private:
  static std::vector<std::string> BuildFlagsArgv(const AppSettings& settings);
  static std::string BuildFlagsShell(const AppSettings& settings);
};
