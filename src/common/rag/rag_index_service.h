#pragma once

#include <cstddef>
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
/// Current state for asynchronous RAG scanning/vectorisation.
/// </summary>
enum class RagScanLifecycleState {
  Stopped,
  Running,
  Finished,
};

/// <summary>
/// Snapshot of scan lifecycle + progress.
/// </summary>
struct RagScanState {
  RagScanLifecycleState lifecycle = RagScanLifecycleState::Stopped;
  std::size_t vector_database_size = 0;
  std::size_t files_processed = 0;
  std::size_t total_files = 0;
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
    std::size_t vector_max_tokens = 0;
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
  /// <summary>Overrides the next explicit scan source used for a workspace.</summary>
  void SetScanSourceOverride(const std::filesystem::path& workspace_root, const std::filesystem::path& scan_source_root);
  /// <summary>Clears any scan-source override for a workspace.</summary>
  void ClearScanSourceOverride(const std::filesystem::path& workspace_root);

  /// <summary>Incrementally refreshes a workspace index.</summary>
  RagRefreshResult RefreshIndexIncremental(const std::filesystem::path& workspace_root);
  /// <summary>Forces a full workspace index rebuild.</summary>
  RagRefreshResult RebuildIndex(const std::filesystem::path& workspace_root);
  /// <summary>Runs a rescan using the engine's previous source semantics.</summary>
  RagRefreshResult RescanPreviousSource(const std::filesystem::path& workspace_root);

  /// <summary>Retrieves top matching snippets for a query.</summary>
  std::vector<RagSnippet> RetrieveTopK(const std::filesystem::path& workspace_root,
                                       const std::string& query);
  /// <summary>Retrieves snippets with explicit max/min limits.</summary>
  std::vector<RagSnippet> Retrieve(const std::filesystem::path& workspace_root,
                                   const std::string& query,
                                   std::size_t max_results,
                                   std::size_t min_results,
                                   std::string* error_out = nullptr);
  /// <summary>Returns current scan lifecycle/progress state.</summary>
  RagScanState FetchState();

 private:
  RagRefreshResult ScanWorkspace(const std::filesystem::path& workspace_root, bool reuse_previous_source);
  bool ConfigureWorkspaceDatabase(const std::filesystem::path& workspace_root, std::string* error_out = nullptr);
  std::string WorkspaceDatabaseName(const std::string& workspace_key);
  static RagSnippet ParseSnippet(const std::string& snippet_text, std::size_t max_snippet_chars);
  static std::string NormalizeWorkspaceKey(const std::filesystem::path& workspace_root);

  Config config_;
  std::filesystem::path model_folder_;
  std::unordered_map<std::string, std::string> database_name_by_workspace_;
  std::unordered_map<std::string, std::string> scan_source_override_by_workspace_;
  std::string active_workspace_key_;
  std::string last_scan_error_;
  std::unique_ptr<OllamaEngineClient> model_engine_client_;
};
