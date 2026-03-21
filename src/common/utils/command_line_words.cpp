#include "command_line_words.h"

#include <cctype>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::vector<std::string> SplitShellWordsFallback(const std::string& value) {
  std::vector<std::string> words;
  std::string current;
  bool escaping = false;
  char quote = '\0';
  for (char ch : value) {
    if (escaping) {
      current.push_back(ch);
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (quote != '\0') {
      if (ch == quote) {
        quote = '\0';
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (escaping) {
    current.push_back('\\');
  }
  if (!current.empty()) {
    words.push_back(current);
  }
  return words;
}

#if defined(_WIN32)
std::wstring WideFromString(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }

  auto convert = [&](const UINT code_page, const DWORD flags) -> std::wstring {
    const int wide_len = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wide_len <= 0) {
      return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    if (MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), wide.data(), wide_len) <= 0) {
      return std::wstring();
    }
    return wide;
  };

  std::wstring wide = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
  if (!wide.empty()) {
    return wide;
  }
  return convert(CP_ACP, 0);
}

std::string StringFromWide(const std::wstring& value) {
  if (value.empty()) {
    return std::string();
  }

  auto convert = [&](const UINT code_page) -> std::string {
    const int narrow_len =
        WideCharToMultiByte(code_page, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (narrow_len <= 0) {
      return std::string();
    }
    std::string narrow(static_cast<std::size_t>(narrow_len), '\0');
    if (WideCharToMultiByte(code_page, 0, value.data(), static_cast<int>(value.size()),
                            narrow.data(), narrow_len, nullptr, nullptr) <= 0) {
      return std::string();
    }
    return narrow;
  };

  std::string narrow = convert(CP_UTF8);
  if (!narrow.empty()) {
    return narrow;
  }
  return convert(CP_ACP);
}
#endif

}  // namespace

std::vector<std::string> SplitCommandLineWords(const std::string& value) {
  if (value.empty()) {
    return {};
  }

#if defined(_WIN32)
  const std::wstring wide = WideFromString(value);
  if (!wide.empty()) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(wide.c_str(), &argc);
    if (argv != nullptr) {
      std::vector<std::string> words;
      words.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
      for (int i = 0; i < argc; ++i) {
        words.push_back(StringFromWide(argv[i] == nullptr ? L"" : std::wstring(argv[i])));
      }
      LocalFree(argv);
      return words;
    }
  }
#endif

  return SplitShellWordsFallback(value);
}
