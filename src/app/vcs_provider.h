#pragma once

#include "app/vcs_commit_service.h"
#include "common/vcs/vcs_provider_declaration.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	class RemoteVcsContext;

	struct VcsStatusRequest
	{
		const AppState& app;
		const ChatSession& chat;
		bool include_line_stats;
		std::string_view comparison_ref;
		bool context_only;
	};

	/// <summary>One version-control system behind VcsCommitService. To add a system, declare it in
	/// vcs_provider_declaration.h, implement this interface and register it in VcsProviders().</summary>
	class VcsProvider
	{
	  public:
		virtual ~VcsProvider() = default;

		virtual VcsType Type() const = 0;
		virtual const vcs::VcsProviderDeclaration& Declaration() const = 0;
		virtual bool Enabled(const AppSettings& settings) const = 0;

		/// <summary>`root` arrives as the workspace and is replaced only when a repository root is known.</summary>
		virtual bool DetectLocal(const std::filesystem::path& workspace, bool validate, std::filesystem::path& root) const = 0;
		virtual bool DetectRemote(RemoteVcsContext& context, const std::string& workspace, std::string& root) const = 0;

		/// <summary>Fills branch or revision and changed files for an already detected and selected system.</summary>
		virtual void PopulateLocalStatus(const VcsStatusRequest& request, const std::filesystem::path& workspace, const std::filesystem::path& root, VcsCommitStatus& status) const = 0;
		virtual void PopulateRemoteStatus(RemoteVcsContext& context, const VcsStatusRequest& request, const std::string& root, VcsCommitStatus& status) const = 0;

		/// <summary>Returns the patch for one selected path. The remote form has the caller's already validated changed-file row.</summary>
		virtual std::string LocalDiff(const AppState& app, const ChatSession& chat, const std::filesystem::path& workspace, const std::string& trimmed_path, std::string_view comparison_ref, std::string* error_out) const = 0;
		virtual std::string RemoteDiff(RemoteVcsContext& context, const AppState& app, const ChatSession& chat, const std::string& path, const VcsChangedFile& selected, std::string cwd, const std::string& comparison_ref, std::string* error_out) const = 0;

		/// <summary>Publishes the selected paths. On false, `error` is set and the system's own staging state has been restored or the failure reported in it.</summary>
		virtual bool LocalCommit(const std::filesystem::path& workspace, const std::string& message, const std::vector<std::string>& files, const VcsCommitStatus& status, ProcessExecutionResult& committed, std::string& error) const = 0;
		virtual bool RemoteCommit(RemoteVcsContext& context, std::string cwd, const std::string& message, std::vector<std::string> selected_files, const VcsCommitStatus& status, ProcessExecutionResult& committed, std::string& error) const = 0;
	};

	/// <summary>Registered providers in detection order; the first is the default selection.</summary>
	std::span<const VcsProvider* const> VcsProviders();
	const VcsProvider& VcsProviderFor(VcsType type);
} // namespace uam
