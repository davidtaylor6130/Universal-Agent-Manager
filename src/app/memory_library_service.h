#pragma once
#include "common/state/app_state.h"
#include "common/memory/memory_library_store.h"

class MemoryLibraryService : public MemoryLibraryStore
{
public:
	struct Scope : MemoryLibraryStore::Scope
	{
		std::optional<ExecutionHost> remote_host;
		std::string remote_workspace;
		std::vector<Scope> remote_scopes;
	};
	static bool ResolveScope(const uam::AppState& app, std::string_view scope_type, std::string_view folder_id, Scope& out_scope, std::string* error_out = nullptr);
	static std::vector<Entry> ListEntries(const Scope& scope, std::string* error_out = nullptr);
	static bool CreateEntry(const Scope& scope, const Draft& draft, Entry* created_entry = nullptr, std::string* error_out = nullptr);
	static bool DeleteEntry(const Scope& scope, std::string_view entry_id, std::string* error_out = nullptr);
};
