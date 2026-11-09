#pragma once

#include "app/vcs_commit_service.h"
#include "common/vcs/vcs_provider_declaration.h"

#include <filesystem>
#include <span>
#include <string>

namespace uam
{
	class RemoteVcsContext;

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
	};

	/// <summary>Registered providers in detection order; the first is the default selection.</summary>
	std::span<const VcsProvider* const> VcsProviders();
	const VcsProvider& VcsProviderFor(VcsType type);
} // namespace uam
