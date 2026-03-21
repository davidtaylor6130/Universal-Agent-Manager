#pragma once

/// <summary>
/// Cross-platform shell/file-manager helpers used by UI actions.
/// </summary>
static std::string ShellQuotePath(const std::string& path) {
  std::string escaped = "'";
  for (const char ch : path) {
    if (ch == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('\'');
  return escaped;
}

/// <summary>
/// Executes a shell command and returns success status.
/// </summary>
static bool RunShellCommand(const std::string& command) {
  return std::system(command.c_str()) == 0;
}

/// <summary>
/// Opens a folder path in the native file manager.
/// </summary>
static bool OpenFolderInFileManager(const fs::path& folder_path, std::string* error_out = nullptr) {
  if (folder_path.empty()) {
    if (error_out != nullptr) {
      *error_out = "Folder path is empty.";
    }
    return false;
  }
  std::error_code ec;
  fs::create_directories(folder_path, ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create folder: " + ec.message();
    }
    return false;
  }

#if defined(_WIN32)
  const HINSTANCE result = ShellExecuteW(nullptr, L"open", folder_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    if (error_out != nullptr) {
      *error_out = "Failed to open folder in file manager.";
    }
    return false;
  }
  return true;
#elif defined(__APPLE__)
  const std::string command = "open " + ShellQuotePath(folder_path.string());
#else
  const std::string command = "xdg-open " + ShellQuotePath(folder_path.string()) + " >/dev/null 2>&1";
#endif
#if !defined(_WIN32)
  if (!RunShellCommand(command)) {
    if (error_out != nullptr) {
      *error_out = "Failed to open folder in file manager.";
    }
    return false;
  }
#endif
  return true;
}

/// <summary>
/// Reveals a file path in the native file manager.
/// </summary>
static bool RevealPathInFileManager(const fs::path& file_path, std::string* error_out = nullptr) {
  if (file_path.empty()) {
    if (error_out != nullptr) {
      *error_out = "File path is empty.";
    }
    return false;
  }
  if (!fs::exists(file_path)) {
    return OpenFolderInFileManager(file_path.parent_path(), error_out);
  }
#if defined(_WIN32)
  const std::wstring params = L"/select,\"" + file_path.wstring() + L"\"";
  const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    if (error_out != nullptr) {
      *error_out = "Failed to reveal file in file manager.";
    }
    return false;
  }
  return true;
#elif defined(__APPLE__)
  const std::string command = "open -R " + ShellQuotePath(file_path.string());
#else
  return OpenFolderInFileManager(file_path.parent_path(), error_out);
#endif
#if !defined(_WIN32)
  if (!RunShellCommand(command)) {
    if (error_out != nullptr) {
      *error_out = "Failed to reveal file in file manager.";
    }
    return false;
  }
#endif
  return true;
}
