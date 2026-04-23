#ifndef UAM_APP_MEMORY_LIBRARY_SERVICE_H
#define UAM_APP_MEMORY_LIBRARY_SERVICE_H

#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

class MemoryLibraryService
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
		std::string confidence;
		std::string source_chat_id;
	};

	static bool ResolveScope(const uam::AppState& app, const std::string& scope_type, const std::string& folder_id, Scope& out_scope, std::string* error_out = nullptr);
	static std::vector<Entry> ListEntries(const Scope& scope, std::string* error_out = nullptr);
	static bool CreateEntry(const Scope& scope, const Draft& draft, Entry* created_entry = nullptr, std::string* error_out = nullptr);
	static bool DeleteEntry(const Scope& scope, const std::string& entry_id, std::string* error_out = nullptr);
};

#endif // UAM_APP_MEMORY_LIBRARY_SERVICE_H
