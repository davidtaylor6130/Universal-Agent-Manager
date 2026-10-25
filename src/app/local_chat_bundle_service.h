#pragma once

#include "common/models/app_models.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	struct LocalChatBundleExportResult
	{
		bool ok = false;
		bool degraded = false;
		std::filesystem::path bundle_path;
		std::size_t total_count = 0;
		std::size_t exported_count = 0;
		std::vector<std::string> warnings;
		std::vector<std::string> errors;
	};

	struct LocalChatBundleImportItem
	{
		std::string source_id;
		std::string imported_id;
		std::string imported_title;
		bool renamed = false;
		bool imported = false;
		std::string error;
	};

	struct LocalChatBundleImportResult
	{
		bool ok = false;
		bool degraded = false;
		std::filesystem::path bundle_path;
		std::size_t total_count = 0;
		std::size_t imported_count = 0;
		std::size_t failed_count = 0;
		std::size_t renamed_count = 0;
		std::vector<LocalChatBundleImportItem> items;
		// Folders added by ImportLegacyDataRoot, for merging into the live app.
		std::vector<ChatFolder> imported_folders;
		std::vector<std::string> warnings;
		std::vector<std::string> errors;
	};

	class LocalChatBundleService
	{
	  public:
		static constexpr std::string_view kSchema = "uam.local-chat-bundle";
		static constexpr int kVersion = 2;

		static LocalChatBundleExportResult Export(
		    const std::filesystem::path& data_root,
		    const std::filesystem::path& bundle_path);
		static LocalChatBundleImportResult Import(
		    const std::filesystem::path& data_root,
		    const std::filesystem::path& bundle_path);
		// Copies chats and folders from a previous version's data root into data_root,
		// keeping their ids and leaving the source untouched. Existing ids are skipped,
		// and a fully imported source is recorded so later runs never import it again.
		// Callers ask the user first (see LegacyDataRootPending).
		static LocalChatBundleImportResult ImportLegacyDataRoot(
		    const std::filesystem::path& data_root,
		    const std::filesystem::path& legacy_root);
		// True when legacy_root holds earlier-release chats the user has neither copied nor declined.
		static bool LegacyDataRootPending(
		    const std::filesystem::path& data_root,
		    const std::filesystem::path& legacy_root);
		// Records the "Brand new" choice so legacy_root is never offered again.
		static bool DeclineLegacyDataRoot(
		    const std::filesystem::path& data_root,
		    const std::filesystem::path& legacy_root);
	};
} // namespace uam
