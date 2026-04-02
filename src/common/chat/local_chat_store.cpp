#include "local_chat_store.h"

#include "chat_repository.h"

std::vector<ChatSession> LocalChatStore::Load(const std::filesystem::path& data_root)
{
	return ChatRepository::LoadLocalChats(data_root);
}

bool LocalChatStore::Save(const std::filesystem::path& data_root, const ChatSession& chat)
{
	return ChatRepository::SaveChat(data_root, chat);
}
