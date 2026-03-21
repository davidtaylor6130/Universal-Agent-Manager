#pragma once

#include <filesystem>
#include <string>

enum class VcsRepoType {
  None,
  Svn
};

struct VcsSnapshot {
  VcsRepoType repo_type = VcsRepoType::None;
  std::string working_copy_root;
  std::string repo_url;
  std::string revision;
  std::string branch_path;
};

struct VcsCommandResult {
  bool ok = false;
  bool timed_out = false;
  bool truncated = false;
  int exit_code = -1;
  std::string output;
  std::string error;
};

class VcsWorkspaceService {
 public:
  static VcsRepoType DetectRepo(const std::filesystem::path& workspace_root);

  static VcsCommandResult ReadSnapshot(const std::filesystem::path& workspace_root,
                                       VcsSnapshot& snapshot_out);

  static VcsCommandResult ReadStatus(const std::filesystem::path& workspace_root);
  static VcsCommandResult ReadDiff(const std::filesystem::path& workspace_root);
  static VcsCommandResult ReadLog(const std::filesystem::path& workspace_root);
};
