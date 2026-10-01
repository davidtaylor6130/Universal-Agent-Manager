#pragma once


#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class MemoryLibraryStore
{
  public:
	struct Root
	{
		std::string scope_type;
		std::string folder_id;
		std::string label;
		std::filesystem::path root_path;
	};

	struct Scope
	{
		std::string scope_type;
		std::string folder_id;
		std::string label;
		std::filesystem::path root_path;
		std::vector<Root> roots;
	};

	struct Entry
	{
		std::string id;
		std::string title;
		std::string category;
		std::string scope;
		std::string confidence;
		std::string source_chat_id;
		std::string last_observed;
		int occurrence_count = 1;
		std::string preview;
		std::filesystem::path file_path;
		std::string scope_type;
		std::string folder_id;
		std::string scope_label;
		std::filesystem::path root_path;
	};

	struct Draft
	{
		std::string category;
		std::string title;
		std::string memory;
		std::string evidence;
		std::string extraction_key;
		std::string confidence;
		std::string source_chat_id;
	};

	static std::vector<Entry> ListEntries(const Scope& scope, std::string* error_out = nullptr);
	static bool CreateEntry(const Scope& scope, const Draft& draft, Entry* created_entry = nullptr, std::string* error_out = nullptr);
	static bool DeleteEntry(const Scope& scope, std::string_view entry_id, std::string* error_out = nullptr);
};
