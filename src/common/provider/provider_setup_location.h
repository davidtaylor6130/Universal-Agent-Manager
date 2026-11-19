#pragma once

#include "common/config/execution_host_config.h"
#include "common/utils/env_utils.h"
#include "common/provider/provider_native_context.h"
#include "common/provider/provider_setup.h"

#include <chrono>
#include <fstream>
#include <optional>
#include <string>

namespace uam::provider_setup
{
	/// <summary>A private folder under the helper's home. The home path is resolved so a link above it is not treated as an unsafe resource path.</summary>
	inline bool HelperPrivateFolder(const char* parent, const char* name, std::filesystem::path& root, std::string& error)
	{
		const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath();
		std::error_code ec;
		const std::filesystem::path resolved = home ? std::filesystem::canonical(*home, ec) : std::filesystem::path{};
		if (!home || ec || !resolved.is_absolute()) { error = "The helper's home folder is unavailable."; return false; }
		root = resolved / parent / name;
		std::filesystem::create_directories(root, ec);
		if (ec || !NoSymlinks(root)) { error = "The helper's private setup folder is unavailable."; return false; }
		std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
		if (ec) { error = "The helper's private setup folder could not be secured."; return false; }
		return true;
	}

	inline bool HelperOwnedRoot(std::filesystem::path& root, std::string& error) { return HelperPrivateFolder(".uam", "provider-setup", root, error); }

	/// <summary>Handoff context keeps the .UAM/context shape its own ownership checks require.</summary>
	inline bool HelperContextRoot(std::filesystem::path& root, std::string& error) { return HelperPrivateFolder(".UAM", "context", root, error); }

	/// <summary>True only for a folder directly inside the helper's private root.</summary>
	inline bool IsHelperOwnedResource(const std::filesystem::path& directory)
	{
		std::filesystem::path root;
		std::string error;
		return HelperOwnedRoot(root, error) && directory.parent_path() == root;
	}

	/// <summary>True only for a folder directly inside the helper's private context root.</summary>
	inline bool IsHelperContextResource(const std::filesystem::path& directory)
	{
		std::filesystem::path root;
		std::string error;
		return HelperContextRoot(root, error) && directory.parent_path() == root;
	}

	/// <summary>A helper may run on another platform, so its paths cannot be judged by this computer's path rules.</summary>
	inline bool IsAbsoluteOnAnyHost(const std::string& path)
	{
		return uam::execution_hosts::IsAbsoluteRemotePath("linux", path) || uam::execution_hosts::IsAbsoluteRemotePath("windows", path);
	}

	/// <summary>Launch setup may only start from this workspace's managed folder or the helper's private root.</summary>
	inline bool IsAllowedConfigurationDirectory(const std::filesystem::path& directory, const std::filesystem::path& workspace)
	{
		return IsManagedWorkspaceResource(directory, "provider-setup", workspace) || IsHelperOwnedResource(directory);
	}

	/// <summary>Creates the folder and proves this account can write inside it.</summary>
	inline bool CanWriteInside(const std::filesystem::path& folder, const std::filesystem::path& target)
	{
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		if (ec) return false;
		const std::filesystem::path probe = folder / (".write-probe-" + uam::paths::Utf8PathString(target.filename()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		{
			std::ofstream output(probe, std::ios::binary | std::ios::trunc);
			if (!output) return false;
		}
		std::filesystem::remove(probe, ec);
		return true;
	}

	/// <summary>A workspace reached through a link, or one this user cannot write to, cannot hold launch setup.</summary>
	inline bool WorkspaceCanHoldSetup(const std::filesystem::path& target)
	{
		if (!NoSymlinks(target) || !CanWriteInside(target.parent_path(), target)) return false;
		// An existing folder from another account can sit in a writable parent.
		std::error_code ec;
		return !std::filesystem::exists(target, ec) || CanWriteInside(target, target);
	}

	/// <summary>Handoff context needs only a writable workspace: its ownership checks already allow a link above the workspace.</summary>
	inline bool ResolveContextLocation(const std::filesystem::path& requested, std::filesystem::path& directory, std::string& error)
	{
		std::string link_error;
		if (uam::provider_native_context::OwnedPathHasNoLinks(requested, link_error) && CanWriteInside(requested.parent_path(), requested)) { directory = requested; return true; }
		std::filesystem::path root;
		if (!HelperContextRoot(root, error)) return false;
		directory = root / requested.filename();
		return true;
	}

	/// <summary>Chooses where a bundle installs. Setup that references its own files stays in a usable workspace, so agents can read them there.</summary>
	inline bool ResolveConfigurationLocation(const std::filesystem::path& workspace, const std::string& digest, bool prefer_workspace, std::filesystem::path& directory, std::string& error)
	{
		if (digest.size() != 16 || digest.find_first_not_of("0123456789abcdef") != std::string::npos) { error = "The launch setup identity is invalid."; return false; }
		const std::filesystem::path candidate = workspace / ".UAM" / "provider-setup" / uam::paths::PathFromUtf8(digest);
		if (prefer_workspace && WorkspaceCanHoldSetup(candidate)) { directory = candidate; return true; }
		std::filesystem::path root;
		if (!HelperOwnedRoot(root, error)) return false;
		directory = root / uam::paths::PathFromUtf8(digest);
		return true;
	}
}
