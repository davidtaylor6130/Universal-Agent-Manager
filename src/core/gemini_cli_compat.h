#pragma once

#include <span>
#include <string>
#include <string_view>

namespace uam {

std::span<const std::string_view> SupportedGeminiCliVersions();
std::string_view PreferredGeminiCliVersion();
bool IsSupportedGeminiCliVersion(std::string_view version);
std::string SupportedGeminiCliVersionsLabel();

}  // namespace uam
