#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class MarkdownStoreService
{
  public:
	struct Entry
	{
		std::string id;
		std::string title;
		std::string maker;
		std::string review;
		std::string date_created;
		std::string date_updated;
		std::string preview;
		std::string body;
		bool favorite = false;
		std::string source_provider;
		std::string source_path;
		std::string command_name;
		std::string group;
		std::filesystem::path file_path;
	};

	struct Draft
	{
		std::string title;
		std::string maker;
		std::string review;
		std::string body;
		std::string group;
	};

	struct ImportSource
	{
		std::string provider;
		std::filesystem::path path;
	};

	struct ImportCandidate
	{
		std::string id;
		std::string title;
		std::string maker;
		std::string review;
		std::string preview;
		std::string source_provider;
		std::filesystem::path source_path;
		bool supported = false;
		std::string validation_error;
		std::filesystem::path collision_path;
	};

	enum class ImportConflictAction
	{
		Skip,
		Replace,
		Separate,
	};

	struct ImportRequest
	{
		std::string source_provider;
		std::filesystem::path source_path;
		ImportConflictAction conflict_action = ImportConflictAction::Skip;
	};

	struct ImportResult
	{
		std::filesystem::path source_path;
		std::string status;
		std::string message;
		Entry entry;
	};

	static std::filesystem::path NormalizeRoot(std::string_view root);
	static bool IsConfiguredRoot(const std::filesystem::path& root, std::string* error_out = nullptr);
	static std::vector<Entry> ListEntries(const std::filesystem::path& root, std::string* error_out = nullptr);
	static bool CreateEntry(const std::filesystem::path& root, const Draft& draft, Entry* created_entry = nullptr, std::string* error_out = nullptr);
	static bool UpdateEntry(const std::filesystem::path& root, std::string_view file_path, const Draft& draft, Entry* updated_entry = nullptr, std::string* error_out = nullptr);
	static bool SetFavorite(const std::filesystem::path& root, std::string_view file_path, bool favorite, Entry* updated_entry = nullptr, std::string* error_out = nullptr);
	static std::vector<ImportSource> DefaultImportSources();
	static std::vector<ImportCandidate> PreviewImports(const std::filesystem::path& root, const std::vector<ImportSource>& sources, std::string* error_out = nullptr);
	static std::vector<ImportResult> ImportEntries(const std::filesystem::path& root, const std::vector<ImportRequest>& requests, std::string* error_out = nullptr);
	static bool ValidateStoreFilePath(const std::filesystem::path& root, std::string_view file_path, std::filesystem::path* normalized_path_out = nullptr, std::string* error_out = nullptr);
};
