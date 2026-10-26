#pragma once

struct ChatSession;

namespace uam::provider_storage
{
	/// Provider-owned identity normalization after decoding saved storage; no persistence authority.
	void NormalizeLoadedNativeSessionId(ChatSession& chat);
}
