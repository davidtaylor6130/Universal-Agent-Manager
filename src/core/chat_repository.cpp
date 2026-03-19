#include "chat_repository.h"

#include "app_paths.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
namespace fs = std::filesystem;

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

void SortChatsByRecent(std::vector<ChatSession>& chats) {
  std::sort(chats.begin(), chats.end(), [](const ChatSession& a, const ChatSession& b) {
    return a.updated_at > b.updated_at;
  });
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

}  // namespace

bool ChatRepository::SaveChat(const std::filesystem::path& data_root, const ChatSession& chat) {
  const fs::path chat_root = AppPaths::ChatPath(data_root, chat.id);
  const fs::path messages_dir = chat_root / "messages";

  std::error_code ec;
  fs::create_directories(messages_dir, ec);
  if (ec) {
    return false;
  }

  std::ostringstream meta;
  meta << "id=" << chat.id << '\n';
  meta << "folder=" << chat.folder_id << '\n';
  meta << "template_override=" << chat.template_override_id << '\n';
  meta << "title=" << chat.title << '\n';
  meta << "created_at=" << chat.created_at << '\n';
  meta << "updated_at=" << chat.updated_at << '\n';
  for (const std::string& file_path : chat.linked_files) {
    meta << "file=" << file_path << '\n';
  }
  if (!WriteTextFile(chat_root / "meta.txt", meta.str())) {
    return false;
  }

  for (const auto& item : fs::directory_iterator(messages_dir, ec)) {
    if (ec) {
      return false;
    }
    if (item.is_regular_file()) {
      fs::remove(item.path(), ec);
      if (ec) {
        return false;
      }
    }
  }

  for (std::size_t i = 0; i < chat.messages.size(); ++i) {
    std::ostringstream filename;
    filename << std::setfill('0') << std::setw(6) << (i + 1) << "_" << RoleToString(chat.messages[i].role) << ".txt";
    if (!WriteTextFile(messages_dir / filename.str(), chat.messages[i].content)) {
      return false;
    }
  }

  return true;
}

std::vector<ChatSession> ChatRepository::LoadLocalChats(const std::filesystem::path& data_root) {
  std::vector<ChatSession> chats;
  const fs::path chats_root = AppPaths::ChatsRootPath(data_root);
  if (!fs::exists(chats_root)) {
    return chats;
  }

  std::error_code ec;
  for (const auto& folder : fs::directory_iterator(chats_root, ec)) {
    if (ec || !folder.is_directory()) {
      continue;
    }

    ChatSession chat;
    chat.id = folder.path().filename().string();
    chat.title = "Untitled Chat";

    const fs::path meta_file = folder.path() / "meta.txt";
    if (fs::exists(meta_file)) {
      std::istringstream lines(ReadTextFile(meta_file));
      std::string line;
      while (std::getline(lines, line)) {
        const auto equals_at = line.find('=');
        if (equals_at == std::string::npos) {
          continue;
        }
        const std::string key = line.substr(0, equals_at);
        const std::string value = line.substr(equals_at + 1);
        if (key == "id") {
          chat.id = value;
        } else if (key == "folder") {
          chat.folder_id = value;
        } else if (key == "template_override") {
          chat.template_override_id = value;
        } else if (key == "title") {
          chat.title = value;
        } else if (key == "created_at") {
          chat.created_at = value;
        } else if (key == "updated_at") {
          chat.updated_at = value;
        } else if (key == "file" && !value.empty()) {
          chat.linked_files.push_back(value);
        }
      }
    }
    if (chat.created_at.empty()) {
      chat.created_at = chat.updated_at.empty() ? TimestampNow() : chat.updated_at;
    }
    if (chat.updated_at.empty()) {
      chat.updated_at = chat.created_at;
    }

    const fs::path messages_dir = folder.path() / "messages";
    if (fs::exists(messages_dir)) {
      std::vector<fs::path> message_files;
      for (const auto& file : fs::directory_iterator(messages_dir, ec)) {
        if (!ec && file.is_regular_file() && file.path().extension() == ".txt") {
          message_files.push_back(file.path());
        }
      }
      std::sort(message_files.begin(), message_files.end());

      for (const auto& message_file : message_files) {
        const std::string file_name = message_file.filename().string();
        const auto underscore = file_name.find('_');
        const auto dot = file_name.find_last_of('.');
        std::string role = "user";
        if (underscore != std::string::npos && dot != std::string::npos && dot > underscore) {
          role = file_name.substr(underscore + 1, dot - underscore - 1);
        }
        Message message;
        message.role = RoleFromString(role);
        message.content = ReadTextFile(message_file);
        message.created_at = chat.updated_at;
        chat.messages.push_back(std::move(message));
      }
    }

    chats.push_back(std::move(chat));
  }

  SortChatsByRecent(chats);
  return chats;
}
