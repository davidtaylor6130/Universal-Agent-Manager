#pragma once

#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_file_reader.h"
#include "common/platform/native_user_home.h"
#include "common/utils/env_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uam::gemini
{
	// Published CLI homedir() replaces HOME; Storage then appends .gemini.
	inline std::filesystem::path HistoryHomePath()
	{
		auto home = platform::ResolveNativeUserHomePath(env::GetNonEmptyString("HOME").value_or(""), env::GetNonEmptyString("USERPROFILE").value_or(""));
		if (const auto configured = env::GetNonEmptyString("GEMINI_CLI_HOME")) home = paths::PathFromUtf8(*configured);
		if (!home) home = paths::TempDirectoryPathNoThrow();
		return home ? paths::AbsolutePathNoThrow(*home / ".gemini") : std::filesystem::path{};
	}

	inline bool SameHistoryWorkspace(std::string_view first, std::string_view second)
	{
		return execution_hosts::RemotePathsMatch(execution_hosts::IsAbsoluteRemotePath("windows", first) && execution_hosts::IsAbsoluteRemotePath("windows", second)
		    ? "windows" : execution_hosts::LocalHost().platform, first, second);
	}

	enum class HistoryPathState { Missing, Readable, Unreadable };
	struct HistoryTextFile
	{
		HistoryPathState state = HistoryPathState::Unreadable;
		std::string text;
	};
	inline HistoryTextFile ReadHistoryOwnershipFile(const std::filesystem::path& root, const std::filesystem::path& relative, std::size_t limit)
	{
		std::error_code error;
		const auto status = std::filesystem::symlink_status(root / relative, error);
		if (error == std::errc::no_such_file_or_directory || (!error && status.type() == std::filesystem::file_type::not_found)) return {HistoryPathState::Missing, {}};
		if (error) return {};
		auto file = workspace_files::Read(root, paths::Utf8PathString(relative), false, limit);
		return file.ok ? HistoryTextFile{HistoryPathState::Readable, strings::Trim(file.text)} : HistoryTextFile{};
	}

	struct HistoryProject
	{
		std::filesystem::path root;
		std::filesystem::path relative;
		std::string workspace;
	};
	inline std::string HistoryWorkspaceKey(std::string_view workspace)
	{
		return execution_hosts::NormalizeRemotePath(execution_hosts::IsAbsoluteRemotePath("windows", workspace) ? "windows" : execution_hosts::LocalHost().platform, workspace);
	}

	enum class HistoryDiagnosticScope { Profile, Project };
	struct HistoryDiscoveryDiagnostic
	{
		HistoryDiagnosticScope scope = HistoryDiagnosticScope::Profile;
		std::filesystem::path project;
		std::vector<std::string> workspaces;
		std::string message;
	};
	struct HistoryProjectDiscovery
	{
		std::vector<HistoryProject> projects;
		std::vector<HistoryDiscoveryDiagnostic> diagnostics;
		std::unordered_map<std::string, std::size_t> workspace_claims;
		std::unordered_set<std::string> rejected_workspaces;
		bool profile_failed = false;
		bool cancelled = false;
		bool Fatal() const
		{
			return cancelled || profile_failed;
		}
		bool CanReadWorkspace(std::string_view workspace) const
		{
			if (Fatal()) return false;
			const auto key = HistoryWorkspaceKey(workspace);
			if (rejected_workspaces.contains(key)) return false;
			const auto claims = workspace_claims.find(key);
			// A healthy project cannot mask a second claim to the same workspace.
			return claims != workspace_claims.end() && claims->second == 1;
		}
		std::string DiagnosticText() const
		{
			std::string text;
			for (const auto& diagnostic : diagnostics)
			{
				if (!text.empty()) text += "\n";
				text += diagnostic.message;
				if (!diagnostic.project.empty()) text += " [" + paths::Utf8PathString(diagnostic.project) + "]";
			}
			return text;
		}
	};
	inline HistoryProjectDiscovery DiscoverHistoryProjectsWithDiagnostics(std::stop_token stop_token = {}, std::size_t max_projects = 0)
	{
		HistoryProjectDiscovery result;
		const auto fatal = [&](std::string text) { result.profile_failed = true; result.diagnostics.push_back({HistoryDiagnosticScope::Profile, {}, {}, std::move(text)}); result.projects.clear(); };
		const auto cancel = [&] { result.cancelled = true; result.projects.clear(); };
		const auto root = HistoryHomePath();
		if (stop_token.stop_requested()) { cancel(); return result; }
		if (root.empty()) { fatal("Could not resolve the Gemini history profile."); return result; }
		std::unordered_map<std::string, std::vector<std::string>> registered;
		const auto registry = ReadHistoryOwnershipFile(root, "projects.json", 1024 * 1024);
		if (registry.state == HistoryPathState::Unreadable) { fatal("Could not read the Gemini project registry."); return result; }
		if (registry.state == HistoryPathState::Readable)
		{
			const auto data = nlohmann::json::parse(registry.text, nullptr, false);
			if (data.is_object())
				if (const auto projects = data.find("projects"); projects != data.end() && projects->is_object())
				{
					bool valid = true;
					for (auto item = projects->begin(); item != projects->end(); ++item)
					{
						if (!item.value().is_string()) { valid = false; break; }
						const auto& slug = item.value().get_ref<const std::string&>();
						if (slug.empty() || !std::ranges::all_of(slug, [](unsigned char byte) { return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-'; }))
						{ valid = false; break; }
						registered[slug].push_back(item.key());
					}
					// Upstream resets malformed registry data; readable markers can attest it.
					if (!valid) registered.clear();
				}
		}
		std::error_code error;
		const auto tmp_status = std::filesystem::status(root / "tmp", error);
		if (error == std::errc::no_such_file_or_directory || (!error && !std::filesystem::exists(tmp_status))) return result;
		if (error) { fatal("Could not inspect the Gemini project directory: " + error.message()); return result; }
		if (!std::filesystem::is_directory(tmp_status)) { fatal("Could not inspect the Gemini project directory: not a directory."); return result; }
		std::size_t count = 0;
		for (std::filesystem::directory_iterator it(root / "tmp", error), end; !error && it != end; it.increment(error))
		{
			if (stop_token.stop_requested()) { cancel(); return result; }
			if (max_projects > 0 && ++count > max_projects) { fatal("Gemini project discovery exceeded its directory limit."); return result; }
			const bool directory = it->is_directory(error);
			if (error) break;
			if (!directory) continue;
			const bool symlink = it->is_symlink(error);
			if (error) break;
			if (symlink) continue;
			const auto relative = std::filesystem::path("tmp") / it->path().filename();
			const auto marker = ReadHistoryOwnershipFile(root, relative / ".project_root", 16384);
			const auto owners = registered.find(paths::Utf8PathString(it->path().filename()));
			std::vector<std::string> witnesses = owners != registered.end() ? owners->second : std::vector<std::string>{};
			if (marker.state == HistoryPathState::Readable && !marker.text.empty()) witnesses.push_back(marker.text);
			const auto reject = [&](const char* text) { result.diagnostics.push_back({HistoryDiagnosticScope::Project, relative, witnesses, text}); };
			std::string owner;
			bool conflicting = false;
			for (const auto& candidate : witnesses)
			{
				if (!owner.empty() && !SameHistoryWorkspace(owner, candidate)) conflicting = true;
				owner = candidate;
			}
			if (marker.state == HistoryPathState::Unreadable) { reject("Could not read a Gemini project ownership marker."); continue; }
			if (marker.state == HistoryPathState::Readable && marker.text.empty()) conflicting = true;
			if (conflicting) { reject("Gemini project registry and ownership marker conflict."); continue; }
			if (!owner.empty())
			{
				if (!execution_hosts::IsAbsoluteRemotePath("windows", owner) && !owner.starts_with('/'))
				{ reject("Gemini project ownership is not an absolute workspace path."); continue; }
				result.projects.push_back({root, relative, owner});
			}
		}
		if (error) fatal("Could not finish Gemini project discovery: " + error.message());
		if (stop_token.stop_requested()) cancel();
		for (const auto& project : result.projects) ++result.workspace_claims[HistoryWorkspaceKey(project.workspace)];
		for (const auto& diagnostic : result.diagnostics)
			for (const auto& owner : diagnostic.workspaces) result.rejected_workspaces.insert(HistoryWorkspaceKey(owner));
		// Record ambiguity explicitly; do not select by iteration order.
		for (const auto& project : result.projects)
			if (result.workspace_claims[HistoryWorkspaceKey(project.workspace)] > 1)
				result.diagnostics.push_back({HistoryDiagnosticScope::Project, project.relative, {project.workspace}, "More than one Gemini project claims this workspace."});
		return result;
	}

	inline std::vector<HistoryProject> DiscoverHistoryProjects(std::string* error_out = nullptr, std::stop_token stop_token = {}, std::size_t max_projects = 0)
	{
		auto discovery = DiscoverHistoryProjectsWithDiagnostics(stop_token, max_projects);
		if (error_out) *error_out = discovery.DiagnosticText();
		std::vector<HistoryProject> projects;
		for (auto& project : discovery.projects)
			if (discovery.CanReadWorkspace(project.workspace)) projects.push_back(project);
		return stop_token.stop_requested() ? std::vector<HistoryProject>{} : projects;
	}

	// Canonicalize the provider root, but do not let a child path change its
	// selected project/parent witness through an in-root directory or file alias.
	inline std::optional<std::filesystem::path> HistoryPathInProject(const std::filesystem::path& root, const std::filesystem::path& relative, bool directory)
	{
		if (relative.empty() || relative.is_absolute()) return std::nullopt;
		auto part = relative.begin();
		if (*part++ != "tmp" || part == relative.end()) return std::nullopt;
		const auto slug = *part++;
		if (slug == "." || slug == ".." || part == relative.end() || *part != "chats") return std::nullopt;
		std::filesystem::path native_root;
		std::string path_error;
		if (!io::NativeFilePath(root, native_root, path_error, true)) return std::nullopt;
		std::error_code error;
		const auto canonical_root = std::filesystem::canonical(native_root, error);
		if (error) return std::nullopt;
		const auto project = std::filesystem::canonical(canonical_root / "tmp" / slug, error);
		if (error) return std::nullopt;
		const auto within = project.lexically_relative(canonical_root);
		if (within.empty() || within.is_absolute() || *within.begin() == "..") return std::nullopt;
		auto expected = project;
		for (; part != relative.end(); ++part)
		{
			if (*part == "." || *part == "..") return std::nullopt;
			expected /= *part;
		}
		const auto resolved = std::filesystem::canonical(expected, error);
		if (error) return std::nullopt;
		const bool expected_type = directory ? std::filesystem::is_directory(resolved, error) : std::filesystem::is_regular_file(resolved, error);
		if (error || !expected_type || !SameHistoryWorkspace(paths::Utf8PathString(expected), paths::Utf8PathString(resolved))) return std::nullopt;
		return resolved;
	}

	inline std::optional<std::filesystem::path> HistoryFileInProject(const std::filesystem::path& root, const std::filesystem::path& relative)
	{
		return HistoryPathInProject(root, relative, false);
	}
	inline std::optional<std::filesystem::path> HistoryDirectoryInProject(const std::filesystem::path& root, const std::filesystem::path& relative)
	{
		return HistoryPathInProject(root, relative, true);
	}
}
