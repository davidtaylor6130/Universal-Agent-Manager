#pragma once

#include <filesystem>
#include <string>

/// <summary>
/// Supported workspace repository types.
/// </summary>
enum class VcsRepoType {
  None,
  Svn
};

/// <summary>
/// Normalized repository snapshot metadata.
/// </summary>
struct VcsSnapshot {
  VcsRepoType repo_type = VcsRepoType::None;
  std::string working_copy_root;
  std::string repo_url;
  std::string revision;
  std::string branch_path;
};

/// <summary>
/// Command execution result for VCS operations.
/// </summary>
struct VcsCommandResult {
  bool ok = false;
  bool timed_out = false;
  bool truncated = false;
  int exit_code = -1;
  std::string output;
  std::string error;
};

/// <summary>
/// Reads repository metadata and command outputs for a workspace.
/// </summary>
class VcsWorkspaceService {
 public:
  /// <summary>Detects supported repository type for a workspace.</summary>
  static VcsRepoType DetectRepo(const std::filesystem::path& workspace_root);

  /// <summary>Reads repository snapshot metadata into the output structure.</summary>
  static VcsCommandResult ReadSnapshot(const std::filesystem::path& workspace_root,
                                       VcsSnapshot& snapshot_out);

  /// <summary>Reads repository status output.</summary>
  static VcsCommandResult ReadStatus(const std::filesystem::path& workspace_root);
  /// <summary>Reads repository diff output.</summary>
  static VcsCommandResult ReadDiff(const std::filesystem::path& workspace_root);
  /// <summary>Reads repository log output.</summary>
  static VcsCommandResult ReadLog(const std::filesystem::path& workspace_root);
};
