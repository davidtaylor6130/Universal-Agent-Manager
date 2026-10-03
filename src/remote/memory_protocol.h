#pragma once

#include "common/memory/memory_library_store.h"
#include "common/paths/path_utils.h"
#include <nlohmann/json.hpp>

namespace uam::remote
{
	inline nlohmann::json EncodeMemoryEntry(const MemoryLibraryStore::Entry& entry)
	{
		return {{"id", entry.id}, {"title", entry.title}, {"category", entry.category}, {"scope", entry.scope},
		    {"confidence", entry.confidence}, {"sourceChatId", entry.source_chat_id}, {"lastObserved", entry.last_observed},
		    {"occurrenceCount", entry.occurrence_count}, {"preview", entry.preview}};
	}

	inline bool DecodeMemoryEntry(const nlohmann::json& value, MemoryLibraryStore::Entry& entry)
	{
		if (!value.is_object()) return false;
		for (const char* field : {"id", "title", "category", "scope", "confidence", "sourceChatId", "lastObserved", "preview"})
			if (!value.contains(field) || !value[field].is_string() || value[field].get_ref<const std::string&>().size() > 4096) return false;
		if (!value.contains("occurrenceCount") || !value["occurrenceCount"].is_number_integer()) return false;
		entry.id = value["id"].get<std::string>();
		entry.title = value["title"].get<std::string>();
		entry.category = value["category"].get<std::string>();
		entry.scope = value["scope"].get<std::string>();
		entry.confidence = value["confidence"].get<std::string>();
		entry.source_chat_id = value["sourceChatId"].get<std::string>();
		entry.last_observed = value["lastObserved"].get<std::string>();
		entry.preview = value["preview"].get<std::string>();
		entry.occurrence_count = value["occurrenceCount"].get<int>();
		return entry.occurrence_count > 0;
	}
}
