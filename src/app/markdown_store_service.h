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
		std::filesystem::path file_path;
	};

	struct Draft
	{
		std::string title;
		std::string maker;
		std::string review;
		std::string body;
	};

	static std::filesystem::path NormalizeRoot(std::string_view root);
	static bool IsConfiguredRoot(const std::filesystem::path& root, std::string* error_out = nullptr);
	static std::vector<Entry> ListEntries(const std::filesystem::path& root, std::string* error_out = nullptr);
	static bool CreateEntry(const std::filesystem::path& root, const Draft& draft, Entry* created_entry = nullptr, std::string* error_out = nullptr);
	static bool ValidateStoreFilePath(const std::filesystem::path& root, std::string_view file_path, std::filesystem::path* normalized_path_out = nullptr, std::string* error_out = nullptr);
};
