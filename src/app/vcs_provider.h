#pragma once

#include "app/vcs_commit_service.h"
#include "common/vcs/vcs_provider_declaration.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

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
	};

	/// <summary>Registered providers in detection order; the first is the default selection.</summary>
	std::span<const VcsProvider* const> VcsProviders();
	const VcsProvider& VcsProviderFor(VcsType type);
} // namespace uam
