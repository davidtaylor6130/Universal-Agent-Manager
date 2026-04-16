#include "core/gemini_cli_compat.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace uam {
namespace {

constexpr std::array<std::string_view, 2> kSupportedGeminiCliVersions{{
    "0.38.1",
    "0.36.0",
}};

struct Semver {
  int major = -1;
  int minor = -1;
  int patch = -1;
};

bool ParseSemver(const std::string_view value, Semver& out) {
  const std::string text(value);
  char* end = nullptr;
  const long major = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '.') {
    return false;
  }

  const long minor = std::strtol(end + 1, &end, 10);
  if (end == nullptr || *end != '.') {
    return false;
  }

  const long patch = std::strtol(end + 1, &end, 10);
  if (major < 0 || minor < 0 || patch < 0) {
    return false;
  }

  out.major = static_cast<int>(major);
  out.minor = static_cast<int>(minor);
  out.patch = static_cast<int>(patch);
  return true;
}

}  // namespace

std::span<const std::string_view> SupportedGeminiCliVersions() {
  return std::span<const std::string_view>(kSupportedGeminiCliVersions.data(), kSupportedGeminiCliVersions.size());
}

std::string_view PreferredGeminiCliVersion() {
  return kSupportedGeminiCliVersions.front();
}

bool IsSupportedGeminiCliVersion(const std::string_view version) {
  if (std::find(kSupportedGeminiCliVersions.begin(), kSupportedGeminiCliVersions.end(), version) !=
      kSupportedGeminiCliVersions.end()) {
    return true;
  }

  Semver parsed;
  if (!ParseSemver(version, parsed)) {
    return false;
  }

  if (parsed.major > 0) {
    return true;
  }

  return parsed.major == 0 && parsed.minor >= 36;
}

std::string SupportedGeminiCliVersionsLabel() {
  return "0.36.0 or newer (preferred 0.38.1)";
}

}  // namespace uam
