#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class OllamaEngineClient;

/// <summary>
/// One retrieved RAG snippet candidate.
/// </summary>
struct RagSnippet {
  std::string relative_path;
  int start_line = 1;
  int end_line = 1;
  std::string text;
  double score = 0.0;
};

/// <summary>
/// Result metadata for a RAG index refresh/rebuild operation.
/// </summary>
struct RagRefreshResult {
  bool ok = true;
  int indexed_files = 0;
  int updated_files = 0;
  int removed_files = 0;
  std::string error;
};

/// <summary>
/// Builds and queries workspace-local retrieval indexes for prompt augmentation.
/// </summary>
class RagIndexService {
 public:
  /// <summary>Runtime configuration for indexing and retrieval.</summary>
  struct Config {
    bool enabled = true;
    bool vector_enabled = true;
    int top_k = 6;
    std::size_t max_snippet_chars = 600;
    std::size_t max_file_bytes = 1024 * 1024;
    std::size_t vector_dimensions = 256;
    int max_line_count = 20000;
    int chunk_line_span = 60;
    std::size_t chunk_char_budget = 2400;
  };

  /// <summary>Constructs the service with default configuration.</summary>
  RagIndexService();
  /// <summary>Constructs the service with explicit configuration.</summary>
  explicit RagIndexService(const Config& config);
  /// <summary>Destroys the service and any model client state.</summary>
  ~RagIndexService();

  /// <summary>Applies a new runtime configuration.</summary>
  void SetConfig(const Config& config);
  /// <summary>Returns the active runtime configuration.</summary>
  const Config& GetConfig() const;
  /// <summary>Sets the model folder used by the embedding engine.</summary>
  void SetModelFolder(const std::filesystem::path& model_folder);
  /// <summary>Lists available embedding models.</summary>
  std::vector<std::string> ListModels();
  /// <summary>Loads an embedding model by name.</summary>
  bool LoadModel(const std::string& model_name, std::string* error_out = nullptr);

  /// <summary>Incrementally refreshes a workspace index.</summary>
  RagRefreshResult RefreshIndexIncremental(const std::filesystem::path& workspace_root);
  /// <summary>Forces a full workspace index rebuild.</summary>
  RagRefreshResult RebuildIndex(const std::filesystem::path& workspace_root);

  /// <summary>Retrieves top matching snippets for a query.</summary>
  std::vector<RagSnippet> RetrieveTopK(const std::filesystem::path& workspace_root,
                                       const std::string& query);

 private:
  struct Chunk {
    std::string relative_path;
    int start_line = 1;
    int end_line = 1;
    std::string text;
    std::unordered_map<std::string, int> term_frequency;
    int token_count = 0;
    std::vector<float> vector_embedding;
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
  bool EnsureModelLoaded(std::string* error_out = nullptr);
  std::vector<float> BuildQueryEmbedding(const std::string& query);
  static std::vector<float> BuildFallbackEmbedding(const std::string& text, std::size_t dimensions);
  static double CosineSimilarity(const std::vector<float>& lhs, const std::vector<float>& rhs);

  Config config_;
  std::unordered_map<std::string, WorkspaceIndex> indexes_by_workspace_;
  std::filesystem::path model_folder_;
  std::string loaded_model_;
  std::unique_ptr<OllamaEngineClient> model_engine_client_;
};
