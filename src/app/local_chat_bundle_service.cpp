#include "app/local_chat_bundle_service.h"

#include "common/chat/chat_ids.h"
#include "common/chat/chat_repository.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uam
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr std::string_view kManifestFilename = "manifest.json";
		constexpr std::string_view kChatsDirectoryName = "chats";
		constexpr std::uintmax_t kMaxManifestBytes = 8ULL * 1024ULL * 1024ULL;
		constexpr std::size_t kMaxBundleChatCount = 10000;
		constexpr std::size_t kMaxChatBytes = 64ULL * 1024ULL * 1024ULL;
		constexpr std::size_t kMaxTotalChatBytes = 256ULL * 1024ULL * 1024ULL;

		struct ExportCandidate
		{
			std::string id;
			std::string canonical_text;
		};

		struct ImportCandidate
		{
			ChatSession chat;
			std::string source_id;
			std::string imported_id;
			std::string imported_title;
			bool renamed = false;
		};

		struct StoredChatSizes
		{
			std::optional<std::uintmax_t> primary;
			std::optional<std::uintmax_t> backup;
		};

		std::string CanonicalRelativeChatPath(std::string_view chat_id)
		{
			return std::string(kChatsDirectoryName) + "/" + std::string(chat_id) + ".json";
		}

		void AppendError(std::vector<std::string>& errors, std::string error)
		{
			if (!error.empty())
			{
				errors.push_back(std::move(error));
			}
		}

		bool ReadRequiredString(const nlohmann::json& object, std::string_view field, std::string& value_out)
		{
			const auto it = object.find(std::string(field));
			if (it == object.end() || !it->is_string())
			{
				return false;
			}
			value_out = it->get<std::string>();
			return true;
		}

		std::optional<std::size_t> NonNegativeSize(const nlohmann::json& value)
		{
			if (value.is_number_unsigned())
			{
				return static_cast<std::size_t>(value.get<std::uint64_t>());
			}
			if (value.is_number_integer())
			{
				const std::int64_t parsed = value.get<std::int64_t>();
				if (parsed >= 0)
				{
					return static_cast<std::size_t>(parsed);
				}
			}
			return std::nullopt;
		}

		bool ValidateEmptyOrNewBundleDestination(const fs::path& bundle_path, std::vector<std::string>& errors)
		{
			if (bundle_path.empty())
			{
				AppendError(errors, "Choose a new or empty folder for the local chat bundle.");
				return false;
			}

			std::error_code ec;
			const bool exists = fs::exists(bundle_path, ec);
			if (ec)
			{
				AppendError(errors, "Could not inspect the export folder: " + ec.message());
				return false;
			}
			if (!exists)
			{
				return true;
			}
			if (!fs::is_directory(bundle_path, ec) || ec)
			{
				AppendError(errors, "The local chat export destination must be a folder.");
				return false;
			}
			const bool empty = fs::is_empty(bundle_path, ec);
			if (ec)
			{
				AppendError(errors, "Could not inspect the export folder contents: " + ec.message());
				return false;
			}
			if (!empty)
			{
				AppendError(errors, "The local chat export destination is not empty. Choose a new or empty folder.");
				return false;
			}
			return true;
		}

		bool PreflightExportStorage(
		    const fs::path& data_root,
		    std::vector<std::string>& chat_ids_out,
		    std::vector<std::string>& errors)
		{
			chat_ids_out.clear();
			const fs::path chats_path = AppPaths::UamChatsRootPath(data_root);
			if (!uam::paths::IsDirectoryNoThrow(chats_path))
			{
				return true;
			}

			std::unordered_map<std::string, StoredChatSizes> stored;
			std::error_code iteration_error;
			for (fs::directory_iterator it(chats_path, iteration_error), end;
			     !iteration_error && it != end; it.increment(iteration_error))
			{
				const fs::directory_entry& entry = *it;
				if (uam::paths::IsDirectoryEntryNoThrow(entry))
				{
					AppendError(errors, "Legacy local chat storage must be migrated before export: " +
					                        uam::paths::Utf8PathString(entry.path()));
					return false;
				}
				if (!uam::paths::IsRegularFileNoThrow(entry.path()))
				{
					continue;
				}

				const std::string filename = entry.path().filename().string();
				std::string id;
				bool backup = false;
				if (filename.ends_with(".json.bak"))
				{
					id = filename.substr(0, filename.size() - std::string_view(".json.bak").size());
					backup = true;
				}
				else if (filename.ends_with(".json"))
				{
					id = filename.substr(0, filename.size() - std::string_view(".json").size());
				}
				else
				{
					continue;
				}

				if (!uam::chat_ids::IsSafeStorageChatId(id))
				{
					AppendError(errors, "Local chat storage contains an invalid chat id: " + id);
					return false;
				}
				std::error_code size_error;
				const std::uintmax_t size = fs::file_size(entry.path(), size_error);
				if (size_error || size > kMaxChatBytes)
				{
					AppendError(errors, "Local chat storage exceeds the safe bundle data limit: " + id);
					return false;
				}
				auto [stored_it, inserted] = stored.try_emplace(id);
				if (inserted && stored.size() > kMaxBundleChatCount)
				{
					AppendError(errors, "Local chat storage exceeds the safe bundle chat-count limit.");
					return false;
				}
				StoredChatSizes& sizes = stored_it->second;
				(backup ? sizes.backup : sizes.primary) = size;
			}
			if (iteration_error)
			{
				AppendError(errors, "Could not inspect every local chat before export: " + iteration_error.message());
				return false;
			}
			std::size_t total_bytes = 0;
			nlohmann::json manifest = {
			    {"schema", LocalChatBundleService::kSchema},
			    {"version", LocalChatBundleService::kVersion},
			    {"chatCount", stored.size()},
			    {"chats", nlohmann::json::array()},
			};
			for (const auto& [id, sizes] : stored)
			{
				const std::size_t size = static_cast<std::size_t>(sizes.primary.value_or(sizes.backup.value_or(0)));
				if (total_bytes > kMaxTotalChatBytes - size)
				{
					AppendError(errors, "Local chat storage exceeds the safe bundle data limit: " + id);
					return false;
				}
				total_bytes += size;
				manifest["chats"].push_back({
				    {"id", id},
				    {"file", CanonicalRelativeChatPath(id)},
				    {"byteSize", size},
				});
				chat_ids_out.push_back(id);
			}
			if (manifest.dump(2).size() > kMaxManifestBytes)
			{
				AppendError(errors, "Local chat storage exceeds the safe bundle manifest limit.");
				return false;
			}
			std::ranges::sort(chat_ids_out);
			return true;
		}

		std::vector<ExportCandidate> LoadExportCandidates(
		    const fs::path& data_root,
		    std::vector<std::string>& warnings,
		    std::vector<std::string>& errors)
		{
			std::vector<std::string> chat_ids;
			if (!PreflightExportStorage(data_root, chat_ids, errors))
			{
				return {};
			}
			std::vector<ExportCandidate> candidates;
			candidates.reserve(chat_ids.size());
			std::size_t total_bytes = 0;
			for (const std::string& chat_id : chat_ids)
			{
				std::string validation_warning;
				const std::optional<ChatSession> validated =
				    ChatRepository::LoadLocalChat(data_root, chat_id, true, &validation_warning);
				if (!validation_warning.empty())
				{
					warnings.push_back(validation_warning);
				}
				if (!validated || validated->id != chat_id)
				{
					AppendError(errors, "Canonical chat failed validation before export: " + chat_id +
					                        (validation_warning.empty() ? std::string{} : " (" + validation_warning + ")"));
					return {};
				}

				const fs::path canonical_path = AppPaths::UamChatFilePath(data_root, chat_id);
				std::string canonical_text;
				if (!uam::paths::IsRegularFileNoThrow(canonical_path) ||
				    !uam::io::TryReadTextFile(canonical_path, canonical_text) || canonical_text.empty())
				{
					AppendError(errors, "Could not read canonical chat file for export: " +
					                        uam::paths::Utf8PathString(canonical_path));
					return {};
				}
				if (canonical_text.size() > kMaxChatBytes ||
				    total_bytes > kMaxTotalChatBytes - canonical_text.size())
				{
					AppendError(errors, "Local chat storage exceeds the safe bundle data limit: " + chat_id);
					return {};
				}
				total_bytes += canonical_text.size();

				candidates.push_back({chat_id, std::move(canonical_text)});
			}
			return candidates;
		}

		bool ValidateBundleManifest(
		    const fs::path& bundle_path,
		    std::vector<ImportCandidate>& candidates_out,
		    std::vector<std::string>& errors)
		{
			if (!uam::paths::IsDirectoryNoThrow(bundle_path))
			{
				AppendError(errors, "The selected local chat bundle folder does not exist.");
				return false;
			}

			const fs::path manifest_path = bundle_path / kManifestFilename;
			if (!uam::paths::IsRegularFileNoThrow(manifest_path))
			{
				AppendError(errors, "The selected folder does not contain a readable manifest.json.");
				return false;
			}

			std::error_code manifest_size_error;
			const std::uintmax_t manifest_size = fs::file_size(manifest_path, manifest_size_error);
			if (manifest_size_error || manifest_size > kMaxManifestBytes)
			{
				AppendError(errors, "The local chat bundle manifest exceeds the safe import limit.");
				return false;
			}

			std::string manifest_text;
			if (!uam::io::TryReadTextFile(manifest_path, manifest_text))
			{
				AppendError(errors, "The selected folder does not contain a readable manifest.json.");
				return false;
			}
			const nlohmann::json manifest = nlohmann::json::parse(manifest_text, nullptr, false);
			if (manifest.is_discarded() || !manifest.is_object())
			{
				AppendError(errors, "The local chat bundle manifest contains invalid JSON.");
				return false;
			}

			std::string schema;
			if (!ReadRequiredString(manifest, "schema", schema) || schema != LocalChatBundleService::kSchema)
			{
				AppendError(errors, "The selected folder is not a supported Universal Agent Manager local chat bundle.");
				return false;
			}
			const auto version_it = manifest.find("version");
			const std::optional<std::size_t> version =
			    version_it == manifest.end() ? std::nullopt : NonNegativeSize(*version_it);
			if (!version || *version != static_cast<std::size_t>(LocalChatBundleService::kVersion))
			{
				AppendError(errors, "The local chat bundle version is not supported.");
				return false;
			}

			const auto chats_it = manifest.find("chats");
			const auto count_it = manifest.find("chatCount");
			const std::optional<std::size_t> declared_count =
			    count_it == manifest.end() ? std::nullopt : NonNegativeSize(*count_it);
			if (chats_it == manifest.end() || !chats_it->is_array() || !declared_count ||
			    *declared_count != chats_it->size() || *declared_count > kMaxBundleChatCount)
			{
				AppendError(errors, "The local chat bundle manifest has an invalid chat list or count.");
				return false;
			}

			const fs::path chats_path = bundle_path / kChatsDirectoryName;
			if (!uam::paths::IsDirectoryNoThrow(chats_path))
			{
				AppendError(errors, "The local chat bundle is missing its chats folder.");
				return false;
			}

			std::unordered_set<std::string> declared_ids;
			std::unordered_set<std::string> declared_files;
			std::size_t declared_total_bytes = 0;
			std::vector<ImportCandidate> validated;
			validated.reserve(chats_it->size());
			for (const nlohmann::json& entry : *chats_it)
			{
				std::string id;
				std::string relative_file;
				if (!entry.is_object() || !ReadRequiredString(entry, "id", id) ||
				    !ReadRequiredString(entry, "file", relative_file) ||
				    !uam::chat_ids::IsSafeStorageChatId(id) || !declared_ids.insert(id).second ||
				    relative_file != CanonicalRelativeChatPath(id) || !declared_files.insert(relative_file).second)
				{
					AppendError(errors, "The local chat bundle manifest contains an invalid or duplicate chat entry.");
					return false;
				}

				const fs::path chat_path = AppPaths::UamChatFilePath(bundle_path, id);
				if (!uam::paths::IsRegularFileNoThrow(chat_path))
				{
					AppendError(errors, "The local chat bundle is missing canonical chat file: " + relative_file);
					return false;
				}
				const auto byte_size_it = entry.find("byteSize");
				const std::optional<std::size_t> declared_size =
				    byte_size_it == entry.end() ? std::nullopt : NonNegativeSize(*byte_size_it);
				if (!declared_size || *declared_size > kMaxChatBytes ||
				    declared_total_bytes > kMaxTotalChatBytes - *declared_size)
				{
					AppendError(errors, "Canonical chat data exceeds the safe import limit: " + relative_file);
					return false;
				}
				declared_total_bytes += *declared_size;
				std::error_code size_error;
				const std::uintmax_t actual_size = fs::file_size(chat_path, size_error);
				if (!declared_size || size_error || actual_size != *declared_size)
				{
					AppendError(errors, "Canonical chat file size does not match the bundle manifest: " + relative_file);
					return false;
				}

				std::string validation_warning;
				std::optional<ChatSession> chat =
				    ChatRepository::LoadLocalChat(bundle_path, id, true, &validation_warning);
				if (!chat || chat->id != id || !chat->messages_loaded)
				{
					AppendError(errors, "Canonical chat failed validation: " + relative_file +
					                        (validation_warning.empty() ? std::string{} : " (" + validation_warning + ")"));
					return false;
				}
				const std::string imported_title = chat->title;
				validated.push_back({std::move(*chat), id, id, imported_title, false});
			}

			std::error_code iteration_error;
			for (fs::directory_iterator it(chats_path, iteration_error), end;
			     !iteration_error && it != end; it.increment(iteration_error))
			{
				const fs::directory_entry& entry = *it;
				if (!uam::paths::IsRegularFileWithExtensionNoThrow(entry, ".json"))
				{
					continue;
				}
				const std::string relative_file = CanonicalRelativeChatPath(entry.path().stem().string());
				if (!declared_files.contains(relative_file))
				{
					AppendError(errors, "The local chat bundle contains an undeclared canonical chat file: " + relative_file);
					return false;
				}
			}
			if (iteration_error)
			{
				AppendError(errors, "Could not inspect every canonical chat in the bundle: " + iteration_error.message());
				return false;
			}

			candidates_out = std::move(validated);
			return true;
		}

		void AppendStoredChatIds(
		    const fs::path& data_root,
		    std::unordered_set<std::string>& ids_out,
		    std::vector<std::string>& warnings_out)
		{
			std::string warning;
			for (const ChatSession& chat : ChatRepository::LoadLocalChats(data_root, &warning))
			{
				ids_out.insert(chat.id);
			}
			if (!warning.empty())
			{
				warnings_out.push_back(warning);
			}

			const fs::path chats_path = AppPaths::UamChatsRootPath(data_root);
			std::error_code ec;
			for (fs::directory_iterator it(chats_path, ec), end; !ec && it != end; it.increment(ec))
			{
				const std::string filename = it->path().filename().string();
				std::string id;
				if (filename.ends_with(".json.bak"))
				{
					id = filename.substr(0, filename.size() - std::string_view(".json.bak").size());
				}
				else if (filename.ends_with(".json"))
				{
					id = filename.substr(0, filename.size() - std::string_view(".json").size());
				}
				if (uam::chat_ids::IsSafeStorageChatId(id))
				{
					ids_out.insert(std::move(id));
				}
			}
		}

		std::string UniqueImportedId(
		    std::string_view source_id,
		    std::unordered_set<std::string>& used_ids,
		    std::size_t& suffix_number_out)
		{
			const std::string base = std::string(source_id) + "--imported";
			std::string candidate = base;
			std::size_t suffix_number = 1;
			while (used_ids.contains(candidate))
			{
				++suffix_number;
				candidate = base + "-" + std::to_string(suffix_number);
			}
			used_ids.insert(candidate);
			suffix_number_out = suffix_number;
			return candidate;
		}

		std::string ImportedTitle(std::string_view title, std::string_view source_id, std::size_t suffix_number)
		{
			const std::string base = uam::strings::NonEmptyOrFallback(uam::strings::Trim(title), std::string(source_id));
			return suffix_number <= 1
			           ? base + " (Imported)"
			           : base + " (Imported " + std::to_string(suffix_number) + ")";
		}

		void RemapChatReference(std::string& value, const std::unordered_map<std::string, std::string>& id_map)
		{
			const auto mapped = id_map.find(value);
			if (mapped != id_map.end())
			{
				value = mapped->second;
			}
		}

		void StripImportedExecutionAuthority(ChatSession& chat)
		{
			chat.native_session_id.clear();
			chat.execution_host_id = "local";
			chat.remote_turn_reconnect_pending = false;
			chat.remote_stop_cleanup_pending = false;
			chat.remote_restart_pending = false;
			chat.remote_process_control_token.clear();
			chat.remote_delivered_stdout_cursor = 0;
			chat.remote_delivered_stderr_cursor = 0;
			chat.folder_id.clear();
			chat.linked_files.clear();
			chat.workspace_directory.clear();
			chat.workspace_isolation_kind.clear();
			chat.workspace_source_directory.clear();
			chat.workspace_base_ref.clear();
			chat.workspace_branch_name.clear();
			chat.workspace_worktree_directory.clear();
			chat.imported_read_only = true;
			chat.approval_mode = "default";
			chat.uam_agent_id = "build";
			chat.agent_run_id.clear();
			chat.goal_owner_chat_id.clear();
			chat.goal_iteration_goal_id.clear();
			chat.goal_iteration_turn_kind.clear();
			chat.goal_iteration_repair_attempts = 0;
			chat.uam_control_enabled = false;
			chat.command_safety_tier = "off";
			chat.extra_flags.clear();
			chat.memory_enabled = false;
			chat.memory_level = "off";
			chat.memory_last_processed_message_count = 0;
			chat.memory_last_processed_at.clear();
			chat.small_model_mode = false;
			chat.goals.clear();
			chat.active_goal_id.clear();
			chat.uam_control_audit.clear();
		}
	} // namespace

	LocalChatBundleExportResult LocalChatBundleService::Export(
	    const std::filesystem::path& data_root,
	    const std::filesystem::path& bundle_path)
	{
		LocalChatBundleExportResult result;
		result.bundle_path = bundle_path;
		if (!ValidateEmptyOrNewBundleDestination(bundle_path, result.errors))
		{
			return result;
		}

		std::vector<ExportCandidate> candidates =
		    LoadExportCandidates(data_root, result.warnings, result.errors);
		if (!result.errors.empty())
		{
			return result;
		}
		result.total_count = candidates.size();

		nlohmann::json manifest = {
		    {"schema", kSchema},
		    {"version", kVersion},
		    {"chatCount", candidates.size()},
		    {"chats", nlohmann::json::array()},
		};
		for (const ExportCandidate& candidate : candidates)
		{
			const std::string relative_file = CanonicalRelativeChatPath(candidate.id);
			manifest["chats"].push_back({
			    {"id", candidate.id},
			    {"file", relative_file},
			    {"byteSize", candidate.canonical_text.size()},
			});
		}
		const std::string manifest_text = manifest.dump(2);
		if (manifest_text.size() > kMaxManifestBytes)
		{
			AppendError(result.errors, "Local chat storage exceeds the safe bundle manifest limit.");
			return result;
		}

		std::error_code directory_error;
		if (!uam::paths::CreateDirectoriesNoThrow(bundle_path / kChatsDirectoryName, &directory_error))
		{
			AppendError(result.errors, "Could not create the local chat bundle folder: " + directory_error.message());
			return result;
		}

		for (const ExportCandidate& candidate : candidates)
		{
			const std::string relative_file = CanonicalRelativeChatPath(candidate.id);
			const fs::path destination = AppPaths::UamChatFilePath(bundle_path, candidate.id);
			if (!uam::io::WriteTextFile(destination, candidate.canonical_text))
			{
				AppendError(result.errors, "Failed to export canonical chat file: " + relative_file);
				continue;
			}
			++result.exported_count;
		}

		if (!result.errors.empty())
		{
			result.degraded = result.exported_count > 0;
			return result;
		}
		if (!uam::io::WriteTextFile(bundle_path / kManifestFilename, manifest_text))
		{
			AppendError(result.errors, "Failed to write the local chat bundle manifest.");
			result.degraded = result.exported_count > 0;
			return result;
		}

		result.ok = true;
		return result;
	}

	LocalChatBundleImportResult LocalChatBundleService::Import(
	    const std::filesystem::path& data_root,
	    const std::filesystem::path& bundle_path)
	{
		LocalChatBundleImportResult result;
		result.bundle_path = bundle_path;
		std::vector<ImportCandidate> candidates;
		if (!ValidateBundleManifest(bundle_path, candidates, result.errors))
		{
			return result;
		}
		result.total_count = candidates.size();
		if (!candidates.empty())
		{
			result.warnings.push_back(
			    "Imported chats are passive transcripts. Reconnect workspaces, provider sessions, agents, goals, attachments, and command permissions explicitly before running them.");
		}

		std::unordered_set<std::string> used_ids;
		AppendStoredChatIds(data_root, used_ids, result.warnings);
		std::unordered_map<std::string, std::string> id_map;
		for (ImportCandidate& candidate : candidates)
		{
			candidate.imported_id = candidate.source_id;
			candidate.imported_title = candidate.chat.title;
			if (used_ids.contains(candidate.imported_id))
			{
				std::size_t suffix_number = 1;
				candidate.imported_id = UniqueImportedId(candidate.source_id, used_ids, suffix_number);
				candidate.imported_title = ImportedTitle(candidate.chat.title, candidate.source_id, suffix_number);
				candidate.renamed = true;
				++result.renamed_count;
			}
			else
			{
				used_ids.insert(candidate.imported_id);
			}
			id_map.emplace(candidate.source_id, candidate.imported_id);
		}

		for (ImportCandidate& candidate : candidates)
		{
			candidate.chat.id = candidate.imported_id;
			candidate.chat.title = candidate.imported_title;
			StripImportedExecutionAuthority(candidate.chat);
			RemapChatReference(candidate.chat.parent_chat_id, id_map);
			RemapChatReference(candidate.chat.branch_root_chat_id, id_map);

			LocalChatBundleImportItem item;
			item.source_id = candidate.source_id;
			item.imported_id = candidate.imported_id;
			item.imported_title = candidate.imported_title;
			item.renamed = candidate.renamed;
			item.imported = ChatRepository::SaveChatIfAbsent(data_root, candidate.chat);
			if (item.imported)
			{
				++result.imported_count;
			}
			else
			{
				++result.failed_count;
				item.error = "Could not save imported chat without overwriting destination storage: " + candidate.imported_id;
				result.errors.push_back(item.error);
			}
			result.items.push_back(std::move(item));
		}

		result.ok = result.failed_count == 0;
		result.degraded = result.imported_count > 0 && result.failed_count > 0;
		return result;
	}
} // namespace uam
