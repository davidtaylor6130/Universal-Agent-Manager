#pragma once

#include "common/config/execution_host_config.h"
#include "common/models/app_models.h"
#include "common/provider/provider_ids.h"

namespace uam::copilot
{

/// A profile migration cannot change the protocol of an already running helper-owned CLI.
inline bool NeedsNativeWireMigration(const ChatSession& chat, std::string_view effective_provider)
{
	return provider_ids::CanonicalCliProviderLookupId(effective_provider) == provider_ids::kCopilotCli &&
	    !chat.execution_host_id.empty() && chat.execution_host_id != execution_hosts::kLocalHostId &&
	    chat.remote_process_exists && chat.remote_structured_protocol != "copilot-sdk";
}

}
