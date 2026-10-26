#include "app/local_chat_bundle_service.h"

#include "app/runtime_orchestration_services.h"

#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_ids.h"
#include "common/chat/chat_repository.h"
#include "common/chat/message_attachment_sources.h"
#include "common/chat/native_chat_identity.h"
#include "common/chat/native_attachment_assets.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/base64.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/env_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <limits>
#include <optional>
#include <stdexcept>
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
		constexpr std::size_t kMaxNativeMetadataBytes = 1024U * 1024U;
		constexpr std::string_view kLegacyImportLedgerFilename = "legacy-data-imports.json";
		constexpr std::size_t kMaxLegacyChatBytes = 256ULL * 1024ULL * 1024ULL; // Matches the repository chat limit.

		// legacy-data-imports.json: copied and declined ("Brand new") sources plus every chat/folder ID
		// ever imported, so a moved, renamed or partly imported source never brings back a deletion.
		bool LoadLegacyImportLedger(const fs::path& data_root, nlohmann::json& ledger)
		{
			ledger = nlohmann::json::object();
			const fs::path path = data_root / kLegacyImportLedgerFilename;
			if (uam::paths::PathExistsNoThrow(path)) ledger = nlohmann::json::parse(uam::io::ReadTextFile(path), nullptr, false);
			if (!ledger.is_object()) return false;
			for (const char* key : {"completed_sources", "declined_sources", "imported_chat_ids", "imported_folder_ids"})
			{
				nlohmann::json& value = ledger[key];
				if (value.is_null()) value = nlohmann::json::array();
				if (!value.is_array()) return false;
			}
			return true;
		}

		bool LedgerContains(const nlohmann::json& values, const std::string& value)
		{
			return std::find(values.begin(), values.end(), value) != values.end();
		}

		struct BundleOriginal
		{
			native_assets::AssetReference reference;
			native_assets::Payload payload;
		};

		struct ExportCandidate
		{
			std::string id;
			std::string canonical_text;
			ChatSession chat;
			nlohmann::json document;
			std::vector<BundleOriginal> originals;
			std::unique_ptr<native_assets::PreparedBatch> prepared;
		};

		struct ImportCandidate
		{
			ChatSession chat;
			nlohmann::json document;
			std::string source_id;
			std::string imported_id;
			std::string imported_title;
			bool renamed = false;
			std::vector<BundleOriginal> originals;
			std::unique_ptr<native_assets::PreparedBatch> prepared;
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
				const auto size = value.get<std::uint64_t>();
				if (size <= std::numeric_limits<std::size_t>::max()) return static_cast<std::size_t>(size);
				return std::nullopt;
			}
			if (value.is_number_integer())
			{
				const std::int64_t parsed = value.get<std::int64_t>();
				if (parsed >= 0 && static_cast<std::uint64_t>(parsed) <= std::numeric_limits<std::size_t>::max())
				{
					return static_cast<std::size_t>(parsed);
				}
			}
			return std::nullopt;
		}

		std::string OriginalIdentity(const native_assets::AssetReference& reference)
		{
			return reference.key + '\n' + reference.mime_type;
		}

		native_assets::AssetReference OriginalReference(const MessageAttachment& attachment)
		{
			return {attachment.native_asset_key, attachment.mime_type, attachment.content_digest,
			    static_cast<std::size_t>(attachment.size_bytes)};
		}

		template <typename Visit> bool VisitAttachments(ChatSession& chat, const Visit& visit)
		{
			for (auto& message : chat.messages)
				for (auto& attachment : message.attachments) if (!visit(attachment)) return false;
			for (auto& prompt : chat.acp_queued_prompts)
				for (auto& attachment : prompt.attachments) if (!visit(attachment)) return false;
			return true;
		}

		bool RawAttachmentMatches(const nlohmann::json& raw, const MessageAttachment& attachment)
		{
			for (const auto& [field, value] : {
			         std::pair{"id", &attachment.id}, std::pair{"name", &attachment.name}, std::pair{"kind", &attachment.kind},
			         std::pair{"mime_type", &attachment.mime_type}, std::pair{"path", &attachment.path},
			         std::pair{"sourceUri", &attachment.source_uri}, std::pair{"originExecutionHostId", &attachment.origin_execution_host_id},
			         std::pair{"nativePartKey", &attachment.native_part_key}, std::pair{"nativeToolCallId", &attachment.native_tool_call_id},
			         std::pair{"nativeSourceField", &attachment.native_source_field}, std::pair{"nativeAssetKey", &attachment.native_asset_key},
			         std::pair{"contentDigest", &attachment.content_digest}, std::pair{"previewKey", &attachment.preview_key},
			         std::pair{"previewOriginPath", &attachment.preview_origin_path}})
			{
				const auto stored = raw.find(field);
				if (stored == raw.end() ? !value->empty() : (!stored->is_string() || stored->get<std::string>() != *value)) return false;
			}
			const auto source = raw.find("source");
			if (source != raw.end() && (!source->is_string() || source->get<std::string>() != attachment_sources::Name(attachment.source))) return false;
			const auto size = raw.find("size_bytes");
			if (size != raw.end())
			{
				const auto bytes = NonNegativeSize(*size);
				if (!bytes || *bytes != attachment.size_bytes) return false;
			}
			const auto copied = raw.find("copied");
			return copied == raw.end() || (copied->is_boolean() && copied->get<bool>() == attachment.copied);
		}

		bool NativeMetadataWithinBounds(const nlohmann::json& document, const ChatSession& chat, std::size_t& metadata_bytes)
		{
			const auto inspect = [&](const nlohmann::json& rows, const auto& model_rows)
			{
				if (!rows.is_array() || rows.size() != model_rows.size()) return false;
				for (std::size_t row = 0; row < rows.size(); ++row)
				{
					if (!rows[row].is_object()) return false;
					const auto parts = rows[row].find("attachments");
					if (parts == rows[row].end()) continue;
					if (!parts->is_array()) return false;
					bool native = false;
					for (const auto& part : *parts)
					{
						if (!part.is_object()) return false;
						if (part.contains("source") || part.contains("nativeAssetKey") || part.contains("sourceUri") ||
						    part.contains("nativePartKey") || part.contains("contentDigest") || part.contains("originExecutionHostId") ||
						    part.contains("nativeToolCallId") || part.contains("nativeSourceField"))
						{
							native = true;
							const auto bytes = part.dump().size();
							if (bytes > kMaxNativeMetadataBytes - metadata_bytes) return false;
							metadata_bytes += bytes;
						}
					}
					// Native rows may not be silently truncated or normalized away.
					if (native && parts->size() != model_rows[row].attachments.size()) return false;
					if (native)
						for (std::size_t index = 0; index < parts->size(); ++index)
							if (!RawAttachmentMatches((*parts)[index], model_rows[row].attachments[index])) return false;
				}
				return true;
			};
			const auto messages = document.find("messages");
			if (messages != document.end() && !inspect(*messages, chat.messages)) return false;
			const auto queue = document.find("acp_queued_prompts");
			return queue == document.end() || inspect(*queue, chat.acp_queued_prompts);
		}

		bool CollectOriginals(const fs::path& root, ChatSession& chat, const std::string& canonical,
		    const nlohmann::json& document, const std::shared_ptr<native_assets::PayloadBudget>& budget,
		    std::size_t& metadata_bytes, std::vector<BundleOriginal>& originals, std::vector<std::string>& errors)
		{
			const auto archive = ChatRepository::ParseArchivalQueuedPrompts(canonical, chat.id);
			if (!archive)
			{
				AppendError(errors, "Canonical queued attachment archive is invalid: " + chat.id);
				return false;
			}
			chat.acp_queued_prompts = *archive;
			if (!NativeMetadataWithinBounds(document, chat, metadata_bytes))
			{
				AppendError(errors, "Canonical native attachment metadata exceeds the archive bounds: " + chat.id);
				return false;
			}
			std::unordered_set<std::string> seen;
			const bool valid = VisitAttachments(chat, [&](const MessageAttachment& attachment)
			{
				if (!attachment_sources::IsPersistable(attachment, chat.id)) return false;
				if (attachment.source != AttachmentSource::OwnedNativeAsset) return true;
				const auto reference = OriginalReference(attachment);
				if (!seen.insert(OriginalIdentity(reference)).second) return true;
				if (budget->UsedCount() >= native_assets::kMaxAssetCount ||
				    reference.size_bytes > native_assets::kMaxBatchBytes - budget->UsedBytes()) return false;
				// The receipt-backed reader is the only source of transferable bytes.
				const auto original = native_assets::ReadOwned(fs::absolute(root), chat.id, reference.key);
				if (!original.ok || original.bytes.size() != reference.size_bytes) return false;
				std::string error;
				auto payload = native_assets::MakeInlinePayload(uam::base64::Encode(original.bytes), reference.mime_type, budget, error);
				if (!payload || payload->Digest() != reference.digest || payload->DecodedSize() != reference.size_bytes) return false;
				originals.push_back({reference, std::move(payload)});
				return true;
			});
			if (!valid) AppendError(errors, "A native original is missing, invalid or exceeds the operation budget: " + chat.id);
			return valid;
		}

		bool PrepareOriginals(const fs::path& root, std::string_view final_owner, const std::vector<BundleOriginal>& originals,
		    std::unique_ptr<native_assets::PreparedBatch>& prepared, std::vector<std::string>& errors)
		{
			if (originals.empty()) return true;
			std::vector<native_assets::Payload> payloads;
			for (const auto& original : originals) payloads.push_back(original.payload);
			std::string error;
			prepared = native_assets::PreparedBatch::Prepare(fs::absolute(root), final_owner, payloads, error);
			if (!prepared) AppendError(errors, "Could not prepare portable native originals: " + error);
			return prepared != nullptr;
		}

		void RebindOriginals(ChatSession& chat, nlohmann::json* document, const std::vector<BundleOriginal>& originals,
		    const native_assets::PreparedBatch& prepared)
		{
			std::unordered_map<std::string, std::string> keys;
			for (std::size_t index = 0; index < originals.size(); ++index)
				keys.emplace(OriginalIdentity(originals[index].reference), prepared.Assets().at(index).key);
			VisitAttachments(chat, [&](MessageAttachment& attachment)
			{
				if (attachment.source == AttachmentSource::OwnedNativeAsset)
					attachment.native_asset_key = keys.at(OriginalIdentity(OriginalReference(attachment)));
				return true;
			});
			if (!document) return;
			for (const auto collection : {"messages", "acp_queued_prompts"})
			{
				auto rows = document->find(collection);
				if (rows == document->end()) continue;
				for (auto& row : *rows)
				{
					auto attachments = row.find("attachments");
					if (attachments == row.end()) continue;
					for (auto& part : *attachments)
						if (part.value("source", "") == "owned_native_asset")
							part["nativeAssetKey"] = keys.at(part.at("nativeAssetKey").get<std::string>() + '\n' + part.at("mime_type").get<std::string>());
				}
			}
		}

		nlohmann::json OriginalInventory(const native_assets::PreparedBatch* prepared)
		{
			auto inventory = nlohmann::json::array();
			std::map<std::string, native_assets::AssetReference> declared;
			if (prepared)
				for (const auto& reference : prepared->Assets())
				{
					const auto [found, inserted] = declared.emplace(OriginalIdentity(reference), reference);
					if (!inserted)
					{
						if (found->second.digest != reference.digest || found->second.size_bytes != reference.size_bytes)
							throw std::length_error("Converged native original inventory metadata disagrees.");
						continue;
					}
					inventory.push_back({{"key", reference.key}, {"mimeType", reference.mime_type},
					    {"digest", reference.digest}, {"byteSize", reference.size_bytes}});
				}
			return inventory;
		}

		bool ValidateOriginalInventory(const nlohmann::json& entry, std::size_t version,
		    const std::vector<BundleOriginal>& originals)
		{
			const auto inventory = entry.find("originals");
			if (version == 1) return originals.empty() && inventory == entry.end();
			if (inventory == entry.end() || !inventory->is_array() || inventory->size() != originals.size()) return false;
			std::map<std::string, native_assets::AssetReference> remaining;
			for (const auto& original : originals) remaining.emplace(OriginalIdentity(original.reference), original.reference);
			for (const auto& item : *inventory)
			{
				native_assets::AssetReference reference;
				if (!item.is_object() || !ReadRequiredString(item, "key", reference.key) ||
				    !ReadRequiredString(item, "mimeType", reference.mime_type) || !ReadRequiredString(item, "digest", reference.digest)) return false;
				const auto size = item.find("byteSize");
				const auto bytes = size == item.end() ? std::nullopt : NonNegativeSize(*size);
				const auto found = remaining.find(OriginalIdentity(reference));
				if (!bytes || found == remaining.end() || found->second.digest != reference.digest || found->second.size_bytes != *bytes) return false;
				remaining.erase(found);
			}
			return remaining.empty();
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
			    {"nativeAssetStorage", native_assets::kStoreDirectory},
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
				    {"originals", nlohmann::json::array()},
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
			std::size_t metadata_bytes = 0;
			const auto budget = std::make_shared<native_assets::PayloadBudget>();
			for (const std::string& chat_id : chat_ids)
			{
				std::string validation_warning;
				std::optional<ChatSession> validated =
				    ChatRepository::LoadLocalChatBounded(data_root, chat_id, kMaxChatBytes);
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
				    !uam::io::TryReadTextFile(canonical_path, canonical_text, kMaxChatBytes) || canonical_text.empty())
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

				{
					// Certify the original queue text before any DOM redaction/dump.
					// This rejects inert numbers that the portable codec would round.
					if (!ChatRepository::ParsePassiveQueuedPromptArchive(canonical_text, chat_id))
					{
						AppendError(errors, "Canonical queued archive cannot be preserved losslessly: " + chat_id);
						return {};
					}
					auto portable = nlohmann::json::parse(canonical_text, nullptr, false);
					if (!portable.is_object())
					{
						AppendError(errors, "Could not remove private process authority from the exported transcript: " + chat_id);
						return {};
					}
					const bool has_authority =
					    (portable.contains("remote_opencode_native_token") && portable["remote_opencode_native_token"] != "") ||
					    (portable.contains("remote_process_control_token") && portable["remote_process_control_token"] != "") ||
					    (portable.contains("remote_manual_compaction_boundary_json") && portable["remote_manual_compaction_boundary_json"] != "");
					if (has_authority)
					{
						portable["remote_opencode_native_token"] = "";
						portable["remote_opencode_native_port"] = 0;
						portable["remote_process_control_token"] = "";
						portable["remote_manual_compaction_pending"] = false;
						portable["remote_manual_compaction_boundary_json"] = "";
						canonical_text = portable.dump(2);
					}
				}
				ExportCandidate candidate;
				candidate.id = chat_id;
				candidate.canonical_text = std::move(canonical_text);
				candidate.chat = std::move(*validated);
				candidate.document = nlohmann::json::parse(candidate.canonical_text, nullptr, false);
				if (!CollectOriginals(data_root, candidate.chat, candidate.canonical_text, candidate.document,
				        budget, metadata_bytes, candidate.originals, errors)) return {};
				candidates.push_back(std::move(candidate));
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
			std::error_code link_error;
			if (!fs::is_regular_file(fs::symlink_status(manifest_path, link_error)) || link_error)
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
			if (!uam::io::TryReadTextFile(manifest_path, manifest_text, kMaxManifestBytes))
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
			if (!version || (*version != 1 && *version != static_cast<std::size_t>(LocalChatBundleService::kVersion)))
			{
				AppendError(errors, "The local chat bundle version is not supported.");
				return false;
			}
			std::string storage;
			if (*version == 2 && (!ReadRequiredString(manifest, "nativeAssetStorage", storage) || storage != native_assets::kStoreDirectory))
			{
				AppendError(errors, "The local chat bundle native original storage version is not supported.");
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
			if (!fs::is_directory(fs::symlink_status(chats_path, link_error)) || link_error)
			{
				AppendError(errors, "The local chat bundle is missing its chats folder.");
				return false;
			}

			std::unordered_set<std::string> declared_ids;
			std::unordered_set<std::string> declared_files;
			std::size_t declared_total_bytes = 0;
			std::size_t metadata_bytes = 0;
			const auto budget = std::make_shared<native_assets::PayloadBudget>();
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
				if (!fs::is_regular_file(fs::symlink_status(chat_path, link_error)) || link_error)
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

				std::string canonical;
				if (!uam::io::TryReadTextFile(chat_path, canonical, *declared_size) || canonical.size() != *declared_size)
				{
					AppendError(errors, "Canonical chat changed or could not be read within its declared bound: " + relative_file);
					return false;
				}
				std::string validation_warning;
				std::optional<ChatSession> chat = ChatRepository::LoadLocalChatBounded(bundle_path, id, *declared_size);
				if (!chat || chat->id != id || !chat->messages_loaded)
				{
					AppendError(errors, "Canonical chat failed validation: " + relative_file +
					                        (validation_warning.empty() ? std::string{} : " (" + validation_warning + ")"));
					return false;
				}
				ImportCandidate candidate;
				candidate.chat = std::move(*chat);
				candidate.source_id = id;
				candidate.imported_id = id;
				candidate.imported_title = candidate.chat.title;
				candidate.document = nlohmann::json::parse(canonical, nullptr, false);
				if (!candidate.document.is_object() || !CollectOriginals(bundle_path, candidate.chat, canonical, candidate.document,
				        budget, metadata_bytes, candidate.originals, errors)) return false;
				if (!ValidateOriginalInventory(entry, *version, candidate.originals))
				{
					AppendError(errors, "Canonical native originals do not match the versioned bundle inventory: " + relative_file);
					return false;
				}
				validated.push_back(std::move(candidate));
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
			chat.acp_unsent_prompt_recoveries.clear();
			// Imported transcripts are independent passive copies. Their source UAM
			// parent may collide with a destination chat, even when IDs are remapped.
			chat.subagent_parent_chat_id.clear();
			chat.subagent_parent_created_at.clear();
			chat.temporary_parent_chat_id.clear();
			chat.side_cleanup_requested = false;
			chat.provider_handoff_context.clear();
			chat.provider_handoff_session_id.clear();
			chat.provider_handoff_cli_contexts.clear();
			chat.native_session_id.clear();
			chat.execution_host_id = "local";
			chat.remote_turn_reconnect_pending = false;
			chat.remote_process_exists = false;
			chat.remote_structured_protocol.clear();
			chat.remote_prompt_image_supported = false;
			chat.remote_opencode_native_port = 0;
			chat.remote_opencode_native_token.clear();
			chat.remote_manual_compaction_pending = false;
			chat.remote_manual_compaction_boundary_json.clear();
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
		try
		{
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

			std::error_code directory_error;
			if (!uam::paths::CreateDirectoriesNoThrow(bundle_path / kChatsDirectoryName, &directory_error))
			{
				AppendError(result.errors, "Could not create the local chat bundle folder: " + directory_error.message());
				return result;
			}
			std::size_t total_chat_bytes = 0;
			for (auto& candidate : candidates)
			{
				if (!PrepareOriginals(bundle_path, candidate.id, candidate.originals, candidate.prepared, result.errors)) return result;
				if (candidate.prepared)
				{
					RebindOriginals(candidate.chat, &candidate.document, candidate.originals, *candidate.prepared);
					candidate.canonical_text = candidate.document.dump(2);
				}
				if (candidate.canonical_text.size() > kMaxChatBytes || total_chat_bytes > kMaxTotalChatBytes - candidate.canonical_text.size())
				{
					AppendError(result.errors, "Portable chat data exceeds the safe bundle data limit.");
					return result;
				}
				total_chat_bytes += candidate.canonical_text.size();
			}

			nlohmann::json manifest = {
			    {"schema", kSchema},
			    {"version", kVersion},
			    {"nativeAssetStorage", native_assets::kStoreDirectory},
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
				    {"originals", OriginalInventory(candidate.prepared.get())},
				});
			}
			const std::string manifest_text = manifest.dump(2);
			if (manifest_text.size() > kMaxManifestBytes)
			{
				AppendError(result.errors, "Local chat storage exceeds the safe bundle manifest limit.");
				return result;
			}

			for (const ExportCandidate& candidate : candidates)
			{
				const std::string relative_file = CanonicalRelativeChatPath(candidate.id);
				const fs::path destination = AppPaths::UamChatFilePath(bundle_path, candidate.id);
				std::string receipt_error;
				if (candidate.prepared && !candidate.prepared->MarkPublicationAttempted(receipt_error))
				{
					AppendError(result.errors, "Could not retain exported originals before publication: " + receipt_error);
					continue;
				}
				if (!uam::io::WriteTextFile(destination, candidate.canonical_text))
				{
					AppendError(result.errors, "Failed to export canonical chat file: " + relative_file);
					continue;
				}
				++result.exported_count;
				// A true canonical write is the primary ACK; manifest/summary success
				// alone cannot acknowledge originals, and uncertain generations stay retained.
				if (candidate.prepared && !candidate.prepared->MarkPublished(receipt_error))
					result.warnings.push_back("Exported originals were retained with an uncertain acknowledgement receipt.");
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
		catch (...)
		{
			result.ok = false;
			result.degraded = result.exported_count > 0;
			AppendError(result.errors, "Local chat export failed; any attempted original generations were retained.");
			return result;
		}
	}

	LocalChatBundleImportResult LocalChatBundleService::Import(
	    const std::filesystem::path& data_root,
	    const std::filesystem::path& bundle_path)
	{
		LocalChatBundleImportResult result;
		result.bundle_path = bundle_path;
		try
		{
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

			const bool has_originals = std::any_of(candidates.begin(), candidates.end(), [](const auto& candidate)
			    { return !candidate.originals.empty(); });
			if (has_originals)
			{
				std::error_code root_error;
				if (!uam::paths::CreateDirectoriesNoThrow(data_root, &root_error))
				{
					AppendError(result.errors, "Could not create the native original destination root.");
					return result;
				}
			}
			// Complete validation of every incoming original happened above. Prepare
			// all destination generations before publishing the first chat, binding
			// each batch to the exact collision-resolved UAM identity.
			for (auto& candidate : candidates)
			{
				if (!PrepareOriginals(data_root, candidate.imported_id, candidate.originals, candidate.prepared, result.errors)) return result;
				if (candidate.prepared) RebindOriginals(candidate.chat, &candidate.document, candidate.originals, *candidate.prepared);
				candidate.chat.id = candidate.imported_id;
				candidate.chat.title = candidate.imported_title;
				StripImportedExecutionAuthority(candidate.chat);
				candidate.document["id"] = candidate.imported_id;
				candidate.chat.passive_queued_prompt_archive = ChatRepository::ParsePassiveQueuedPromptArchive(
				    candidate.document.dump(), candidate.imported_id);
				if (!candidate.chat.passive_queued_prompt_archive)
				{
					AppendError(result.errors, "Could not prepare a complete final-owner passive attachment archive.");
					return result;
				}
				candidate.chat.acp_queued_prompts.clear();
				candidate.chat.acp_queue_held = false;
				candidate.chat.acp_dispatched_queued_prompt_count = 0;
				candidate.chat.acp_queue_storage_revision.clear();
				candidate.chat.acp_queue_full_save_revision.clear();
				candidate.chat.acp_queue_storage_error = false;
				RemapChatReference(candidate.chat.parent_chat_id, id_map);
				RemapChatReference(candidate.chat.branch_root_chat_id, id_map);
			}

			for (ImportCandidate& candidate : candidates)
			{
				LocalChatBundleImportItem item;
				item.source_id = candidate.source_id;
				item.imported_id = candidate.imported_id;
				item.imported_title = candidate.imported_title;
				item.renamed = candidate.renamed;
				std::string receipt_error;
				item.imported = (!candidate.prepared || candidate.prepared->MarkPublicationAttempted(receipt_error)) &&
				    ChatRepository::SaveChatIfAbsent(data_root, candidate.chat);
				if (item.imported)
				{
					++result.imported_count;
					// SaveChatIfAbsent's actual primary ACK is required. No false save,
					// normalization or prepared generation may call MarkPublished.
					if (candidate.prepared && !candidate.prepared->MarkPublished(receipt_error))
						result.warnings.push_back("Imported originals were retained with an uncertain acknowledgement receipt.");
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
		catch (...)
		{
			result.ok = false;
			result.degraded = result.imported_count > 0;
			AppendError(result.errors, "Local chat import failed; any attempted original generations were retained.");
			return result;
		}
	}

	LocalChatBundleImportResult LocalChatBundleService::ImportLegacyDataRoot(
	    const std::filesystem::path& data_root,
	    const std::filesystem::path& legacy_root)
	{
		LocalChatBundleImportResult result;
		result.bundle_path = legacy_root;
		try
		{
			const std::string source_key = uam::paths::NormalizedNativePathString(legacy_root);
			if (source_key == uam::paths::NormalizedNativePathString(data_root))
			{
				result.ok = true;
				return result;
			}
			nlohmann::json ledger;
			const bool ledger_ok = LoadLegacyImportLedger(data_root, ledger);
			const std::optional<std::unordered_set<std::string>> tombstones = ChatHistorySyncService().NativeImportTombstoneKeys(data_root);
			if (!ledger_ok || !tombstones)
			{
				AppendError(result.errors, "Previous-version import or deletion records could not be read in " + uam::paths::Utf8PathString(data_root));
				return result;
			}
			nlohmann::json* completed = &ledger["completed_sources"];
			nlohmann::json* imported_chats = &ledger["imported_chat_ids"];
			nlohmann::json* imported_folders = &ledger["imported_folder_ids"];
			if (LedgerContains(*completed, source_key))
			{
				result.ok = true;
				return result;
			}

			std::error_code ec;
			for (fs::directory_iterator it(AppPaths::UamChatsRootPath(legacy_root), ec), end; !ec && it != end; it.increment(ec))
			{
				const std::string id = uam::paths::Utf8PathString(it->path().stem());
				if (!uam::paths::IsRegularFileWithExtensionNoThrow(*it, ".json") || !uam::chat_ids::IsSafeStorageChatId(id)) continue;
				++result.total_count;
				const fs::path stored = AppPaths::UamChatFilePath(data_root, id);
				if (LedgerContains(*imported_chats, id) || uam::paths::PathExistsNoThrow(stored) || uam::paths::PathExistsNoThrow(uam::io::MakeBackupPath(stored))) continue;
				LocalChatBundleImportItem item;
				item.source_id = id;
				item.imported_id = id;
				// The bounded reader never publishes caches, so the source root stays read-only.
				// ponytail: no .bak recovery for a corrupt source primary; it fails and retries each start.
				std::optional<ChatSession> chat = ChatRepository::LoadLocalChatBounded(legacy_root, id, kMaxLegacyChatBytes);
				if (chat && chat->id == id && !uam::strings::IsBlank(chat->native_session_id) &&
				    tombstones->contains(chat_identity::NativeIdentityKeyForHistoryImport(*chat)))
					continue; // Provider sessions the user already deleted here stay deleted.
				if (chat) item.imported_title = chat->title;
				item.imported = chat && chat->id == id && ChatRepository::SaveChatIfAbsent(data_root, *chat);
				if (item.imported)
				{
					imported_chats->push_back(id);
					++result.imported_count;
				}
				else
				{
					++result.failed_count;
					item.error = "Could not import previous-version chat " + id + " from " + uam::paths::Utf8PathString(legacy_root);
					result.errors.push_back(item.error);
				}
				result.items.push_back(std::move(item));
			}
			if (ec) AppendError(result.errors, "Could not read previous-version chats in " + uam::paths::Utf8PathString(legacy_root) + ": " + ec.message());

			std::vector<ChatFolder> folders = ChatFolderStore::Load(data_root);
			const std::size_t existing_folder_count = folders.size();
			for (ChatFolder& folder : ChatFolderStore::Load(legacy_root))
			{
				if (!LedgerContains(*imported_folders, folder.id) &&
				    std::none_of(folders.begin(), folders.end(), [&](const ChatFolder& existing) { return existing.id == folder.id; }))
					folders.push_back(std::move(folder));
			}
			if (folders.size() != existing_folder_count)
			{
				if (ChatFolderStore::Save(data_root, folders))
					for (std::size_t index = existing_folder_count; index < folders.size(); ++index)
					{
						imported_folders->push_back(folders[index].id);
						result.imported_folders.push_back(folders[index]);
					}
				else
					AppendError(result.errors, "Could not import previous-version chat folders.");
			}

			result.ok = result.errors.empty();
			if (result.ok) completed->push_back(source_key);
			if ((result.ok || result.imported_count > 0 || folders.size() != existing_folder_count) &&
			    !uam::io::WriteTextFile(data_root / kLegacyImportLedgerFilename, ledger.dump(2)))
			{
				result.ok = false;
				AppendError(result.errors, "Could not record the previous-version import.");
			}
			result.degraded = result.imported_count > 0 && !result.ok;
			return result;
		}
		catch (...)
		{
			result.ok = false;
			result.degraded = result.imported_count > 0;
			AppendError(result.errors, "Previous-version chat import failed; the original data was left in place.");
			return result;
		}
	}

	bool LocalChatBundleService::LegacyDataRootPending(
	    const std::filesystem::path& data_root,
	    const std::filesystem::path& legacy_root)
	{
		const std::string source_key = uam::paths::NormalizedNativePathString(legacy_root);
		if (source_key == uam::paths::NormalizedNativePathString(data_root) ||
		    !uam::paths::IsDirectoryNoThrow(AppPaths::UamChatsRootPath(legacy_root))) return false;
		nlohmann::json ledger;
		// An unreadable ledger cannot record any answer, so asking would only trap the user in the prompt.
		if (!LoadLegacyImportLedger(data_root, ledger))
		{
			uam::diagnostics::Write("[legacy-import] Not offering " + uam::paths::Utf8PathString(legacy_root) + ": " +
			                        uam::paths::Utf8PathString(data_root / kLegacyImportLedgerFilename) + " cannot be read.");
			return false;
		}
		return !LedgerContains(ledger["completed_sources"], source_key) && !LedgerContains(ledger["declined_sources"], source_key);
	}

	bool LocalChatBundleService::DeclineLegacyDataRoot(
	    const std::filesystem::path& data_root,
	    const std::filesystem::path& legacy_root)
	{
		nlohmann::json ledger;
		if (!LoadLegacyImportLedger(data_root, ledger)) return false;
		const std::string source_key = uam::paths::NormalizedNativePathString(legacy_root);
		if (LedgerContains(ledger["declined_sources"], source_key)) return true;
		ledger["declined_sources"].push_back(source_key);
		return uam::io::WriteTextFile(data_root / kLegacyImportLedgerFilename, ledger.dump(2));
	}

	std::vector<std::filesystem::path> LocalChatBundleService::PendingLegacyDataRoots(
	    const std::filesystem::path& data_root,
	    const std::filesystem::path& default_root,
	    const std::filesystem::path& executable_path)
	{
		std::vector<fs::path> candidates = {default_root};
		// v4.x fell back to %APPDATA% when %LOCALAPPDATA% was unset.
		if (const std::optional<fs::path> app_data = uam::env::GetTrimmedPath("APPDATA"))
			candidates.push_back(*app_data / "Universal Agent Manager");
		if (!executable_path.empty())
		{
			candidates.push_back(executable_path.parent_path() / "data");
			candidates.push_back(executable_path.parent_path());
		}
		std::vector<fs::path> pending;
		std::vector<std::string> seen;
		for (const fs::path& candidate : candidates)
		{
			std::string key = uam::paths::NormalizedNativePathString(candidate);
			if (candidate.empty() || std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
			seen.push_back(std::move(key));
			if (LegacyDataRootPending(data_root, candidate)) pending.push_back(candidate);
		}
		return pending;
	}
} // namespace uam
