#pragma once

#include "common/utils/hash_utils.h"

#include <string>
#include <string_view>

namespace uam::remote
{
	/// <summary>Preserves user/version/protocol and explicit acceptance namespace isolation.</summary>
	inline std::string WindowsRunnerEndpointDigest(std::string_view sid_bytes, std::string_view runner_version,
	    int protocol_version, std::string_view installation_identity, std::string_view explicit_namespace = {})
	{
		if (sid_bytes.empty() || (explicit_namespace.empty() && installation_identity.empty())) return {};
		std::uint64_t digest = hashing::kFnv1a64OffsetBasis;
		hashing::UpdateFnv1a64(digest, sid_bytes);
		hashing::UpdateFnv1a64(digest, runner_version);
		hashing::UpdateFnv1a64(digest, std::to_string(protocol_version));
		if (!explicit_namespace.empty()) hashing::UpdateFnv1a64(digest, "\nnamespace:" + std::string(explicit_namespace));
		else hashing::UpdateFnv1a64(digest, "\ninstallation:" + std::string(installation_identity));
		return hashing::Hex64Padded(digest);
	}
}
