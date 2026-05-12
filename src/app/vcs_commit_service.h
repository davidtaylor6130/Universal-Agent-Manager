#ifndef UAM_APP_VCS_COMMIT_SERVICE_H
#define UAM_APP_VCS_COMMIT_SERVICE_H

#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

namespace uam
{
	enum class VcsType
	{
		Git,
		Svn
	};

	struct VcsChangedFile
	{
		std::string path;
		std::string status;
		bool staged = false;
		int additions = 0;
		int deletions = 0;
		bool binary = false;
	};

	struct VcsCommitStatus
	{
		bool available = false;
		std::vector<VcsType> vcs_types;
		VcsType active_vcs_type = VcsType::Git;
		std::string workspace_directory;
		std::string branch_or_revision;
		std::vector<VcsChangedFile> changed_files;
		bool line_stats_ready = true;
		std::string warning;
		std::string error;
	};

	struct VcsCommitResult
	{
		bool ok = false;
		VcsCommitStatus status;
		std::string message;
		std::string error;
	};

	struct VcsCommitMessageSuggestion
	{
		bool ok = false;
		std::string title;
		std::string description;
		std::string error;
	};

	std::string VcsTypeToString(VcsType type);
	VcsType VcsTypeFromString(const std::string& value);

	class VcsCommitService
	{
	  public:
		VcsCommitStatus Status(const AppState& app, const ChatSession& chat, VcsType requested_type = VcsType::Git, bool include_line_stats = true) const;
		std::string Diff(const AppState& app, const ChatSession& chat, const std::string& path, VcsType type, std::string* error_out = nullptr) const;
		VcsCommitResult Commit(AppState& app, const ChatSession& chat, VcsType type, const std::string& message, const std::vector<std::string>& files) const;
		VcsCommitMessageSuggestion GenerateMessage(const AppState& app, const ChatSession& chat, VcsType type, const std::vector<std::string>& files) const;
	};
} // namespace uam

#endif // UAM_APP_VCS_COMMIT_SERVICE_H
