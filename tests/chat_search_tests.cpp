#include "test_harness.h"
#include "app/chat_search_service.h"

using namespace uam_test;

UAM_TEST(ChatSearchRetainsChatWideAndSemanticsAndMessageProvenance)
{
	ChatSession chat;
	chat.id = "matched";
	chat.title = "Project alpha";
	chat.messages = {{MessageRole::User, "first beta term"}, {MessageRole::Assistant, "later gamma term"}};
	ChatSession unmatched = chat;
	unmatched.id = "unmatched";
	unmatched.messages.pop_back();
	const nlohmann::json result = uam::SearchChatMessages({chat, unmatched}, {"alpha", "beta", "gamma"});
	UAM_ASSERT_EQ(result["chatIds"], nlohmann::json::array({"matched"}));
	UAM_ASSERT_EQ(result["hits"].size(), std::size_t{2});
	UAM_ASSERT_EQ(result["hits"][0]["messageIndex"], 0);
	UAM_ASSERT_EQ(result["hits"][1]["messageIndex"], 1);
	UAM_ASSERT_EQ(result["hits"][0]["messagesDigest"], uam::StateSerializer::MessageDigest(chat));
	UAM_ASSERT_EQ(result["hits"][0]["field"], "content");
}

UAM_TEST(ChatSearchMetadataOnlyAndEmptyQueryDoNotInventMessageHits)
{
	ChatSession chat;
	chat.id = "metadata";
	chat.title = "needle project";
	chat.messages = {{MessageRole::User, "different body"}};
	const nlohmann::json result = uam::SearchChatMessages({chat}, {"needle"});
	UAM_ASSERT_EQ(result["chatIds"], nlohmann::json::array({"metadata"}));
	UAM_ASSERT(result["hits"].empty());
	const nlohmann::json empty = uam::SearchChatMessages({chat}, {});
	UAM_ASSERT(empty["chatIds"].empty());
	UAM_ASSERT(empty["hits"].empty());
}

UAM_TEST(ChatSearchBoundsHitsWithoutDroppingOtherMatchingChats)
{
	std::vector<ChatSession> chats;
	for (int index = 0; index < 40; ++index)
	{
		ChatSession chat;
		chat.id = std::to_string(index);
		chat.messages = {{MessageRole::Assistant, "needle"}};
		chats.push_back(chat);
	}
	const nlohmann::json result = uam::SearchChatMessages(chats, {"needle"});
	UAM_ASSERT_EQ(result["chatIds"].size(), std::size_t{40});
	UAM_ASSERT_EQ(result["hits"].size(), std::size_t{32});
}

UAM_TEST(ChatSearchExcerptsPreserveUtf8AndThoughtProvenance)
{
	ChatSession chat;
	chat.id = "utf8";
	Message message;
	message.role = MessageRole::Assistant;
	for (int index = 0; index < 180; ++index) message.thoughts += "界";
	message.thoughts += "needle";
	for (int index = 0; index < 300; ++index) message.thoughts += "界";
	message.plan_summary = "needle plan";
	chat.messages.push_back(message);
	const nlohmann::json result = uam::SearchChatMessages({chat}, {"needle"});
	UAM_ASSERT_EQ(result["hits"].size(), std::size_t{1});
	UAM_ASSERT_EQ(result["hits"][0]["field"], "thoughts");
	const std::string excerpt = result["hits"][0]["excerpt"];
	UAM_ASSERT(excerpt.find("needle") != std::string::npos);
	UAM_ASSERT(excerpt.starts_with("界"));
	UAM_ASSERT(excerpt.ends_with("界"));
	std::size_t characters = 0;
	for (const unsigned char byte : excerpt) if ((byte & 0xc0) != 0x80) ++characters;
	UAM_ASSERT_EQ(characters, std::size_t{240});
	// Strict JSON UTF-8 serialization also checks that no partial character escaped.
	UAM_ASSERT(!result.dump().empty());
}

UAM_TEST(ChatSearchCursorRejectsSameSizeEditsAndMissingTargets)
{
	ChatSession chat;
	chat.id = "cursor";
	chat.messages = {{MessageRole::Assistant, "before"}};
	chat.messages_loaded = true;
	const std::string digest = uam::StateSerializer::MessageDigest(chat);
	UAM_ASSERT(uam::ChatSearchTargetMatches(chat, digest, 0));
	UAM_ASSERT(!uam::ChatSearchTargetMatches(chat, digest, 1));
	chat.messages.front().content = "after!";
	UAM_ASSERT(!uam::ChatSearchTargetMatches(chat, digest, 0));
	UAM_ASSERT(!uam::ChatSearchTargetMatches(chat, "", 0));
	chat.messages_loaded = false;
	UAM_ASSERT(!uam::ChatSearchTargetMatches(chat, uam::StateSerializer::MessageDigest(chat), 0));
}
