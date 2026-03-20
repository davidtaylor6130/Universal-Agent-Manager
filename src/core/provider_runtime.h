#pragma once

#include "app_models.h"
#include "provider_profile.h"

#include <string>
#include <vector>

class ProviderRuntime {
 public:
  static std::string BuildPrompt(const ProviderProfile& profile,
                                 const std::string& user_prompt,
                                 const std::vector<std::string>& files);

  static std::string BuildCommand(const ProviderProfile& profile,
                                  const AppSettings& settings,
                                  const std::string& prompt,
                                  const std::vector<std::string>& files,
                                  const std::string& resume_session_id);

  static std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile,
                                                       const ChatSession& chat,
                                                       const AppSettings& settings);

  static MessageRole RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type);

  static bool SupportsGeminiJsonHistory(const ProviderProfile& profile);
};

