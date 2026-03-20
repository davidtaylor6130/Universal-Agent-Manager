#include "provider_profile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {
namespace fs = std::filesystem;

fs::path ProviderFilePath(const fs::path& data_root) {
  return data_root / "providers.txt";
}

bool WriteTextFile(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    return false;
  }
  out << content;
  return out.good();
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string Trim(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::vector<std::string> SplitCsv(const std::string& csv) {
  std::vector<std::string> out;
  std::istringstream in(csv);
  std::string token;
  while (std::getline(in, token, ',')) {
    token = Trim(token);
    if (!token.empty()) {
      out.push_back(token);
    }
  }
  return out;
}

std::string JoinCsv(const std::vector<std::string>& values) {
  std::ostringstream out;
  bool first = true;
  for (const std::string& value : values) {
    if (!first) {
      out << ",";
    }
    out << value;
    first = false;
  }
  return out.str();
}

bool IsSameId(const std::string& lhs, const std::string& rhs) {
  auto lower = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return value;
  };
  return lower(lhs) == lower(rhs);
}

bool ParseBool(const std::string& value) {
  std::string lowered = Trim(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes";
}

}  // namespace

ProviderProfile ProviderProfileStore::DefaultGeminiProfile() {
  ProviderProfile profile;
  profile.id = "gemini";
  profile.title = "Gemini CLI";
  profile.command_template = "gemini {resume} {flags} {prompt}";
  profile.interactive_command = "gemini";
  profile.supports_resume = true;
  profile.resume_argument = "--resume";
  profile.history_adapter = "gemini-cli-json";
  profile.user_message_types = {"user"};
  profile.assistant_message_types = {"assistant", "model", "gemini"};
  return profile;
}

void ProviderProfileStore::EnsureDefaultProfile(std::vector<ProviderProfile>& profiles) {
  for (const ProviderProfile& profile : profiles) {
    if (IsSameId(profile.id, "gemini")) {
      return;
    }
  }
  profiles.push_back(DefaultGeminiProfile());
}

const ProviderProfile* ProviderProfileStore::FindById(const std::vector<ProviderProfile>& profiles, const std::string& id) {
  for (const ProviderProfile& profile : profiles) {
    if (IsSameId(profile.id, id)) {
      return &profile;
    }
  }
  return nullptr;
}

ProviderProfile* ProviderProfileStore::FindById(std::vector<ProviderProfile>& profiles, const std::string& id) {
  for (ProviderProfile& profile : profiles) {
    if (IsSameId(profile.id, id)) {
      return &profile;
    }
  }
  return nullptr;
}

std::vector<ProviderProfile> ProviderProfileStore::Load(const std::filesystem::path& data_root) {
  std::vector<ProviderProfile> profiles;
  const fs::path file = ProviderFilePath(data_root);
  if (!fs::exists(file)) {
    profiles.push_back(DefaultGeminiProfile());
    return profiles;
  }

  std::istringstream lines(ReadTextFile(file));
  std::string line;
  ProviderProfile current;
  bool in_profile = false;
  while (std::getline(lines, line)) {
    if (line == "[provider]") {
      if (in_profile && !Trim(current.id).empty()) {
        profiles.push_back(current);
      }
      current = ProviderProfile{};
      in_profile = true;
      continue;
    }
    if (!in_profile || line.empty()) {
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, eq));
    const std::string value = Trim(line.substr(eq + 1));
    if (key == "id") {
      current.id = value;
    } else if (key == "title") {
      current.title = value;
    } else if (key == "command_template") {
      current.command_template = value;
    } else if (key == "interactive_command") {
      current.interactive_command = value;
    } else if (key == "supports_resume") {
      current.supports_resume = ParseBool(value);
    } else if (key == "runtime_flag") {
      if (!value.empty()) {
        current.runtime_flags.push_back(value);
      }
    } else if (key == "runtime_flags") {
      std::vector<std::string> flags = SplitCsv(value);
      current.runtime_flags.insert(current.runtime_flags.end(), flags.begin(), flags.end());
    } else if (key == "resume_argument") {
      current.resume_argument = value;
    } else if (key == "history_adapter") {
      current.history_adapter = value;
    } else if (key == "user_types") {
      current.user_message_types = SplitCsv(value);
    } else if (key == "assistant_types") {
      current.assistant_message_types = SplitCsv(value);
    }
  }
  if (in_profile && !Trim(current.id).empty()) {
    profiles.push_back(current);
  }

  if (profiles.empty()) {
    profiles.push_back(DefaultGeminiProfile());
  }
  EnsureDefaultProfile(profiles);
  return profiles;
}

bool ProviderProfileStore::Save(const std::filesystem::path& data_root, const std::vector<ProviderProfile>& profiles) {
  std::error_code ec;
  fs::create_directories(data_root, ec);

  std::ostringstream out;
  for (const ProviderProfile& profile : profiles) {
    if (Trim(profile.id).empty()) {
      continue;
    }
    out << "[provider]\n";
    out << "id=" << profile.id << "\n";
    out << "title=" << profile.title << "\n";
    out << "command_template=" << profile.command_template << "\n";
    out << "interactive_command=" << profile.interactive_command << "\n";
    out << "supports_resume=" << (profile.supports_resume ? "1" : "0") << "\n";
    out << "runtime_flags=" << JoinCsv(profile.runtime_flags) << "\n";
    out << "resume_argument=" << profile.resume_argument << "\n";
    out << "history_adapter=" << profile.history_adapter << "\n";
    out << "user_types=" << JoinCsv(profile.user_message_types) << "\n";
    out << "assistant_types=" << JoinCsv(profile.assistant_message_types) << "\n\n";
  }
  return WriteTextFile(ProviderFilePath(data_root), out.str());
}
