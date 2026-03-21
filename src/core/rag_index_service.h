#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct RagSnippet {
  std::string relative_path;
  int start_line = 1;
  int end_line = 1;
  std::string text;
  double score = 0.0;
};

struct RagRefreshResult {
  bool ok = true;
  int indexed_files = 0;
  int updated_files = 0;
  int removed_files = 0;
  std::string error;
};

class RagIndexService {
 public:
  struct Config {
    bool enabled = true;
    int top_k = 6;
    std::size_t max_snippet_chars = 600;
    std::size_t max_file_bytes = 1024 * 1024;
    int max_line_count = 20000;
    int chunk_line_span = 60;
    std::size_t chunk_char_budget = 2400;
  };

  RagIndexService();
  explicit RagIndexService(const Config& config);

  void SetConfig(const Config& config);
  const Config& GetConfig() const;

  RagRefreshResult RefreshIndexIncremental(const std::filesystem::path& workspace_root);
  RagRefreshResult RebuildIndex(const std::filesystem::path& workspace_root);

  std::vector<RagSnippet> RetrieveTopK(const std::filesystem::path& workspace_root,
                                       const std::string& query) const;

 private:
  struct Chunk {
    std::string relative_path;
    int start_line = 1;
    int end_line = 1;
    std::string text;
    std::unordered_map<std::string, int> term_frequency;
    int token_count = 0;
  };

  struct FileIndexEntry {
    std::uint64_t mtime_ticks = 0;
    std::uintmax_t file_size = 0;
    std::uint64_t content_hash = 0;
    std::vector<Chunk> chunks;
  };

  struct WorkspaceIndex {
    std::unordered_map<std::string, FileIndexEntry> files_by_relative_path;
    std::vector<Chunk> all_chunks;
    std::unordered_map<std::string, int> chunk_document_frequency;
  };

  RagRefreshResult RefreshImpl(const std::filesystem::path& workspace_root, bool force_rebuild);

  Config config_;
  std::unordered_map<std::string, WorkspaceIndex> indexes_by_workspace_;
};
