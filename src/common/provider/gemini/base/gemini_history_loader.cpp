#include "common/provider/gemini/base/gemini_history_loader.h"

#include "common/chat/gemini_json_history_store.h"

std::vector<ChatSession> LoadGeminiJsonHistoryForRuntime(const std::filesystem::path& chats_dir, const ProviderProfile& profile, const ProviderRuntimeHistoryLoadOptions& options)
{
	GeminiJsonHistoryStoreOptions native_options;
	native_options.max_file_bytes = options.native_max_file_bytes;
	native_options.max_messages = options.native_max_messages;
	return GeminiJsonHistoryStore::Load(chats_dir, profile, native_options);
}
