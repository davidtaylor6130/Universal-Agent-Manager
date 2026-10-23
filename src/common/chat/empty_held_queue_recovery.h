#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct ChatSession;
namespace uam::platform { class PublishedFileChangeWitness; }
namespace uam::queue_recovery
{
	struct EmptyHeldQueueAckProof;
	struct EmptyHeldQueueAckReceipt
	{
		bool saved = false, uncertain = false;
		std::string revision;
		std::shared_ptr<const platform::PublishedFileChangeWitness> journal;
	};
	// Worker-only DATA preparation/publication; no input or stopped-launch permission.
	std::shared_ptr<const EmptyHeldQueueAckProof> PrepareEmptyHeldQueueAck(
	    const std::filesystem::path& root, ChatSession metadata, const std::function<bool()>& owned);
	bool PreparedEmptyHeldQueueAckMatches(const EmptyHeldQueueAckProof&, const std::filesystem::path&, const ChatSession&);
	EmptyHeldQueueAckReceipt PublishPreparedEmptyHeldQueueAck(
	    const std::shared_ptr<const EmptyHeldQueueAckProof>&, const std::function<bool()>& owned);
	bool PublishedEmptyHeldQueueAckMatches(const EmptyHeldQueueAckProof&, const EmptyHeldQueueAckReceipt&,
	    const std::filesystem::path&, const ChatSession&);
	bool AdoptEmptyHeldQueueAckRevision(const EmptyHeldQueueAckProof&, const EmptyHeldQueueAckReceipt&,
	    const std::filesystem::path&, ChatSession&);
}
