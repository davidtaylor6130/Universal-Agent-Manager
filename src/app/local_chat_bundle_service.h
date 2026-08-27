#pragma once

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
		std::vector<std::string> warnings;
		std::vector<std::string> errors;
	};

	class LocalChatBundleService
	{
	  public:
		static constexpr std::string_view kSchema = "uam.local-chat-bundle";
		static constexpr int kVersion = 1;

		static LocalChatBundleExportResult Export(
		    const std::filesystem::path& data_root,
		    const std::filesystem::path& bundle_path);
		static LocalChatBundleImportResult Import(
		    const std::filesystem::path& data_root,
		    const std::filesystem::path& bundle_path);
	};
} // namespace uam
