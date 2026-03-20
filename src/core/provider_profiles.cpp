#include "provider_profiles.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace uam {
namespace {

std::string Trim(std::string value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool StartsWithComment(const std::string& line) {
  return !line.empty() && (line[0] == '#' || line[0] == ';');
}

bool IsSectionLine(const std::string& line) {
  return line.size() >= 2 && line.front() == '[' && line.back() == ']';
}

std::string SectionName(const std::string& line) {
  if (!IsSectionLine(line)) {
    return "";
  }
  return ToLower(Trim(line.substr(1, line.size() - 2)));
}

bool ParseBool(const std::string& raw_value, const bool fallback) {
  const std::string value = ToLower(Trim(raw_value));
  if (value.empty()) {
    return fallback;
  }
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return fallback;
}

std::string UnescapeQuotedValue(const std::string& raw_value) {
  if (raw_value.size() < 2) {
    return raw_value;
  }
  const char quote = raw_value.front();
  if ((quote != '"' && quote != '\'') || raw_value.back() != quote) {
    return raw_value;
  }

  std::string out;
  out.reserve(raw_value.size() - 2);
  for (std::size_t i = 1; i + 1 < raw_value.size(); ++i) {
    const char ch = raw_value[i];
    if (ch != '\\' || i + 1 >= raw_value.size() - 1) {
      out.push_back(ch);
      continue;
    }
    const char next = raw_value[++i];
    switch (next) {
      case '\\':
      case '"':
      case '\'':
      case '=':
      case ',':
        out.push_back(next);
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        out.push_back(next);
        break;
    }
  }
  return out;
}

std::string ParseScalarValue(const std::string& raw_value) {
  const std::string trimmed = Trim(raw_value);
  return UnescapeQuotedValue(trimmed);
}

std::vector<std::string> SplitListValue(const std::string& raw_value) {
  std::vector<std::string> values;
  std::string current;
  bool in_quotes = false;
  char quote = '\0';
  bool escaping = false;

  auto flush_current = [&]() {
    std::string item = Trim(current);
    if (!item.empty()) {
      values.push_back(ParseScalarValue(item));
    }
    current.clear();
  };

  for (char ch : raw_value) {
    if (escaping) {
      current.push_back('\\');
      current.push_back(ch);
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (in_quotes) {
      current.push_back(ch);
      if (ch == quote) {
        in_quotes = false;
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      in_quotes = true;
      quote = ch;
      current.push_back(ch);
      continue;
    }
    if (ch == ',') {
      flush_current();
      continue;
    }
    current.push_back(ch);
  }

  if (escaping) {
    current.push_back('\\');
  }
  flush_current();
  return values;
}

std::string EscapeValue(const std::string& value) {
  if (value.empty()) {
    return "\"\"";
  }

  const bool needs_quotes = value.find_first_of(" \t\r\n#;=,\"'") != std::string::npos ||
                            value.front() == ' ' || value.back() == ' ';
  if (!needs_quotes) {
    return value;
  }

  std::string out;
  out.push_back('"');
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string BoolToText(const bool value) {
  return value ? "true" : "false";
}

void AppendError(std::string* error_out, const std::string& message) {
  if (error_out == nullptr) {
    return;
  }
  if (error_out->empty()) {
    *error_out = message;
  } else {
    *error_out += "\n";
    *error_out += message;
  }
}

bool IsValidProfile(const ProviderProfile& profile, std::string* error_out) {
  bool ok = true;
  if (Trim(profile.id).empty()) {
    AppendError(error_out, "Profile is missing an id.");
    ok = false;
  }
  if (Trim(profile.display_name).empty()) {
    AppendError(error_out, "Profile '" + profile.id + "' is missing a display_name.");
    ok = false;
  }
  if (Trim(profile.command_template).empty()) {
    AppendError(error_out, "Profile '" + profile.id + "' is missing a command_template.");
    ok = false;
  }
  return ok;
}

ProviderProfile FinalizeProfile(ProviderProfile profile) {
  profile.id = Trim(profile.id);
  profile.display_name = Trim(profile.display_name);
  profile.command_template = Trim(profile.command_template);
  profile.runtime_flags.erase(
      std::remove_if(profile.runtime_flags.begin(), profile.runtime_flags.end(),
                     [](const std::string& flag) { return Trim(flag).empty(); }),
      profile.runtime_flags.end());
  if (profile.display_name.empty()) {
    profile.display_name = profile.id;
  }
  return profile;
}

void WriteProfileBlock(std::ostream& out, const ProviderProfile& profile) {
  out << "[profile]\n";
  out << "id=" << EscapeValue(profile.id) << "\n";
  out << "display_name=" << EscapeValue(profile.display_name) << "\n";
  out << "command_template=" << EscapeValue(profile.command_template) << "\n";
  out << "supports_resume=" << BoolToText(profile.supports_resume) << "\n";
  for (const std::string& flag : profile.runtime_flags) {
    out << "runtime_flag=" << EscapeValue(flag) << "\n";
  }
  out << "\n";
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    return false;
  }
  out << content;
  return out.good();
}

std::vector<ProviderProfile> ParseProfilesText(const std::string& text, std::string* error_out) {
  std::vector<ProviderProfile> profiles;
  ProviderProfile current;
  bool in_profile = false;
  std::string parse_errors;

  auto finalize_current = [&]() {
    if (!in_profile) {
      return;
    }
    ProviderProfile finalized = FinalizeProfile(current);
    if (IsValidProfile(finalized, &parse_errors)) {
      profiles.push_back(std::move(finalized));
    }
    current = ProviderProfile{};
  };

  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF && line.size() >= 3 &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
      line = line.substr(3);
    }
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || StartsWithComment(trimmed)) {
      continue;
    }

    if (IsSectionLine(trimmed)) {
      finalize_current();
      const std::string section = SectionName(trimmed);
      in_profile = (section == "profile" || section == "provider");
      continue;
    }

    const auto equals_at = trimmed.find('=');
    if (equals_at == std::string::npos) {
      AppendError(&parse_errors, "Ignoring malformed line: " + trimmed);
      continue;
    }

    const std::string key = ToLower(Trim(trimmed.substr(0, equals_at)));
    const std::string value = ParseScalarValue(trimmed.substr(equals_at + 1));

    if (key == "version") {
      continue;
    }
    if (!in_profile) {
      AppendError(&parse_errors, "Ignoring key outside a [profile] block: " + key);
      continue;
    }
    if (key == "id") {
      current.id = value;
    } else if (key == "display_name" || key == "name") {
      current.display_name = value;
    } else if (key == "command_template") {
      current.command_template = value;
    } else if (key == "supports_resume" || key == "resume") {
      current.supports_resume = ParseBool(value, current.supports_resume);
    } else if (key == "runtime_flag" || key == "runtime_flags") {
      const std::vector<std::string> parsed_flags = (key == "runtime_flags") ? SplitListValue(value)
                                                                             : std::vector<std::string>{value};
      for (const std::string& flag : parsed_flags) {
        if (!Trim(flag).empty()) {
          current.runtime_flags.push_back(flag);
        }
      }
    } else {
      // Unknown keys are ignored so the format can grow without breaking older builds.
    }
  }

  finalize_current();

  if (profiles.empty() && !parse_errors.empty()) {
    AppendError(error_out, parse_errors);
  } else if (!parse_errors.empty() && error_out != nullptr) {
    *error_out = parse_errors;
  }
  return profiles;
}

std::string SerializeProfilesText(const std::vector<ProviderProfile>& profiles) {
  std::ostringstream out;
  out << "# Universal Agent Manager provider profiles\n";
  out << "# Hand-editable format.\n";
  out << "# Sections start with [profile].\n";
  out << "# Supported keys: id, display_name, command_template, supports_resume, runtime_flag, runtime_flags\n";
  out << "version=1\n\n";
  for (const ProviderProfile& profile : profiles) {
    WriteProfileBlock(out, FinalizeProfile(profile));
  }
  return out.str();
}

}  // namespace

std::filesystem::path ProviderProfileStore::ProfilesFilePath(const std::filesystem::path& data_root) {
  return data_root / "provider_profiles.txt";
}

ProviderProfile ProviderProfileStore::DefaultGeminiProfile() {
  return ProviderProfile{
      "gemini",
      "Gemini CLI",
      "gemini {resume} {flags} {prompt}",
      true,
      {}};
}

std::vector<ProviderProfile> ProviderProfileStore::DefaultProfiles() {
  return {DefaultGeminiProfile()};
}

std::vector<ProviderProfile> ProviderProfileStore::LoadUserProfiles(const std::filesystem::path& file_path,
                                                                    std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  const std::string text = ReadTextFile(file_path);
  if (text.empty()) {
    return {};
  }
  return ParseProfilesText(text, error_out);
}

bool ProviderProfileStore::SaveUserProfiles(const std::filesystem::path& file_path,
                                            const std::vector<ProviderProfile>& profiles,
                                            std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  std::error_code ec;
  const std::filesystem::path parent = file_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
  }
  if (ec) {
    AppendError(error_out, "Failed to create directory '" + parent.string() + "': " + ec.message());
    return false;
  }

  const std::string text = SerializeProfilesText(profiles);
  if (!WriteTextFile(file_path, text)) {
    AppendError(error_out, "Failed to write provider profile file '" + file_path.string() + "'.");
    return false;
  }
  return true;
}

std::vector<ProviderProfile> ProviderProfileStore::MergeProfiles(const std::vector<ProviderProfile>& base_profiles,
                                                                const std::vector<ProviderProfile>& overlay_profiles) {
  std::vector<ProviderProfile> merged = base_profiles;
  for (const ProviderProfile& overlay : overlay_profiles) {
    const std::string id = Trim(overlay.id);
    if (id.empty()) {
      continue;
    }
    auto it = std::find_if(merged.begin(), merged.end(), [&](const ProviderProfile& profile) {
      return Trim(profile.id) == id;
    });
    if (it == merged.end()) {
      merged.push_back(overlay);
    } else {
      *it = overlay;
    }
  }
  return merged;
}

std::vector<ProviderProfile> ProviderProfileStore::LoadProfiles(const std::filesystem::path& data_root,
                                                                std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  const std::filesystem::path file_path = ProfilesFilePath(data_root);
  const std::vector<ProviderProfile> user_profiles = LoadUserProfiles(file_path, error_out);
  return MergeProfiles(DefaultProfiles(), user_profiles);
}

bool ProviderProfileStore::SaveProfiles(const std::filesystem::path& data_root,
                                        const std::vector<ProviderProfile>& profiles,
                                        std::string* error_out) {
  return SaveUserProfiles(ProfilesFilePath(data_root), profiles, error_out);
}

const ProviderProfile* ProviderProfileStore::FindById(const std::vector<ProviderProfile>& profiles,
                                                      const std::string& provider_id) {
  const std::string needle = Trim(provider_id);
  if (needle.empty()) {
    return nullptr;
  }
  const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const ProviderProfile& profile) {
    return Trim(profile.id) == needle;
  });
  return (it == profiles.end()) ? nullptr : &(*it);
}

}  // namespace uam
