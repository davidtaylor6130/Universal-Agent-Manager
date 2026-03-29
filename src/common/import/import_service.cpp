#include "import_service.h"

#include "chat_folder_store.h"
#include "chat_repository.h"
#include "constants/app_constants.h"
#include "gemini_template_catalog.h"
#include "paths/app_paths.h"
#include "provider_runtime.h"
#include "runtime/json_runtime.h"
#include "settings_store.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace uam {
namespace {
namespace fs = std::filesystem;

std::string Trim(std::string value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string TimestampNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &tt);
#else
  localtime_r(&tt, &tm_snapshot);
#endif
  std::ostringstream out;
  out << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
  return out.str();
}

std::string NewImportedChatId() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> hex_digit(0, 15);
  std::ostringstream id;
  id << "chat-" << epoch_ms << "-";
  for (int i = 0; i < 6; ++i) {
    id << std::hex << hex_digit(rng);
  }
  return id.str();
}

std::string NewImportedFolderId() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::ostringstream id;
  id << "folder-" << epoch_ms << "-" << (epoch_ms % 997);
  return id.str();
}

std::string NormalizePathString(const fs::path& path) {
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(path, ec);
  return (ec ? path.lexically_normal() : canonical.lexically_normal()).generic_string();
}

std::string ReadFileText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void EnsureDefaultFolder(std::vector<ChatFolder>& folders) {
  const auto it = std::find_if(folders.begin(), folders.end(), [](const ChatFolder& folder) {
    return folder.id == constants::kDefaultFolderId;
  });
  if (it != folders.end()) {
    return;
  }
  ChatFolder folder;
  folder.id = constants::kDefaultFolderId;
  folder.title = constants::kDefaultFolderTitle;
  folder.directory = fs::current_path().string();
  folders.push_back(std::move(folder));
}

std::string MapLegacyProviderId(const std::string& provider_id, const bool prefer_cli_for_gemini = false) {
  const std::string lowered = ToLowerAscii(Trim(provider_id));
  if (lowered == "gemini") {
    return prefer_cli_for_gemini ? "gemini-cli" : "gemini-structured";
  }
  if (lowered == "codex") {
    return "codex-cli";
  }
  if (lowered == "claude") {
    return "claude-cli";
  }
  if (lowered == "opencode") {
    return "opencode-cli";
  }
  return Trim(provider_id);
}

ChatSession* FindImportedChat(std::vector<ChatSession>& chats, const std::string& kind, const std::string& ref) {
  for (ChatSession& chat : chats) {
    if (chat.import_source_kind == kind && chat.import_source_ref == ref) {
      return &chat;
    }
  }
  return nullptr;
}

ChatFolder* FindImportedFolder(std::vector<ChatFolder>& folders, const std::string& kind, const std::string& ref) {
  for (ChatFolder& folder : folders) {
    if (folder.import_source_kind == kind && folder.import_source_ref == ref) {
      return &folder;
    }
  }
  return nullptr;
}

std::vector<fs::path> CollectGeminiSourceFiles(const fs::path& source_path) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (!fs::exists(source_path, ec)) {
    return files;
  }
  if (fs::is_regular_file(source_path, ec) && source_path.extension() == ".json") {
    files.push_back(source_path);
    return files;
  }
  if (!fs::is_directory(source_path, ec)) {
    return files;
  }
  for (const auto& entry : fs::recursive_directory_iterator(source_path, ec)) {
    if (ec || !entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() == ".json") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string BuildGeminiImportRef(const fs::path& file_path, const std::string& session_id) {
  return NormalizePathString(file_path) + "::session::" + session_id;
}

std::string BuildLegacyChatImportRef(const fs::path& legacy_root, const std::string& chat_id) {
  return NormalizePathString(legacy_root) + "::chat::" + chat_id;
}

std::string BuildLegacyFolderImportRef(const fs::path& legacy_root, const std::string& folder_id) {
  return NormalizePathString(legacy_root) + "::folder::" + folder_id;
}

bool ImportTemplateIfNeeded(const fs::path& source_prompt_root,
                            const fs::path& destination_prompt_root,
                            const std::string& source_template_id,
                            std::unordered_map<std::string, std::string>& template_id_map,
                            ImportSummary& summary,
                            std::string* template_id_in_out) {
  if (template_id_in_out == nullptr) {
    return false;
  }
  const std::string template_id = Trim(*template_id_in_out);
  if (template_id.empty()) {
    return true;
  }
  if (GeminiTemplateCatalog::HasTemplate(destination_prompt_root, template_id)) {
    return true;
  }
  const auto mapped = template_id_map.find(template_id);
  if (mapped != template_id_map.end()) {
    *template_id_in_out = mapped->second;
    return true;
  }
  if (source_prompt_root.empty()) {
    return false;
  }

  fs::path source_template_path;
  std::string resolve_error;
  if (!GeminiTemplateCatalog::ResolveTemplatePath(source_prompt_root, template_id, source_template_path, &resolve_error)) {
    return false;
  }

  std::string imported_id;
  std::string import_error;
  if (!GeminiTemplateCatalog::ImportMarkdownTemplate(destination_prompt_root, source_template_path, &imported_id, &import_error)) {
    return false;
  }
  template_id_map[template_id] = imported_id;
  *template_id_in_out = imported_id;
  ++summary.templates_imported;
  return true;
}

void MaybeImportReferencedProvider(const std::string& original_provider_id,
                                   const std::vector<ProviderProfile>& legacy_profiles,
                                   std::vector<ProviderProfile>& destination_profiles,
                                   ImportSummary& summary,
                                   std::string* provider_id_in_out) {
  if (provider_id_in_out == nullptr) {
    return;
  }
  const std::string mapped_id = MapLegacyProviderId(original_provider_id);
  *provider_id_in_out = mapped_id;
  if (mapped_id.empty() || ProviderProfileStore::FindById(destination_profiles, mapped_id) != nullptr) {
    return;
  }
  const ProviderProfile* legacy_profile = ProviderProfileStore::FindById(legacy_profiles, mapped_id);
  if (legacy_profile == nullptr) {
    legacy_profile = ProviderProfileStore::FindById(legacy_profiles, original_provider_id);
  }
  if (legacy_profile == nullptr) {
    return;
  }
  destination_profiles.push_back(*legacy_profile);
  ++summary.providers_imported;
}

void FinalizeImportedChat(ChatSession& chat,
                          const std::string& provider_id,
                          const std::string& import_kind,
                          const std::string& import_ref) {
  chat.provider_id = provider_id.empty() ? "gemini-structured" : provider_id;
  chat.uses_native_session = false;
  chat.native_session_id.clear();
  chat.import_source_kind = import_kind;
  chat.import_source_ref = import_ref;
  chat.gemini_md_bootstrapped = false;
  if (chat.branch_root_chat_id.empty()) {
    chat.branch_root_chat_id = chat.id;
  }
  if (chat.created_at.empty()) {
    chat.created_at = TimestampNow();
  }
  if (chat.updated_at.empty()) {
    chat.updated_at = chat.created_at;
  }
}

}  // namespace

std::optional<ChatSession> ParseGeminiSessionFileForImport(const fs::path& file_path, const ProviderProfile& provider) {
  const std::string file_text = ReadFileText(file_path);
  if (file_text.empty()) {
    return std::nullopt;
  }
  const std::optional<JsonValue> root_opt = ParseJson(file_text);
  if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object) {
    return std::nullopt;
  }
  const JsonValue& root = root_opt.value();
  const std::string session_id = JsonStringOrEmpty(root.Find("sessionId"));
  if (session_id.empty()) {
    return std::nullopt;
  }

  ChatSession chat;
  chat.id = session_id;
  chat.provider_id = provider.id;
  chat.native_session_id = session_id;
  chat.uses_native_session = true;
  chat.branch_root_chat_id = session_id;
  chat.branch_from_message_index = -1;
  chat.created_at = JsonStringOrEmpty(root.Find("startTime"));
  chat.updated_at = JsonStringOrEmpty(root.Find("lastUpdated"));
  if (chat.created_at.empty()) {
    chat.created_at = TimestampNow();
  }
  if (chat.updated_at.empty()) {
    chat.updated_at = chat.created_at;
  }

  const JsonValue* messages = root.Find("messages");
  if (messages != nullptr && messages->type == JsonValue::Type::Array) {
    for (const JsonValue& raw_message : messages->array_value) {
      if (raw_message.type != JsonValue::Type::Object) {
        continue;
      }
      const std::string type = JsonStringOrEmpty(raw_message.Find("type"));
      const std::string timestamp = JsonStringOrEmpty(raw_message.Find("timestamp"));
      const std::string content = Trim(ExtractGeminiContentText(raw_message.Find("content")));
      if (content.empty()) {
        continue;
      }

      Message message;
      message.role = ProviderRuntime::RoleFromNativeType(provider, type);
      message.content = content;
      message.created_at = timestamp.empty() ? chat.updated_at : timestamp;
      chat.messages.push_back(std::move(message));
    }
  }

  chat.title = "Session " + chat.created_at;
  for (const Message& message : chat.messages) {
    if (message.role != MessageRole::User) {
      continue;
    }
    std::string title = Trim(message.content);
    if (title.size() > 48) {
      title = title.substr(0, 45) + "...";
    }
    if (!title.empty()) {
      chat.title = title;
    }
    break;
  }
  return chat;
}

ImportSummary ImportGeminiChatsIntoLocalData(const fs::path& source_path,
                                             const fs::path& destination_data_root,
                                             const std::string& destination_provider_id) {
  ImportSummary summary;
  const std::vector<fs::path> source_files = CollectGeminiSourceFiles(source_path);
  if (source_files.empty()) {
    summary.message = "No Gemini session JSON files were found.";
    return summary;
  }

  ProviderProfile provider = ProviderProfileStore::DefaultGeminiProfile();
  provider.id = destination_provider_id.empty() ? provider.id : destination_provider_id;
  std::vector<ChatSession> destination_chats = ChatRepository::LoadLocalChats(destination_data_root);

  for (const fs::path& file_path : source_files) {
    const std::optional<ChatSession> parsed_opt = ParseGeminiSessionFileForImport(file_path, provider);
    if (!parsed_opt.has_value()) {
      ++summary.chats_skipped;
      continue;
    }

    ChatSession imported = parsed_opt.value();
    const std::string import_ref = BuildGeminiImportRef(file_path, imported.id);
    ChatSession* existing = FindImportedChat(destination_chats, "gemini-json", import_ref);
    if (existing != nullptr) {
      imported.id = existing->id;
      if (!existing->folder_id.empty()) {
        imported.folder_id = existing->folder_id;
      }
      ++summary.chats_updated;
    } else {
      imported.id = NewImportedChatId();
      imported.folder_id = constants::kDefaultFolderId;
      ++summary.chats_imported;
    }

    FinalizeImportedChat(imported, provider.id, "gemini-json", import_ref);
    if (!ChatRepository::SaveChat(destination_data_root, imported)) {
      summary.message = "Failed to save imported Gemini chat data.";
      return summary;
    }
    if (existing != nullptr) {
      *existing = imported;
    } else {
      destination_chats.push_back(std::move(imported));
    }
  }

  summary.ok = (summary.chats_imported + summary.chats_updated) > 0;
  if (summary.ok) {
    std::ostringstream out;
    out << "Imported Gemini chats: " << summary.chats_imported;
    if (summary.chats_updated > 0) {
      out << " | updated: " << summary.chats_updated;
    }
    if (summary.chats_skipped > 0) {
      out << " | skipped: " << summary.chats_skipped;
    }
    summary.message = out.str();
  }
  return summary;
}

ImportSummary ImportLegacyUamDataIntoLocalData(const fs::path& legacy_data_root,
                                               const fs::path& destination_data_root,
                                               const fs::path& destination_prompt_root) {
  ImportSummary summary;
  std::error_code ec;
  if (!fs::exists(legacy_data_root, ec) || !fs::is_directory(legacy_data_root, ec)) {
    summary.message = "Legacy UAM data directory was not found.";
    return summary;
  }

  std::vector<ChatSession> legacy_chats = ChatRepository::LoadLocalChats(legacy_data_root);
  std::vector<ChatFolder> legacy_folders = ChatFolderStore::Load(legacy_data_root);
  AppSettings legacy_settings;
  CenterViewMode legacy_view_mode = CenterViewMode::Structured;
  SettingsStore::Load(AppPaths::SettingsFilePath(legacy_data_root), legacy_settings, legacy_view_mode);
  std::vector<ProviderProfile> legacy_profiles = ProviderProfileStore::Load(legacy_data_root);

  std::vector<ChatSession> destination_chats = ChatRepository::LoadLocalChats(destination_data_root);
  std::vector<ChatFolder> destination_folders = ChatFolderStore::Load(destination_data_root);
  std::vector<ProviderProfile> destination_profiles = ProviderProfileStore::Load(destination_data_root);
  ProviderProfileStore::EnsureDefaultProfile(destination_profiles);
  EnsureDefaultFolder(destination_folders);

  const fs::path legacy_prompt_root = Trim(legacy_settings.prompt_profile_root_path).empty()
                                          ? fs::path{}
                                          : fs::path(legacy_settings.prompt_profile_root_path);
  std::unordered_map<std::string, std::string> folder_id_map;
  folder_id_map[constants::kDefaultFolderId] = constants::kDefaultFolderId;

  for (const ChatFolder& legacy_folder : legacy_folders) {
    if (legacy_folder.id.empty() || legacy_folder.id == constants::kDefaultFolderId) {
      continue;
    }
    const std::string import_ref = BuildLegacyFolderImportRef(legacy_data_root, legacy_folder.id);
    ChatFolder* existing = FindImportedFolder(destination_folders, "uam-v1-folder", import_ref);
    if (existing != nullptr) {
      existing->title = legacy_folder.title;
      existing->directory = legacy_folder.directory;
      existing->collapsed = legacy_folder.collapsed;
      folder_id_map[legacy_folder.id] = existing->id;
      ++summary.folders_updated;
      continue;
    }

    ChatFolder imported = legacy_folder;
    imported.id = NewImportedFolderId();
    imported.import_source_kind = "uam-v1-folder";
    imported.import_source_ref = import_ref;
    folder_id_map[legacy_folder.id] = imported.id;
    destination_folders.push_back(std::move(imported));
    ++summary.folders_imported;
  }

  std::unordered_map<std::string, std::string> chat_id_map;
  for (const ChatSession& legacy_chat : legacy_chats) {
    const std::string import_ref = BuildLegacyChatImportRef(legacy_data_root, legacy_chat.id);
    ChatSession* existing = FindImportedChat(destination_chats, "uam-v1-chat", import_ref);
    chat_id_map[legacy_chat.id] = (existing != nullptr) ? existing->id : NewImportedChatId();
  }

  std::unordered_map<std::string, std::string> imported_template_ids;
  for (const ChatSession& legacy_chat : legacy_chats) {
    ChatSession imported = legacy_chat;
    const std::string import_ref = BuildLegacyChatImportRef(legacy_data_root, legacy_chat.id);
    ChatSession* existing = FindImportedChat(destination_chats, "uam-v1-chat", import_ref);
    imported.id = chat_id_map[legacy_chat.id];
    imported.folder_id = constants::kDefaultFolderId;
    const auto folder_it = folder_id_map.find(legacy_chat.folder_id);
    if (folder_it != folder_id_map.end()) {
      imported.folder_id = folder_it->second;
    }
    if (!legacy_chat.parent_chat_id.empty()) {
      const auto parent_it = chat_id_map.find(legacy_chat.parent_chat_id);
      imported.parent_chat_id = (parent_it != chat_id_map.end()) ? parent_it->second : "";
    }
    if (!legacy_chat.branch_root_chat_id.empty()) {
      const auto root_it = chat_id_map.find(legacy_chat.branch_root_chat_id);
      imported.branch_root_chat_id = (root_it != chat_id_map.end()) ? root_it->second : imported.id;
    } else {
      imported.branch_root_chat_id = imported.id;
    }

    MaybeImportReferencedProvider(legacy_chat.provider_id, legacy_profiles, destination_profiles, summary, &imported.provider_id);
    if (!ImportTemplateIfNeeded(legacy_prompt_root,
                                destination_prompt_root,
                                legacy_chat.template_override_id,
                                imported_template_ids,
                                summary,
                                &imported.template_override_id)) {
      imported.template_override_id.clear();
    }

    FinalizeImportedChat(imported, imported.provider_id, "uam-v1-chat", import_ref);
    if (existing != nullptr) {
      ++summary.chats_updated;
      *existing = imported;
    } else {
      ++summary.chats_imported;
      destination_chats.push_back(imported);
    }
    if (!ChatRepository::SaveChat(destination_data_root, imported)) {
      summary.message = "Failed to save imported UAM V1 chat data.";
      return summary;
    }
  }

  if (!ChatFolderStore::Save(destination_data_root, destination_folders)) {
    summary.message = "Failed to save imported folders.";
    return summary;
  }
  if (!ProviderProfileStore::Save(destination_data_root, destination_profiles)) {
    summary.message = "Failed to save imported provider profiles.";
    return summary;
  }

  summary.ok = true;
  std::ostringstream out;
  out << "Imported UAM V1 chats: " << summary.chats_imported;
  if (summary.chats_updated > 0) {
    out << " | updated: " << summary.chats_updated;
  }
  if (summary.folders_imported > 0 || summary.folders_updated > 0) {
    out << " | folders +" << summary.folders_imported << " / updated " << summary.folders_updated;
  }
  if (summary.providers_imported > 0) {
    out << " | providers: " << summary.providers_imported;
  }
  if (summary.templates_imported > 0) {
    out << " | templates: " << summary.templates_imported;
  }
  summary.message = out.str();
  return summary;
}

}  // namespace uam
