#include "core/gemini_cli_compat.h"

#include <algorithm>
#include <array>

namespace uam {
namespace {

constexpr std::array<std::string_view, 2> kSupportedGeminiCliVersions{{
    "0.34.0",
    "0.30.0",
}};

}  // namespace

std::span<const std::string_view> SupportedGeminiCliVersions() {
  return std::span<const std::string_view>(kSupportedGeminiCliVersions.data(), kSupportedGeminiCliVersions.size());
}

std::string_view PreferredGeminiCliVersion() {
  return kSupportedGeminiCliVersions.front();
}

bool IsSupportedGeminiCliVersion(const std::string_view version) {
  return std::find(kSupportedGeminiCliVersions.begin(), kSupportedGeminiCliVersions.end(), version) !=
         kSupportedGeminiCliVersions.end();
}

std::string SupportedGeminiCliVersionsLabel() {
  std::string label;
  for (const std::string_view version : kSupportedGeminiCliVersions) {
    if (!label.empty()) {
      label += ", ";
    }
    label.append(version.begin(), version.end());
  }
  return label;
}

}  // namespace uam
