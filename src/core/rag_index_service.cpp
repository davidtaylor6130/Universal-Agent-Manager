#include "rag_index_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

namespace {
namespace fs = std::filesystem;

struct IgnorePattern {
  std::regex regex;
  bool directory_only = false;
};

std::string NormalizeWorkspaceKey(const fs::path& workspace_root) {
  std::error_code ec;
  const fs::path absolute = fs::absolute(workspace_root, ec);
  const fs::path normalized = ec ? workspace_root.lexically_normal() : absolute.lexically_normal();
  return normalized.generic_string();
}

std::string Trim(const std::string& value) {
  const std::size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string GlobToRegex(const std::string& pattern) {
  std::string out;
  out.reserve(pattern.size() * 2);
  for (const char ch : pattern) {
    switch (ch) {
      case '*':
        out += ".*";
        break;
      case '?':
        out += '.';
        break;
      case '.':
      case '+':
      case '(':
      case ')':
      case '|':
      case '^':
      case '$':
      case '{':
      case '}':
      case '[':
      case ']':
      case '\\':
        out.push_back('\\');
        out.push_back(ch);
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::vector<IgnorePattern> LoadIgnorePatterns(const fs::path& workspace_root) {
  std::vector<IgnorePattern> patterns;
  const fs::path ignore_file = workspace_root / ".gitignore";
  if (!fs::exists(ignore_file)) {
    return patterns;
  }

  std::ifstream in(ignore_file, std::ios::binary);
  if (!in.good()) {
    return patterns;
  }

  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line[0] == '!') {
      continue;
    }

    bool directory_only = false;
    if (!line.empty() && line.back() == '/') {
      directory_only = true;
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }

    const bool anchored = StartsWith(line, "/");
    if (anchored) {
      line = line.substr(1);
    }
    const std::string body = GlobToRegex(line);
    const std::string regex_text = anchored ? ("^" + body + (directory_only ? "(/.*)?$" : "$"))
                                            : ("(^|.*/)" + body + (directory_only ? "(/.*)?$" : "$"));

    try {
      IgnorePattern pattern;
      pattern.regex = std::regex(regex_text);
      pattern.directory_only = directory_only;
      patterns.push_back(std::move(pattern));
    } catch (...) {
      // Skip malformed ignore patterns.
    }
  }
  return patterns;
}

bool PathIsIgnored(const std::string& relative_path, const std::vector<IgnorePattern>& patterns) {
  if (relative_path.empty()) {
    return true;
  }
  if (StartsWith(relative_path, ".git/") || StartsWith(relative_path, ".svn/")) {
    return true;
  }
  for (const IgnorePattern& pattern : patterns) {
    if (std::regex_match(relative_path, pattern.regex)) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> Tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string current;
  current.reserve(32);
  for (const unsigned char ch : text) {
    if (std::isalnum(ch) != 0 || ch == '_') {
      current.push_back(static_cast<char>(std::tolower(ch)));
      continue;
    }
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

std::uint64_t Fnv1a64(const std::string& content) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char ch : content) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::uint64_t LastWriteTimeTicks(const fs::file_time_type& time) {
  const auto ticks = time.time_since_epoch().count();
  return static_cast<std::uint64_t>(ticks);
}

bool IsLikelyBinary(const std::string& content) {
  const std::size_t probe_size = std::min<std::size_t>(content.size(), 4096);
  for (std::size_t i = 0; i < probe_size; ++i) {
    if (content[i] == '\0') {
      return true;
    }
  }
  return false;
}

std::string TruncateSnippet(const std::string& text, const std::size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  if (max_chars <= 3) {
    return text.substr(0, max_chars);
  }
  return text.substr(0, max_chars - 3) + "...";
}

}  // namespace

RagIndexService::RagIndexService() : config_(Config{}) {}

RagIndexService::RagIndexService(const Config& config) : config_(config) {}

void RagIndexService::SetConfig(const Config& config) {
  config_ = config;
}

const RagIndexService::Config& RagIndexService::GetConfig() const {
  return config_;
}

RagRefreshResult RagIndexService::RefreshIndexIncremental(const std::filesystem::path& workspace_root) {
  return RefreshImpl(workspace_root, false);
}

RagRefreshResult RagIndexService::RebuildIndex(const std::filesystem::path& workspace_root) {
  return RefreshImpl(workspace_root, true);
}

RagRefreshResult RagIndexService::RefreshImpl(const std::filesystem::path& workspace_root, const bool force_rebuild) {
  RagRefreshResult result;
  if (!config_.enabled) {
    return result;
  }

  std::error_code ec;
  if (workspace_root.empty() || !fs::exists(workspace_root, ec) || !fs::is_directory(workspace_root, ec)) {
    result.ok = false;
    result.error = "Workspace root is missing or not a directory.";
    return result;
  }

  const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);
  WorkspaceIndex& workspace = indexes_by_workspace_[workspace_key];
  if (force_rebuild) {
    workspace = WorkspaceIndex{};
  }

  const std::vector<IgnorePattern> ignore_patterns = LoadIgnorePatterns(workspace_root);
  std::unordered_set<std::string> seen_paths;

  const auto chunk_text = [&](const std::string& relative_path, const std::string& content) {
    std::vector<Chunk> chunks;
    std::istringstream lines(content);
    std::string line;
    int line_number = 1;

    Chunk current;
    current.relative_path = relative_path;
    current.start_line = 1;
    int chunk_line_count = 0;
    std::size_t chunk_chars = 0;

    auto flush_chunk = [&]() {
      const std::vector<std::string> tokens = Tokenize(current.text);
      for (const std::string& token : tokens) {
        ++current.term_frequency[token];
      }
      current.token_count = static_cast<int>(tokens.size());
      if (!Trim(current.text).empty() && current.token_count > 0) {
        chunks.push_back(current);
      }
      current = Chunk{};
      current.relative_path = relative_path;
      current.start_line = line_number;
      chunk_line_count = 0;
      chunk_chars = 0;
    };

    while (std::getline(lines, line)) {
      if (chunk_line_count == 0) {
        current.start_line = line_number;
      }
      if (!current.text.empty()) {
        current.text.push_back('\n');
      }
      current.text += line;
      current.end_line = line_number;
      ++line_number;
      ++chunk_line_count;
      chunk_chars += line.size() + 1;

      if (chunk_line_count >= config_.chunk_line_span || chunk_chars >= config_.chunk_char_budget) {
        flush_chunk();
      }
    }

    if (!current.text.empty()) {
      flush_chunk();
    }
    return chunks;
  };

  fs::recursive_directory_iterator it(workspace_root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (!ec && it != end) {
    const fs::directory_entry entry = *it;
    ++it;
    if (ec) {
      continue;
    }

    if (!entry.is_regular_file(ec)) {
      continue;
    }

    const fs::path absolute_path = entry.path();
    const fs::path relative_path_fs = fs::relative(absolute_path, workspace_root, ec);
    if (ec) {
      continue;
    }
    const std::string relative_path = relative_path_fs.generic_string();
    if (PathIsIgnored(relative_path, ignore_patterns)) {
      continue;
    }
    seen_paths.insert(relative_path);

    const std::uintmax_t file_size = entry.file_size(ec);
    if (ec) {
      continue;
    }
    if (file_size > config_.max_file_bytes) {
      workspace.files_by_relative_path.erase(relative_path);
      continue;
    }

    const fs::file_time_type mtime = entry.last_write_time(ec);
    if (ec) {
      continue;
    }
    const std::uint64_t mtime_ticks = LastWriteTimeTicks(mtime);

    const auto previous = workspace.files_by_relative_path.find(relative_path);
    if (!force_rebuild && previous != workspace.files_by_relative_path.end() &&
        previous->second.file_size == file_size && previous->second.mtime_ticks == mtime_ticks) {
      continue;
    }

    std::ifstream in(absolute_path, std::ios::binary);
    if (!in.good()) {
      workspace.files_by_relative_path.erase(relative_path);
      continue;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string content = buffer.str();

    if (IsLikelyBinary(content)) {
      workspace.files_by_relative_path.erase(relative_path);
      continue;
    }

    int line_count = 0;
    for (const char ch : content) {
      if (ch == '\n') {
        ++line_count;
      }
    }
    if (line_count > config_.max_line_count) {
      workspace.files_by_relative_path.erase(relative_path);
      continue;
    }

    const std::uint64_t content_hash = Fnv1a64(content);
    if (!force_rebuild && previous != workspace.files_by_relative_path.end() &&
        previous->second.content_hash == content_hash) {
      previous->second.file_size = file_size;
      previous->second.mtime_ticks = mtime_ticks;
      continue;
    }

    FileIndexEntry updated;
    updated.file_size = file_size;
    updated.mtime_ticks = mtime_ticks;
    updated.content_hash = content_hash;
    updated.chunks = chunk_text(relative_path, content);
    workspace.files_by_relative_path[relative_path] = std::move(updated);
    ++result.updated_files;
  }

  std::vector<std::string> stale_paths;
  stale_paths.reserve(workspace.files_by_relative_path.size());
  for (const auto& pair : workspace.files_by_relative_path) {
    if (seen_paths.find(pair.first) == seen_paths.end()) {
      stale_paths.push_back(pair.first);
    }
  }
  for (const std::string& stale : stale_paths) {
    workspace.files_by_relative_path.erase(stale);
    ++result.removed_files;
  }

  workspace.all_chunks.clear();
  workspace.chunk_document_frequency.clear();
  for (const auto& pair : workspace.files_by_relative_path) {
    const FileIndexEntry& entry = pair.second;
    ++result.indexed_files;
    for (const Chunk& chunk : entry.chunks) {
      workspace.all_chunks.push_back(chunk);
      std::set<std::string> unique_terms;
      for (const auto& tf_pair : chunk.term_frequency) {
        unique_terms.insert(tf_pair.first);
      }
      for (const std::string& term : unique_terms) {
        ++workspace.chunk_document_frequency[term];
      }
    }
  }

  return result;
}

std::vector<RagSnippet> RagIndexService::RetrieveTopK(const std::filesystem::path& workspace_root,
                                                      const std::string& query) const {
  std::vector<RagSnippet> snippets;
  if (!config_.enabled) {
    return snippets;
  }

  const auto query_tokens = Tokenize(query);
  if (query_tokens.empty()) {
    return snippets;
  }

  const auto workspace_it = indexes_by_workspace_.find(NormalizeWorkspaceKey(workspace_root));
  if (workspace_it == indexes_by_workspace_.end()) {
    return snippets;
  }
  const WorkspaceIndex& workspace = workspace_it->second;
  if (workspace.all_chunks.empty()) {
    return snippets;
  }

  const double total_chunks = static_cast<double>(workspace.all_chunks.size());
  struct ScoredChunk {
    const Chunk* chunk = nullptr;
    double score = 0.0;
  };
  std::vector<ScoredChunk> scored;
  scored.reserve(workspace.all_chunks.size());

  for (const Chunk& chunk : workspace.all_chunks) {
    if (chunk.token_count <= 0) {
      continue;
    }

    double score = 0.0;
    for (const std::string& token : query_tokens) {
      const auto tf_it = chunk.term_frequency.find(token);
      if (tf_it == chunk.term_frequency.end()) {
        continue;
      }
      const auto df_it = workspace.chunk_document_frequency.find(token);
      const double df = (df_it == workspace.chunk_document_frequency.end()) ? 0.0 : static_cast<double>(df_it->second);
      const double idf = std::log((total_chunks + 1.0) / (df + 1.0)) + 1.0;
      const double tf = static_cast<double>(tf_it->second) / static_cast<double>(chunk.token_count);
      score += (tf * idf);
    }

    if (score > 0.0) {
      scored.push_back(ScoredChunk{&chunk, score});
    }
  }

  std::sort(scored.begin(), scored.end(), [](const ScoredChunk& lhs, const ScoredChunk& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score > rhs.score;
    }
    if (lhs.chunk->relative_path != rhs.chunk->relative_path) {
      return lhs.chunk->relative_path < rhs.chunk->relative_path;
    }
    return lhs.chunk->start_line < rhs.chunk->start_line;
  });

  std::unordered_set<std::string> seen_files;
  for (const ScoredChunk& item : scored) {
    if (seen_files.find(item.chunk->relative_path) != seen_files.end()) {
      continue;
    }
    seen_files.insert(item.chunk->relative_path);
    RagSnippet snippet;
    snippet.relative_path = item.chunk->relative_path;
    snippet.start_line = item.chunk->start_line;
    snippet.end_line = item.chunk->end_line;
    snippet.text = TruncateSnippet(item.chunk->text, config_.max_snippet_chars);
    snippet.score = item.score;
    snippets.push_back(std::move(snippet));
    if (static_cast<int>(snippets.size()) >= config_.top_k) {
      break;
    }
  }

  return snippets;
}
