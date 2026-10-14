#pragma once

#include "app/provider_worker_command.h"
#include <functional>

namespace uam::opencode_import
{
	using Spool = std::shared_ptr<const std::filesystem::path>;
	/// <summary>Owns a private complete export; failed workers and checked writes discard it.</summary>
	Spool Export(ProviderWorkerInvocation invocation, const std::filesystem::path& workspace, std::stop_token stop, std::string& error);
	/// <summary>Maps one complete message at a time; memory follows the largest message, without an aggregate quota.</summary>
	bool Read(const std::filesystem::path& source, const ChatSession& identity,
	    const std::function<bool(const Message&)>& emit, std::stop_token stop, std::string& error);
}
