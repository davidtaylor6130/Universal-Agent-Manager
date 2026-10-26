#include "common/provider/native_session_storage_normalization.h"
#include "common/provider/provider_runtime.h"

void uam::provider_storage::NormalizeLoadedNativeSessionId(ChatSession& chat)
{
	ProviderRuntimeRegistry::ResolveById(chat.provider_id).NormalizeLoadedNativeSessionId(chat);
}
