#include "common/provider/native_session_storage_normalization.h"
#include "common/models/app_models.h"
#include <stdexcept>

// This fixture replaces only provider dispatch. Actual ChatRepository serialization,
// summary freshness, queue ownership and loading remain production implementations.
// The saved identity is already normalized; no CLI/runtime/registry may be started.
void uam::provider_storage::NormalizeLoadedNativeSessionId(ChatSession& chat)
{
	if (chat.provider_id != "gemini-cli" || chat.native_session_id != "fixture-native-session")
		throw std::runtime_error("The storage fixture attempted unqualified provider normalization.");
}
