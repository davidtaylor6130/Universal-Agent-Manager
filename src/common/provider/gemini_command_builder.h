#pragma once

#include "app_models.h"

#include <string>
#include <vector>

/// <summary>
/// Builds Gemini CLI prompt and command strings from app/runtime settings.
/// </summary>
class GeminiCommandBuilder {
 public:
  /// <summary>Builds the Gemini prompt body with linked-file context.</summary>
  static std::string BuildPrompt(const std::string& user_prompt,
                                 const std::vector<std::string>& files);

  /// <summary>Builds the shell command used for one Gemini invocation.</summary>
  static std::string BuildCommand(const AppSettings& settings,
                                  const std::string& prompt,
                                  const std::vector<std::string>& files,
                                  const std::string& resume_session_id);

  /// <summary>Builds argv for interactive Gemini terminal sessions.</summary>
  static std::vector<std::string> BuildInteractiveArgv(const ChatSession& chat,
                                                       const AppSettings& settings);

 private:
  static std::vector<std::string> BuildFlagsArgv(const AppSettings& settings);
  static std::string BuildFlagsShell(const AppSettings& settings);
};
