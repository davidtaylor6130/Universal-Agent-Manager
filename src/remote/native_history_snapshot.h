#pragma once

#include "common/config/execution_host_config.h"
#include "common/provider/provider_ids.h"
#include "common/paths/workspace_file_reader.h"
#include "common/provider/gemini_history_journal.h"
#include "common/provider/gemini_history_paths.h"
#include "common/utils/env_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/string_utils.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>

namespace uam::remote::native_history
{
	inline constexpr std::size_t kMaxSnapshotBytes = 64 * 1024 * 1024;
	inline constexpr std::size_t kChunkBytes = 128 * 1024;
	inline constexpr std::size_t kMaxSnapshots = 2;
	struct Snapshot
	{
		std::string provider;
		std::string session_id;
		std::string agent_id;
		std::string workspace;
		std::string text;
		std::string workspace_yaml;
		std::string fingerprint;
		std::chrono::steady_clock::time_point expires;
		bool ready = false;
	};

	inline const char* Platform()
	{
#if defined(_WIN32)
		return "windows";
#elif defined(__APPLE__)
		return "macos";
#else
		return "linux";
#endif
	}
	inline bool SameWorkspace(std::string_view first, std::string_view second)
	{
		return execution_hosts::RemotePathsMatch(execution_hosts::IsAbsoluteRemotePath("windows", first) && execution_hosts::IsAbsoluteRemotePath("windows", second) ? "windows" : Platform(), first, second);
	}
	inline bool SafeId(std::string_view id)
	{
		return !id.empty() && id.size() <= 128 && id != "." && id != ".." && id.front() != '-' &&
		    std::ranges::all_of(id, [](unsigned char ch) { return strings::IsAsciiAlnum(ch) || ch == '-' || ch == '_'; });
	}
	inline std::string Fingerprint(const Snapshot& snapshot)
	{
		std::uint64_t digest = hashing::kFnv1a64OffsetBasis;
		for (std::string_view field : {std::string_view(snapshot.provider), std::string_view(snapshot.session_id), std::string_view(snapshot.agent_id), std::string_view(snapshot.workspace), std::string_view(snapshot.text), std::string_view(snapshot.workspace_yaml)})
			hashing::UpdateFnv1a64WithSeparator(digest, field);
		return hashing::Hex64Padded(digest);
	}
	inline std::optional<std::string> Read(const std::filesystem::path& root, const std::filesystem::path& path, std::size_t limit)
	{
		auto file = workspace_files::Read(root, paths::Utf8PathString(path), true, limit);
		if (!file.ok || file.text.find('\0') != std::string::npos) return std::nullopt;
		return std::move(file.text);
	}
	inline std::string String(const nlohmann::json& value, const char* key)
	{
		const auto field = value.find(key);
		return field != value.end() && field->is_string() ? field->get<std::string>() : std::string{};
	}
	inline bool ClaudeIdentity(const std::string& text, const std::string& id, const std::string& workspace, std::string& recorded)
	{
		std::istringstream input(text);
		std::string line;
		bool conversation = false;
		while (std::getline(input, line))
		{
			if (strings::IsBlank(line)) continue;
			const auto row = nlohmann::json::parse(line, nullptr, false);
			if (!row.is_object()) return false;
			const std::string session = String(row, "sessionId");
			if (!session.empty() && session != id) return false;
			const std::string type = String(row, "type");
			if (type != "user" && type != "assistant") continue;
			conversation = true;
			const std::string cwd = String(row, "cwd");
			if (!cwd.empty())
			{
				if (!SameWorkspace(cwd, workspace)) return false;
				recorded = cwd;
			}
		}
		return conversation && !recorded.empty();
	}
	inline bool CopilotIdentity(const std::string& text, const std::string& id, const std::string& workspace, std::string& recorded)
	{
		std::istringstream input(text);
		std::string line;
		while (std::getline(input, line))
		{
			if (strings::IsBlank(line)) continue;
			const auto row = nlohmann::json::parse(line, nullptr, false);
			if (!row.is_object()) return false;
			if (String(row, "type") != "session.start") continue;
			const auto data = row.find("data");
			if (data == row.end() || !data->is_object() || String(*data, "sessionId") != id) return false;
			const auto context = data->find("context");
			if (context == data->end() || !context->is_object()) return false;
			recorded = String(*context, "cwd");
			return !recorded.empty() && SameWorkspace(recorded, workspace);
		}
		return false;
	}
	inline bool ClaudeChildIdentity(const std::string& text, const std::string& parent, const std::string& agent, const std::string& workspace)
	{
		std::istringstream input(text);
		std::string line;
		bool conversation = false, owned_workspace = false;
		while (std::getline(input, line))
		{
			if (strings::IsBlank(line)) continue;
			const auto row = nlohmann::json::parse(line, nullptr, false);
			if (!row.is_object()) return false;
			if ((!String(row, "sessionId").empty() && String(row, "sessionId") != parent) ||
			    (!String(row, "agentId").empty() && String(row, "agentId") != agent)) return false;
			if (String(row, "type") != "user" && String(row, "type") != "assistant") continue;
			if (String(row, "sessionId") != parent || String(row, "agentId") != agent ||
			    !row.contains("isSidechain") || !row["isSidechain"].is_boolean() || !row["isSidechain"].get<bool>()) return false;
			conversation = true;
			const std::string cwd = String(row, "cwd");
			if (!cwd.empty()) { if (!SameWorkspace(cwd, workspace)) return false; owned_workspace = true; }
		}
		return conversation && owned_workspace;
	}
	inline bool GeminiChildDocumentIdentity(const nlohmann::json& row, const std::string& parent, const std::string& agent, const std::string& workspace)
	{
		if (!row.is_object() || String(row, "sessionId") != agent || String(row, "kind") != "subagent") return false;
		if (const auto recorded_parent = row.find("parentSessionId"); recorded_parent != row.end() &&
		    (!recorded_parent->is_string() || *recorded_parent != parent)) return false;
		const auto directories = row.find("directories");
		return directories != row.end() && directories->is_array() && std::ranges::any_of(*directories, [&](const auto& value)
		{
			return value.is_string() && SameWorkspace(value.template get<std::string>(), workspace);
		});
	}
	inline bool GeminiChildIdentity(const std::string& text, const std::string& parent, const std::string& agent, const std::string& workspace)
	{
		const auto metadata = gemini::ReadHistoryMetadata(text);
		return metadata && GeminiChildDocumentIdentity(*metadata, parent, agent, workspace);
	}

	// Discovery is shared by bounded SSH snapshots and uncapped local streaming.
	// The visitor owns reading/attestation and cannot widen the selected filename.
	template <typename Visitor>
	inline bool VisitGeminiParentFiles(const std::string& id, const std::string& workspace, Visitor&& visitor, std::stop_token stop_token = {})
	{
		if (!SafeId(id) || !execution_hosts::IsAbsoluteRemotePath(Platform(), workspace) || stop_token.stop_requested()) return false;
		const auto discovery = gemini::DiscoverHistoryProjectsWithDiagnostics(stop_token, 4096);
		if (!discovery.CanReadWorkspace(workspace) || stop_token.stop_requested()) return false;
		for (const auto& project : discovery.projects)
		{
			if (stop_token.stop_requested()) return false;
			if (!SameWorkspace(project.workspace, workspace)) continue;
			std::error_code files_error;
			std::size_t files = 0;
			for (std::filesystem::directory_iterator file(project.root / project.relative / "chats", files_error), files_end; !files_error && file != files_end; file.increment(files_error))
			{
				if (stop_token.stop_requested() || ++files > 4096) return false;
				const auto extension = file->path().extension();
				if ((extension != ".json" && extension != ".jsonl") || !file->is_regular_file(files_error) || file->is_symlink(files_error)) continue;
				auto current_sibling = file->path(); current_sibling += "l";
				if (extension == ".json" && std::filesystem::exists(current_sibling, files_error)) continue;
				const std::string filename = paths::Utf8PathString(file->path().filename());
				const std::string suffix = extension.string();
				if (filename != id + suffix && !(filename.starts_with("session-") && filename.ends_with("-" + id.substr(0, 8) + suffix))) continue;
				if (!visitor(project.root, project.relative / "chats" / file->path().filename())) return false;
			}
			if (files_error) return false;
		}
		return !stop_token.stop_requested();
	}

	inline std::optional<std::filesystem::path> GeminiFileInsideRoot(const std::filesystem::path& root, const std::filesystem::path& relative)
	{
		return gemini::HistoryFileInProject(root, relative);
	}
	inline bool Load(const std::string& provider, const std::string& id, const std::string& workspace, Snapshot& result, std::string& error, const std::string& agent_id = {})
	{
		result = {};
		error = "The exact native history is unavailable or its workspace identity does not match.";
		if (!SafeId(id) || (!agent_id.empty() && (!SafeId(agent_id) || (provider != uam::provider_ids::kClaudeCli && provider != uam::provider_ids::kGeminiCli))) || !execution_hosts::IsAbsoluteRemotePath(Platform(), workspace)) return false;
		const auto home = env::GetUserHomePath();
		if (provider != uam::provider_ids::kGeminiCli && (!home || !home->is_absolute())) return false;
		result.provider = provider; result.session_id = id; result.agent_id = agent_id;
		if (provider == uam::provider_ids::kCopilotCli)
		{
			const auto root = env::GetTrimmedPath("COPILOT_HOME").value_or(*home / ".copilot") / "session-state";
			auto text = Read(root, std::filesystem::path(id) / "events.jsonl", kMaxSnapshotBytes);
			if (!text || !CopilotIdentity(*text, id, workspace, result.workspace)) return false;
			result.text = std::move(*text);
			if (const auto yaml = Read(root, std::filesystem::path(id) / "workspace.yaml", 16 * 1024)) result.workspace_yaml = *yaml;
		}
		else if (provider == uam::provider_ids::kClaudeCli)
		{
			const auto root = env::GetTrimmedPath("CLAUDE_CONFIG_DIR").value_or(*home / ".claude") / "projects";
			std::error_code scan_error;
			std::size_t count = 0, total = 0;
			for (std::filesystem::directory_iterator it(root, scan_error), end; !scan_error && it != end; it.increment(scan_error))
			{
				if (++count > 4096) return false;
				if (!it->is_directory(scan_error) || it->is_symlink(scan_error)) continue;
				const auto path = it->path().filename() / (id + ".jsonl");
				if (!paths::PathExistsNoThrow(root / path)) continue;
				auto text = Read(root, path, kMaxSnapshotBytes);
				if (!text || text->size() > kMaxSnapshotBytes - total) return false;
				total += text->size();
				std::string recorded;
				if (!ClaudeIdentity(*text, id, workspace, recorded)) continue;
				if (!agent_id.empty())
				{
					text.reset();
					text = Read(root, it->path().filename() / id / "subagents" / ("agent-" + agent_id + ".jsonl"), kMaxSnapshotBytes);
					if (!text || !ClaudeChildIdentity(*text, id, agent_id, workspace)) return false;
				}
				if (!result.text.empty()) { error = "More than one native Claude transcript claims this exact identity."; return false; }
				result.text = std::move(*text); result.workspace = recorded;
			}
			if (scan_error || result.text.empty()) return false;
		}
		else if (provider == uam::provider_ids::kGeminiCli)
		{
			std::size_t total = 0;
			const bool visited = VisitGeminiParentFiles(id, workspace, [&](const auto& root, const auto& parent_file)
			{
				if (!GeminiFileInsideRoot(root, parent_file)) return false;
				auto text = Read(root, parent_file, kMaxSnapshotBytes);
				if (!text || text->size() > kMaxSnapshotBytes - total) return false;
				total += text->size();
				const auto metadata = gemini::ReadHistoryMetadata(*text);
				if (!metadata || !metadata->is_object() || String(*metadata, "sessionId") != id) return true;
				if (!agent_id.empty())
				{
					text.reset();
					auto child = parent_file.parent_path() / id / (agent_id + ".jsonl");
					std::error_code child_error;
					if (!std::filesystem::exists(root / child, child_error)) child = parent_file.parent_path() / id / (agent_id + ".json");
					if (child_error || !GeminiFileInsideRoot(root, child)) return false;
					text = Read(root, child, kMaxSnapshotBytes);
					if (!text || !GeminiChildIdentity(*text, id, agent_id, workspace)) return false;
				}
				if (!result.text.empty()) { error = "More than one native Gemini transcript claims this exact identity."; return false; }
				result.text = std::move(*text); result.workspace = workspace;
				return true;
			});
			if (!visited || result.text.empty()) return false;
		}
		else { error = "This provider has no native file history reader."; return false; }
		if (result.text.size() + result.workspace_yaml.size() > kMaxSnapshotBytes) return false;
		result.fingerprint = Fingerprint(result);
		result.ready = true;
		error.clear();
		return true;
	}
}
