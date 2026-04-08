#include "runtime_orchestration_services.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_local_service.h"
#include "app/template_runtime_service.h"

#include "common/paths/app_paths.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/platform/platform_services.h"
#if UAM_ENABLE_ANY_GEMINI_PROVIDER
#include "common/provider/gemini/base/gemini_history_loader.h"
#endif
#include "common/provider/runtime/provider_build_config.h"
#include "common/rag/rag_app_helpers.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/json_runtime.h"
#include "common/runtime/terminal_common.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using uam::AppState;

namespace
{
	constexpr const char* kPromptBootstrapPath = "@.gemini/gemini.md";
	constexpr const char* kDefaultNativeHistoryProviderId = provider_build_config::DefaultNativeHistoryProviderId();

	void ResetAsyncNativeChatLoadTask(uam::platform::AsyncNativeChatLoadTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker.reset();
		}

		task.running = false;
		task.provider_id_snapshot.clear();
		task.chats_dir_snapshot.clear();
		task.state.reset();
	}

	const ProviderProfile& DefaultNativeHistoryProvider(const AppState& app)
	{
		if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, kDefaultNativeHistoryProviderId); profile != nullptr)
		{
			return *profile;
		}

		for (const ProviderProfile& profile : ProviderProfileStore::BuiltInProfiles())
		{
			if (profile.id == kDefaultNativeHistoryProviderId)
			{
				static const ProviderProfile result = profile;
				return result;
			}
		}

		if (!app.provider_profiles.empty())
		{
			static const ProviderProfile result = []()
			{
				ProviderProfile profile;
				profile.id = "fallback";
				profile.title = "Fallback";
				return profile;
			}();
			(void)result;
			return app.provider_profiles.front();
		}

		static const ProviderProfile fallback = []()
		{
			ProviderProfile profile;
			profile.id = provider_build_config::FirstEnabledProviderId();
			profile.title = profile.id;
			return profile;
		}();
		return fallback;
	}

	std::vector<fs::path> CollectWorkspaceRootsForNativeHistory(const AppState& app)
	{
		std::vector<fs::path> roots;
		std::unordered_set<std::string> seen;

		for (const ChatFolder& folder : app.folders)
		{
			fs::path root = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(folder.directory);

			if (root.empty())
			{
				continue;
			}

			std::error_code ec;
			const fs::path absolute_root = fs::absolute(root, ec);
			const fs::path normalized_root = (ec ? root : absolute_root).lexically_normal();
			const std::string key = normalized_root.string();

			if (key.empty() || seen.find(key) != seen.end())
			{
				continue;
			}

			seen.insert(key);
			roots.push_back(normalized_root);
		}

		if (roots.empty())
		{
			std::error_code ec;
			const fs::path current_root = fs::current_path(ec);

			if (!ec)
			{
				roots.push_back(current_root.lexically_normal());
			}
		}

		return roots;
	}
} // namespace

void ProviderRequestService::StartSelectedChatRequest(uam::AppState& p_app) const
{
	ChatSession* lp_chat = ChatDomainService().SelectedChat(p_app);

	if (lp_chat == nullptr)
	{
		p_app.status_line = "Select or create a chat first.";
		return;
	}

	const std::string l_promptText = Trim(p_app.composer_text);

	if (QueuePromptForChat(p_app, *lp_chat, l_promptText, false))
	{
		p_app.composer_text.clear();
	}
}

void ChatHistorySyncService::RefreshChatHistory(uam::AppState& p_app) const
{
	const ChatSession* lcp_selected = ChatDomainService().SelectedChat(p_app);
	const std::string l_selectedId = (lcp_selected != nullptr) ? lcp_selected->id : "";
	SyncChatsFromNative(p_app, l_selectedId, true);
	p_app.status_line = "Chat history refreshed.";
}

void ChatHistorySyncService::SaveChatWithStatus(uam::AppState& p_app, const ChatSession& p_chat, const std::string& p_success, const std::string& p_failure) const
{
	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);

	if (ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat))
	{
		p_app.status_line = p_success;
	}
	else
	{
		p_app.status_line = p_failure;
	}
}

std::vector<ChatSession> ChatHistorySyncService::LoadNativeSessionChats(const fs::path& p_chatsDir, const ProviderProfile& p_provider, std::stop_token p_stopToken) const
{
	ProviderRuntimeHistoryLoadOptions l_options;
	l_options.native_max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	l_options.native_max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
	if (ProviderRuntime::SupportsGeminiJsonHistory(p_provider))
	{
#if UAM_ENABLE_ANY_GEMINI_PROVIDER
		return ChatDomainService().DeduplicateChatsById(LoadGeminiJsonHistoryForRuntime(p_chatsDir, p_provider, l_options, p_stopToken));
#else
		(void)p_chatsDir;
		(void)p_provider;
		(void)p_stopToken;
		return {};
#endif
	}

	return ChatDomainService().DeduplicateChatsById(ProviderRuntime::LoadHistory(p_provider, fs::path{}, p_chatsDir, l_options));
}

std::optional<fs::path> ChatHistorySyncService::ResolveNativeHistoryChatsDirForWorkspace(const fs::path& p_workspaceRoot) const
{
	if (p_workspaceRoot.empty())
	{
		return std::nullopt;
	}

	const auto l_tmpDir = AppPaths::ResolveGeminiProjectTmpDir(p_workspaceRoot);

	if (l_tmpDir.has_value())
	{
		const fs::path l_chatsDir = l_tmpDir.value() / "chats";
		std::error_code l_ec;
		fs::create_directories(l_chatsDir, l_ec);
		return l_ec ? std::nullopt : std::optional<fs::path>(l_chatsDir);
	}

	return std::nullopt;
}

fs::path ChatHistorySyncService::ResolveNativeHistoryChatsDirForChat(const AppState& p_app, const ChatSession& p_chat) const
{
	if (!ProviderResolutionService().ChatUsesNativeOverlayHistory(p_app, p_chat))
	{
		return {};
	}

	const fs::path l_workspaceRoot = ResolveWorkspaceRootPath(p_app, p_chat);
	const auto l_chatsDir = ResolveNativeHistoryChatsDirForWorkspace(l_workspaceRoot);
	return l_chatsDir.has_value() ? l_chatsDir.value() : fs::path{};
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportAllNativeChatsToLocal(AppState& p_app, const bool p_delete_native_after_import, const std::string& p_targetChatId) const
{
	ImportResult result;
	const ProviderProfile& l_nativeProvider = DefaultNativeHistoryProvider(p_app);

	std::vector<ChatSession> l_localChats = ChatRepository::LoadLocalChats(p_app.data_root);
	std::unordered_set<std::string> l_existingIds;
	l_existingIds.reserve(l_localChats.size());
	for (const ChatSession& chat : l_localChats)
	{
		l_existingIds.insert(chat.id);
	}

	for (const fs::path& l_workspaceRoot : CollectWorkspaceRootsForNativeHistory(p_app))
	{
		const auto l_chatsDir = ResolveNativeHistoryChatsDirForWorkspace(l_workspaceRoot);
		if (!l_chatsDir.has_value())
		{
			continue;
		}

		std::vector<ChatSession> l_nativeChats = LoadNativeSessionChats(l_chatsDir.value(), l_nativeProvider);

		for (ChatSession& l_nativeChat : l_nativeChats)
		{
			if (!p_targetChatId.empty() && l_nativeChat.id != p_targetChatId && l_nativeChat.native_session_id != p_targetChatId)
			{
				continue;
			}

			++result.total_count;

			if (p_targetChatId.empty() && l_existingIds.contains(l_nativeChat.id))
			{
				continue;
			}

			for (const ChatFolder& folder : p_app.folders)
			{
				if (FolderDirectoryMatches(folder.directory, l_workspaceRoot))
				{
					l_nativeChat.folder_id = folder.id;
					l_nativeChat.workspace_directory = folder.directory;
					break;
				}
			}

			if (ChatRepository::SaveChat(p_app.data_root, l_nativeChat))
			{
				++result.imported_count;
				l_existingIds.insert(l_nativeChat.id);

				if (p_delete_native_after_import && !l_nativeChat.native_session_id.empty())
				{
					std::error_code l_ec;
					const fs::path l_nativeFile = l_chatsDir.value() / (l_nativeChat.native_session_id + ".json");
					fs::remove(l_nativeFile, l_ec);
				}
			}
		}
	}

	return result;
}

void ChatHistorySyncService::LoadSidebarChats(AppState& p_app) const
{
	p_app.chats = ChatRepository::LoadLocalChats(p_app.data_root);
	p_app.chats = ChatDomainService().DeduplicateChatsById(std::move(p_app.chats));
	ChatBranching::Normalize(p_app.chats);
	ChatDomainService().NormalizeChatFolderAssignments(p_app);
}

void ChatHistorySyncService::LoadSidebarChatsByDiscovery(AppState& p_app) const
{
	p_app.chats = ChatRepository::LoadLocalChats(p_app.data_root);
	p_app.chats = ChatDomainService().DeduplicateChatsById(std::move(p_app.chats));
	ChatBranching::Normalize(p_app.chats);
	ChatDomainService().NormalizeChatFolderAssignments(p_app);
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportAllNativeChatsByDiscovery(AppState& p_app, const bool p_delete_native_after_import, const std::string& p_targetChatId) const
{
	ImportResult result;
	const ProviderProfile& l_nativeProvider = DefaultNativeHistoryProvider(p_app);

	const ProviderDiscoveryResult l_discovery = ProviderRuntime::DiscoverChatSources(l_nativeProvider);
	if (!l_discovery.error.empty())
	{
		return result;
	}

	std::vector<ChatSession> l_localChats = ChatRepository::LoadLocalChats(p_app.data_root);
	std::unordered_set<std::string> l_existingIds;
	l_existingIds.reserve(l_localChats.size());
	for (const ChatSession& chat : l_localChats)
	{
		l_existingIds.insert(chat.id);
	}

	for (const ProviderChatSource& l_source : l_discovery.sources)
	{
		ChatFolder* lp_folder = nullptr;
		for (ChatFolder& folder : p_app.folders)
		{
			if (FolderDirectoryMatches(folder.directory, l_source.folder_directory))
			{
				lp_folder = &folder;
				break;
			}
		}

		if (lp_folder == nullptr)
		{
			ChatFolder new_folder;
			new_folder.id = "folder_" + std::to_string(p_app.folders.size()) + "_" + l_source.folder_title;
			new_folder.title = l_source.folder_title;
			new_folder.directory = l_source.folder_directory;
			new_folder.collapsed = false;
			p_app.folders.push_back(std::move(new_folder));
			lp_folder = &p_app.folders.back();
		}

		std::vector<ChatSession> l_nativeChats = LoadNativeSessionChats(l_source.chats_dir, l_nativeProvider);

		for (ChatSession& l_nativeChat : l_nativeChats)
		{
			if (!p_targetChatId.empty() && l_nativeChat.id != p_targetChatId && l_nativeChat.native_session_id != p_targetChatId)
			{
				continue;
			}

			++result.total_count;

			if (p_targetChatId.empty() && l_existingIds.contains(l_nativeChat.id))
			{
				continue;
			}

			l_nativeChat.folder_id = lp_folder->id;
			l_nativeChat.workspace_directory = l_source.folder_directory;

			if (ChatRepository::SaveChat(p_app.data_root, l_nativeChat))
			{
				++result.imported_count;
				l_existingIds.insert(l_nativeChat.id);

				if (p_delete_native_after_import && !l_nativeChat.native_session_id.empty())
				{
					std::error_code l_ec;
					const fs::path l_nativeFile = l_source.chats_dir / (l_nativeChat.native_session_id + ".json");
					fs::remove(l_nativeFile, l_ec);
				}
			}
		}
	}

	ChatFolderStore::Save(p_app.data_root, p_app.folders);

	return result;
}

bool ChatHistorySyncService::StartAsyncNativeChatLoad(AppState& app, const ProviderProfile& p_provider, const fs::path& p_chatsDir) const
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!ProviderRuntime::UsesNativeOverlayHistory(p_provider) || p_chatsDir.empty() || app.native_chat_load_task.running)
	{
		return false;
	}

	ResetAsyncNativeChatLoadTask(app.native_chat_load_task);
	app.native_chat_load_task.running = true;
	app.native_chat_load_task.provider_id_snapshot = p_provider.id;
	app.native_chat_load_task.chats_dir_snapshot = p_chatsDir.string();
	app.native_chat_load_task.state = std::make_shared<uam::platform::AsyncNativeChatLoadTask::State>();
	std::shared_ptr<uam::platform::AsyncNativeChatLoadTask::State> state = app.native_chat_load_task.state;
	const fs::path chats_dir = p_chatsDir;
	const ProviderProfile provider = p_provider;
	app.native_chat_load_task.worker = std::make_unique<std::jthread>(
	    [this, chats_dir, provider, state](std::stop_token stop_token)
	    {
		    try
		    {
			    state->chats = LoadNativeSessionChats(chats_dir, provider, stop_token);
		    }
		    catch (const std::exception& ex)
		    {
			    state->error = ex.what();
		    }
		    catch (...)
		    {
			    state->error = "Unknown native chat load failure.";
		    }

		    state->completed.store(true, std::memory_order_release);
	    });
	return true;
}

bool ChatHistorySyncService::TryConsumeAsyncNativeChatLoad(AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out) const
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!app.native_chat_load_task.running)
	{
		return false;
	}

	if (app.native_chat_load_task.state == nullptr)
	{
		ResetAsyncNativeChatLoadTask(app.native_chat_load_task);
		chats_out.clear();
		error_out.clear();
		return true;
	}

	if (!app.native_chat_load_task.state->completed.load(std::memory_order_acquire))
	{
		return false;
	}

	chats_out = app.native_chat_load_task.state->chats;
	error_out = app.native_chat_load_task.state->error;
	ResetAsyncNativeChatLoadTask(app.native_chat_load_task);
	return true;
}

std::vector<std::string> ChatHistorySyncService::SessionIdsFromChats(const std::vector<ChatSession>& p_chats) const
{
	std::vector<std::string> l_ids;
	l_ids.reserve(p_chats.size());

	for (const ChatSession& l_chat : p_chats)
	{
		if (!l_chat.native_session_id.empty())
		{
			l_ids.push_back(l_chat.native_session_id);
		}
	}

	return l_ids;
}

std::optional<fs::path> ChatHistorySyncService::FindNativeSessionFilePath(const fs::path& p_chatsDir, const std::string& p_sessionId) const
{
	if (p_sessionId.empty() || p_chatsDir.empty() || !fs::exists(p_chatsDir))
	{
		return std::nullopt;
	}

	std::error_code l_ec;
	std::string searchPart = p_sessionId;
	if (searchPart.length() > 8)
	{
		searchPart = searchPart.substr(0, 8);
	}

	for (const auto& l_item : fs::directory_iterator(p_chatsDir, l_ec))
	{
		if (l_ec || !l_item.is_regular_file() || l_item.path().extension() != ".json")
		{
			continue;
		}

		const std::string l_filename = l_item.path().filename().string();
		if (l_filename.find(searchPart) != std::string::npos)
		{
			return l_item.path();
		}
	}

	return std::nullopt;
}

bool ChatHistorySyncService::DeleteNativeSessionFileForChat(const AppState& p_app, const ChatSession& p_chat, std::error_code* p_errorOut) const
{
	if (p_errorOut != nullptr)
	{
		p_errorOut->clear();
	}

	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);

	if (!ProviderRuntime::UsesNativeOverlayHistory(l_provider) || p_chat.native_session_id.empty())
	{
		return false;
	}

	const fs::path l_chatsDir = ResolveNativeHistoryChatsDirForChat(p_app, p_chat);
	const auto l_sessionFile = FindNativeSessionFilePath(l_chatsDir, p_chat.native_session_id);

	if (!l_sessionFile.has_value())
	{
		return false;
	}

	std::error_code l_ec;
	const bool l_removed = fs::remove(l_sessionFile.value(), l_ec);

	if (p_errorOut != nullptr)
	{
		*p_errorOut = l_ec;
	}

	return l_removed && !l_ec;
}

bool ChatHistorySyncService::PersistLocalDraftNativeSessionLink(const AppState& p_app, ChatSession& p_localChat, const std::string& p_nativeSessionId) const
{
	const std::string l_sessionId = Trim(p_nativeSessionId);

	if (l_sessionId.empty() || !NativeSessionLinkService().IsLocalDraftChatId(p_localChat.id))
	{
		return false;
	}

	p_localChat.native_session_id = l_sessionId;
	ChatRepository::SaveChat(p_app.data_root, p_localChat);
	return true;
}

bool ChatHistorySyncService::MoveChatToFolder(AppState& p_app, ChatSession& p_chat, const std::string& p_newFolderId) const
{
	if (p_newFolderId.empty())
	{
		return false;
	}

	const ChatFolder* l_newFolder = ChatDomainService().FindFolderById(p_app, p_newFolderId);
	if (l_newFolder == nullptr)
	{
		return false;
	}

	const std::string l_oldWorkspace = p_chat.workspace_directory;
	const std::string l_oldFolderId = p_chat.folder_id;
	p_app.move_chat_original_folder_id = l_oldFolderId;
	p_app.move_chat_original_workspace = l_oldWorkspace;
	p_app.move_chat_target_folder_id = p_newFolderId;
	p_app.move_chat_target_workspace = l_newFolder->directory;

	p_chat.folder_id = p_newFolderId;
	p_chat.workspace_directory = l_newFolder->directory;
	p_chat.updated_at = TimestampNow();

	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);
	const std::string l_sessionId = p_chat.native_session_id;

	fs::path normalizedOld = fs::weakly_canonical(l_oldWorkspace);
	fs::path normalizedNew = fs::weakly_canonical(l_newFolder->directory);
	bool l_workspacesDifferent = !l_sessionId.empty() && !l_oldWorkspace.empty() && normalizedOld != normalizedNew;
	bool l_portingSucceeded = true;

	if (l_workspacesDifferent)
	{
		StopAndEraseCliTerminalForChat(p_app, p_chat.id);

		const auto l_oldChatsDir = ResolveNativeHistoryChatsDirForWorkspace(normalizedOld);
		if (l_oldChatsDir.has_value() && !l_sessionId.empty())
		{
			const auto l_sessionFile = FindNativeSessionFilePath(l_oldChatsDir.value(), l_sessionId);
			if (l_sessionFile.has_value())
			{
#if UAM_ENABLE_ANY_GEMINI_PROVIDER
				GeminiJsonHistoryStoreOptions l_opts;
				l_opts.max_messages = 0;
				l_opts.max_file_bytes = 0;
				const auto l_parsed = GeminiJsonHistoryStore::ParseFile(l_sessionFile.value(), l_provider, l_opts);
				if (l_parsed.has_value() && !l_parsed->messages.empty())
				{
					p_chat.messages = l_parsed->messages;
					if (!l_parsed->updated_at.empty())
					{
						p_chat.updated_at = l_parsed->updated_at;
					}
				}
#endif
			}
		}

		bool rebuildResult = ProviderRuntime::RebuildNativeSessionFile(l_provider, p_chat, l_newFolder->directory);
		if (!rebuildResult)
		{
			l_portingSucceeded = false;
			p_app.move_chat_pending_id = p_chat.id;
			p_app.move_chat_show_missing_session_warning = true;
		}
	}
	else
	{
		StopAndEraseCliTerminalForChat(p_app, p_chat.id);
	}

	return ChatRepository::SaveChat(p_app.data_root, p_chat) && l_portingSucceeded;
}

std::string ChatHistorySyncService::ResolveResumeSessionIdForChat(const AppState& p_app, const ChatSession& p_chat) const
{
	if (!ProviderResolutionService().ChatUsesNativeOverlayHistory(p_app, p_chat))
	{
		return "";
	}

	fs::path l_chatsDir = ResolveNativeHistoryChatsDirForChat(p_app, p_chat);

	if (l_chatsDir.empty())
	{
		return "";
	}

	const auto l_findSessionFile = [&](const std::string& p_candidateId, const fs::path& p_searchDir) -> std::string
	{
		if (p_candidateId.empty() || p_searchDir.empty())
		{
			return "";
		}

		std::error_code l_ec;
		if (fs::exists(p_searchDir / (p_candidateId + ".json"), l_ec) && !l_ec)
		{
			return p_candidateId;
		}

		for (const auto& l_entry : fs::directory_iterator(p_searchDir, l_ec))
		{
			if (!l_entry.is_regular_file(l_ec) || l_ec)
			{
				continue;
			}

			const std::string l_filename = l_entry.path().filename().string();
			if (l_filename.find(p_candidateId) != std::string::npos)
			{
				return p_candidateId;
			}
		}

		return "";
	};

	const auto l_tryPortSession = [&](const std::string& p_candidateId, const fs::path& p_sourceDir) -> bool
	{
		if (p_candidateId.empty() || p_sourceDir.empty() || l_chatsDir.empty())
		{
			return false;
		}

		if (p_sourceDir == l_chatsDir)
		{
			return false;
		}

		std::error_code l_ec;
		fs::create_directories(l_chatsDir, l_ec);
		if (l_ec)
		{
			return false;
		}

		bool l_copiedAny = false;
		for (const auto& l_entry : fs::directory_iterator(p_sourceDir, l_ec))
		{
			if (l_ec || !l_entry.is_regular_file())
			{
				continue;
			}

			const std::string l_filename = l_entry.path().filename().string();
			if (l_filename.find(p_candidateId) != std::string::npos)
			{
				const fs::path l_dest = l_chatsDir / l_filename;
				fs::copy_file(l_entry.path(), l_dest, fs::copy_options::overwrite_existing, l_ec);
				if (!l_ec)
				{
					l_copiedAny = true;
				}
			}
		}

		return l_copiedAny;
	};

	std::string l_candidateId;

	if (!p_chat.native_session_id.empty())
	{
		l_candidateId = p_chat.native_session_id;
	}
	else if (!p_chat.messages.empty() && !p_chat.id.empty() && !NativeSessionLinkService().IsLocalDraftChatId(p_chat.id))
	{
		l_candidateId = p_chat.id;
	}

	if (l_candidateId.empty())
	{
		return "";
	}

	if (!l_findSessionFile(l_candidateId, l_chatsDir).empty())
	{
		return l_candidateId;
	}

	const fs::path l_currentWorkspace = ResolveWorkspaceRootPath(p_app, p_chat);
	for (const fs::path& l_workspaceRoot : CollectWorkspaceRootsForNativeHistory(p_app))
	{
		if (l_workspaceRoot == l_currentWorkspace)
		{
			continue;
		}

		const auto l_otherChatsDir = ResolveNativeHistoryChatsDirForWorkspace(l_workspaceRoot);
		if (!l_otherChatsDir.has_value() || l_otherChatsDir.value().empty())
		{
			continue;
		}

		if (!l_findSessionFile(l_candidateId, l_otherChatsDir.value()).empty())
		{
			if (l_tryPortSession(l_candidateId, l_otherChatsDir.value()))
			{
				return l_candidateId;
			}
		}
	}

	return "";
}

TemplatePreflightOutcome ProviderRequestService::PreflightWorkspaceTemplateForChat(AppState& p_app, const ProviderProfile& p_provider, const ChatSession& p_chat, std::string* p_bootstrapPromptOut, std::string* p_statusOut) const
{
	TemplateRuntimeService().RefreshTemplateCatalog(p_app);

	std::string l_effectiveTemplateId = p_chat.template_override_id;

	if (l_effectiveTemplateId.empty())
	{
		l_effectiveTemplateId = p_app.settings.default_prompt_profile_id;
	}

	if (l_effectiveTemplateId.empty())
	{
		l_effectiveTemplateId = p_app.settings.default_gemini_template_id;
	}

	if (l_effectiveTemplateId.empty())
	{
		if (p_statusOut != nullptr)
		{
			*p_statusOut = "No prompt profile selected. Set a default in Templates.";
		}

		return TemplatePreflightOutcome::ReadyWithoutTemplate;
	}

	const TemplateCatalogEntry* lcp_entry = TemplateRuntimeService().FindTemplateEntryById(p_app, l_effectiveTemplateId);

	if (lcp_entry == nullptr)
	{
		if (p_statusOut != nullptr)
		{
			*p_statusOut = "Selected prompt profile is missing: " + l_effectiveTemplateId + ". Choose one in Templates.";
		}

		p_app.open_template_manager_popup = true;
		return TemplatePreflightOutcome::BlockingError;
	}

	if (ProviderRuntime::UsesGeminiPathBootstrap(p_provider))
	{
		if (!TemplateRuntimeService().EnsureWorkspaceProviderLayout(p_app, p_chat, p_statusOut))
		{
			return TemplatePreflightOutcome::BlockingError;
		}

		std::error_code l_ec;
		fs::copy_file(lcp_entry->absolute_path, WorkspacePromptProfileTemplatePath(p_app, p_chat), fs::copy_options::overwrite_existing, l_ec);

		if (l_ec)
		{
			if (p_statusOut != nullptr)
			{
				*p_statusOut = "Failed to materialize workspace prompt profile file: " + l_ec.message();
			}

			return TemplatePreflightOutcome::BlockingError;
		}

		if (p_bootstrapPromptOut != nullptr)
		{
			*p_bootstrapPromptOut = p_provider.prompt_bootstrap_path.empty() ? kPromptBootstrapPath : p_provider.prompt_bootstrap_path;
		}

		return TemplatePreflightOutcome::ReadyWithTemplate;
	}

	if (p_bootstrapPromptOut != nullptr)
	{
		*p_bootstrapPromptOut = ReadTextFile(lcp_entry->absolute_path);

		if (p_bootstrapPromptOut->empty())
		{
			if (p_statusOut != nullptr)
			{
				*p_statusOut = "Selected prompt profile is empty.";
			}

			return TemplatePreflightOutcome::ReadyWithoutTemplate;
		}
	}

	return TemplatePreflightOutcome::ReadyWithTemplate;
}

bool ProviderRequestService::QueuePromptForChat(AppState& p_app, ChatSession& p_chat, const std::string& p_prompt, const bool p_templateControlMessage) const
{
	if (HasPendingCallForChat(p_app, p_chat.id))
	{
		p_app.status_line = "Provider command already running for this chat.";
		return false;
	}

	const std::string l_promptText = Trim(p_prompt);

	if (l_promptText.empty())
	{
		p_app.status_line = "Prompt is empty.";
		return false;
	}

	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);

	if (!ProviderRuntime::IsRuntimeEnabled(l_provider))
	{
		p_app.status_line = ProviderRuntime::DisabledReason(l_provider);

		if (p_app.status_line.empty())
		{
			p_app.status_line = "Runtime '" + l_provider.id + "' is disabled in this build.";
		}

		return false;
	}

	const bool l_useLocalRuntime = ProviderResolutionService().ChatUsesInternalEngine(p_app, p_chat);
	const bool l_useLocalBridgeRuntime = RuntimeLocalService().ProviderUsesLocalBridgeRuntime(l_provider);
	std::string l_templateStatus;
	std::string l_bootstrapPrompt;
	TemplatePreflightOutcome l_templateOutcome = TemplatePreflightOutcome::ReadyWithoutTemplate;

	if (!p_templateControlMessage || !p_chat.prompt_profile_bootstrapped)
	{
		l_templateOutcome = PreflightWorkspaceTemplateForChat(p_app, l_provider, p_chat, &l_bootstrapPrompt, &l_templateStatus);

		if (l_templateOutcome == TemplatePreflightOutcome::BlockingError)
		{
			p_app.status_line = l_templateStatus.empty() ? "Prompt profile preflight failed." : l_templateStatus;
			return false;
		}
	}

	const bool l_shouldBootstrapTemplate = !p_templateControlMessage && !p_chat.prompt_profile_bootstrapped && p_chat.messages.empty() && l_templateOutcome == TemplatePreflightOutcome::ReadyWithTemplate;
	std::string l_runtimePrompt = l_promptText;

	if (!p_templateControlMessage)
	{
		l_runtimePrompt = BuildRagEnhancedPrompt(p_app, p_chat, l_promptText);
	}

	if (l_shouldBootstrapTemplate && !l_bootstrapPrompt.empty())
	{
		l_runtimePrompt = l_bootstrapPrompt + "\n\n" + l_runtimePrompt;
	}

	if ((l_useLocalRuntime || l_useLocalBridgeRuntime) && !RuntimeLocalService().EnsureSelectedLocalRuntimeModelForProvider(p_app))
	{
		return false;
	}

	// NOTE: AddMessage(User) is intentionally deferred below each success path so
	// that a failed dispatch never leaves an unsent prompt persisted in history
	// while the text simultaneously remains in the composer (Bug #1 fix).

	const bool l_useSharedCliSession = !l_useLocalRuntime && ProviderRuntime::UsesCliOutput(l_provider) && l_provider.supports_interactive;

	if (!l_useLocalRuntime && ProviderRuntime::UsesCliOutput(l_provider) && !l_provider.supports_interactive)
	{
		p_app.status_line = "Provider runtime configuration error: provider has no interactive command.";
		return false;
	}

	if (l_useSharedCliSession)
	{
		std::string l_terminalError;

		if (!SendPromptToCliRuntime(p_app, p_chat, l_runtimePrompt, &l_terminalError))
		{
			if (l_terminalError == "OpenCode bridge is starting.")
			{
				p_app.status_line = l_terminalError;
				return false;
			}

			p_app.status_line = "Provider terminal send failed: " + (l_terminalError.empty() ? std::string("unknown error") : l_terminalError);
			return false;
		}

		// Prompt reached the terminal — record it now so the composer can be cleared.
		const int64_t user_prompt_tokens = static_cast<int64_t>(l_promptText.length() / 4);
		ChatDomainService().AddMessageWithAnalytics(p_chat, MessageRole::User, l_promptText, l_provider.id, user_prompt_tokens, 0, 0, 0, false);
		ChatHistorySyncService().SaveChatWithStatus(p_app, p_chat, "Prompt sent to provider terminal.", "Prompt sent, but chat save failed.");

		if (l_shouldBootstrapTemplate)
		{
			p_chat.prompt_profile_bootstrapped = true;
			ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
		}

		if (p_templateControlMessage)
		{
			p_app.status_line = "Prompt profile updated in live provider terminal session.";
		}
		else
		{
			p_app.status_line = "Prompt sent to live provider terminal session.";
		}

		p_app.scroll_to_bottom = true;
		return true;
	}

	if (l_useLocalRuntime)
	{
		std::string l_loadError;

		if (!RuntimeLocalService().EnsureLocalRuntimeModelLoaded(p_app, &l_loadError))
		{
			p_app.status_line = "Local runtime model load failed: " + (l_loadError.empty() ? std::string("unknown error") : l_loadError);
			return false;
		}

		// Model is loaded — record the user prompt before inference so the
		// conversation history is correct for both success and error replies.
		const int64_t local_user_tokens = static_cast<int64_t>(l_promptText.length() / 4);
		ChatDomainService().AddMessageWithAnalytics(p_chat, MessageRole::User, l_promptText, "ollama-local", local_user_tokens, 0, 0, 0, false);
		ChatHistorySyncService().SaveChatWithStatus(p_app, p_chat, "Prompt queued for local runtime.", "Prompt queued, but chat save failed.");

		const LocalEngineResponse l_response = p_app.runtime_model_service.SendPrompt(ResolveRagModelFolder(p_app), l_runtimePrompt);

		if (l_response.ok)
		{
			const int64_t local_output_chars = static_cast<int64_t>(l_response.text.length());
			ChatDomainService().AddMessageWithAnalytics(p_chat, MessageRole::Assistant, l_response.text, "ollama-local", 0, local_output_chars, 0, 0, false);
			ChatHistorySyncService().SaveChatWithStatus(p_app, p_chat, "Local response generated.", "Local response generated, but chat save failed.");
			p_app.scroll_to_bottom = true;
			return true;
		}

		ChatDomainService().AddMessage(p_chat, MessageRole::System, "Local runtime error: " + l_response.error);
		ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
		p_app.status_line = "Local runtime command failed.";
		p_app.scroll_to_bottom = true;
		return false;
	}

	std::vector<ChatSession> l_nativeBefore;

	if (ProviderResolutionService().ChatUsesNativeOverlayHistory(p_app, p_chat))
	{
		const fs::path l_nativeHistoryChatsDir = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(p_app, p_chat);
		l_nativeBefore = ChatHistorySyncService().LoadNativeSessionChats(l_nativeHistoryChatsDir, l_provider);
	}

	const std::string l_resumeSessionId = ChatHistorySyncService().ResolveResumeSessionIdForChat(p_app, p_chat);
	const std::string l_providerPrompt = ProviderRuntime::BuildPrompt(l_provider, l_runtimePrompt, p_chat.linked_files);
	const std::string l_providerCommand = ProviderRuntime::BuildCommand(l_provider, p_app.settings, l_providerPrompt, p_chat.linked_files, l_resumeSessionId);
	const std::string l_command = PlatformServicesFactory::Instance().process_service.BuildShellCommandWithWorkingDirectory(ResolveWorkspaceRootPath(p_app, p_chat), l_providerCommand);
	const std::string l_chatId = p_chat.id;

	PendingRuntimeCall l_pending;
	l_pending.chat_id = l_chatId;
	l_pending.resume_session_id = l_resumeSessionId;
	l_pending.provider_id_snapshot = l_provider.id;
	l_pending.native_history_chats_dir_snapshot = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(p_app, p_chat).string();
	l_pending.session_ids_before = ChatHistorySyncService().SessionIdsFromChats(l_nativeBefore);
	l_pending.command_preview = l_command;
	l_pending.state = std::make_shared<AsyncProcessTaskState>();
	l_pending.state->launch_time = std::chrono::steady_clock::now();
	l_pending.state->provider_id = l_provider.id;
	l_pending.state->estimated_input_tokens = static_cast<int64_t>(l_providerPrompt.length() / 4);
	std::shared_ptr<AsyncProcessTaskState> l_state = l_pending.state;
	l_pending.worker = std::make_unique<std::jthread>(
	    [l_command, l_state](std::stop_token stop_token)
	    {
		    l_state->result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(l_command, -1, stop_token);

		    if (!l_state->result.error.empty() && l_state->result.output.empty())
		    {
			    std::ostringstream message;
			    message << "Failed to launch provider CLI command";
			    message << ".";
			    l_state->result.output = message.str();
		    }
		    else
		    {
			    if (l_state->result.output.empty())
			    {
				    l_state->result.output = "(Provider CLI returned no output.)";
			    }

			    if (l_state->result.timed_out)
			    {
				    l_state->result.output += "\n\n[Provider CLI command timed out]";
			    }
			    else if (l_state->result.canceled)
			    {
				    l_state->result.output += "\n\n[Provider CLI command canceled]";
			    }
			    else if (l_state->result.exit_code != 0)
			    {
				    l_state->result.output += "\n\n[Provider CLI exited with code " + std::to_string(l_state->result.exit_code) + "]";
			    }
		    }

		    l_state->completed.store(true, std::memory_order_release);
	    });

	p_app.pending_calls.push_back(std::move(l_pending));

	// Async dispatch is now in flight — record the user prompt so the composer
	// can be cleared.  Deferring to this point means a launch failure (caught
	// earlier by the empty-command guard) never leaves an orphaned history entry.
	const int64_t user_prompt_tokens = static_cast<int64_t>(l_promptText.length() / 4);
	ChatDomainService().AddMessageWithAnalytics(p_chat, MessageRole::User, l_promptText, l_provider.id, user_prompt_tokens, 0, 0, 0, false);
	ChatHistorySyncService().SaveChatWithStatus(p_app, p_chat, "Prompt queued for async provider.", "Prompt queued, but chat save failed.");

	if (l_shouldBootstrapTemplate)
	{
		p_chat.prompt_profile_bootstrapped = true;
		ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
	}

	if (p_templateControlMessage)
	{
		p_app.status_line = "Prompt profile updated and synced to provider bootstrap flow.";
	}
	else if (l_templateOutcome == TemplatePreflightOutcome::ReadyWithoutTemplate && !l_templateStatus.empty())
	{
		p_app.status_line = l_templateStatus;
	}

	p_app.scroll_to_bottom = true;
	return true;
}

void ChatHistorySyncService::ApplyLocalOverrides(AppState& p_app, std::vector<ChatSession>& p_nativeChats) const
{
	const std::string l_selectedChatId = (ChatDomainService().SelectedChat(p_app) != nullptr) ? ChatDomainService().SelectedChat(p_app)->id : "";
	p_nativeChats = ChatDomainService().DeduplicateChatsById(std::move(p_nativeChats));
	std::vector<ChatSession> l_localChats = ChatRepository::LoadLocalChats(p_app.data_root);

	for (ChatSession& l_local : l_localChats)
	{
		if (!l_local.native_session_id.empty() || !NativeSessionLinkService().IsLocalDraftChatId(l_local.id))
		{
			continue;
		}

		const std::string l_normalizedProviderId = ProviderProfileMigrationService().MapLegacyRuntimeId(l_local.provider_id, false);
		const ProviderProfile* lcp_localProvider = ProviderProfileStore::FindById(p_app.provider_profiles, l_normalizedProviderId);
		const bool l_localChatUsesNativeOverlayHistory = Trim(l_local.provider_id).empty() || (lcp_localProvider != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*lcp_localProvider));

		if (!l_localChatUsesNativeOverlayHistory)
		{
			continue;
		}

		const auto l_inferredSessionId = NativeSessionLinkService().InferNativeSessionIdForLocalDraft(l_local, p_nativeChats);

		if (l_inferredSessionId.has_value())
		{
			PersistLocalDraftNativeSessionLink(p_app, l_local, l_inferredSessionId.value());
		}
	}

	l_localChats = ChatDomainService().DeduplicateChatsById(std::move(l_localChats));
	std::unordered_map<std::string, const ChatSession*> lcp_localMap;

	for (const ChatSession& l_local : l_localChats)
	{
		lcp_localMap[l_local.id] = &l_local;
	}

	std::unordered_set<std::string> l_nativeIds;

	for (ChatSession& l_native : p_nativeChats)
	{
		l_nativeIds.insert(l_native.id);
		const auto l_it = lcp_localMap.find(l_native.id);

		if (l_it == lcp_localMap.end())
		{
			continue;
		}

		const ChatSession& l_local = *l_it->second;
		if (!Trim(l_local.provider_id).empty())
		{
			l_native.provider_id = l_local.provider_id;
		}
		l_native.folder_id = l_local.folder_id;
		l_native.template_override_id = l_local.template_override_id;
		l_native.linked_files = l_local.linked_files;
		l_native.prompt_profile_bootstrapped = l_local.prompt_profile_bootstrapped;
		l_native.rag_enabled = l_local.rag_enabled;
		l_native.rag_source_directories = l_local.rag_source_directories;
		l_native.parent_chat_id = l_local.parent_chat_id;
		l_native.branch_root_chat_id = l_local.branch_root_chat_id;
		l_native.branch_from_message_index = l_local.branch_from_message_index;

		const bool l_localMessagesAreNewer = !l_local.messages.empty() && (l_local.messages.size() > l_native.messages.size() || (l_local.messages.size() == l_native.messages.size() && l_local.updated_at > l_native.updated_at));

		if (l_localMessagesAreNewer)
		{
			l_native.messages = l_local.messages;

			if (!l_local.updated_at.empty())
			{
				l_native.updated_at = l_local.updated_at;
			}

			if (l_native.created_at.empty() && !l_local.created_at.empty())
			{
				l_native.created_at = l_local.created_at;
			}
		}
	}

	std::vector<ChatSession> l_merged = p_nativeChats;

	for (const ChatSession& l_chat : l_localChats)
	{
		if (l_nativeIds.find(l_chat.id) != l_nativeIds.end())
		{
			continue;
		}

		if (!l_chat.native_session_id.empty())
		{
			continue;
		}

		const std::string l_normalizedProviderId = ProviderProfileMigrationService().MapLegacyRuntimeId(l_chat.provider_id, false);
		const ProviderProfile* lcp_localProvider = ProviderProfileStore::FindById(p_app.provider_profiles, l_normalizedProviderId);
		const bool l_localChatUsesNativeOverlayHistory = (lcp_localProvider == nullptr) ? true : ProviderRuntime::UsesNativeOverlayHistory(*lcp_localProvider);

		if (l_localChatUsesNativeOverlayHistory && !NativeSessionLinkService().IsLocalDraftChatId(l_chat.id) && !Trim(l_chat.provider_id).empty())
		{
			continue;
		}

		bool l_hasRunningTerminal = false;

		for (const auto& l_terminal : p_app.cli_terminals)
		{
			if (l_terminal != nullptr && l_terminal->attached_chat_id == l_chat.id && l_terminal->running)
			{
				l_hasRunningTerminal = true;
				break;
			}
		}

		if (l_chat.messages.empty() && !HasPendingCallForChat(p_app, l_chat.id) && !l_hasRunningTerminal && l_chat.id != l_selectedChatId)
		{
			continue;
		}

		l_merged.push_back(l_chat);
	}

	p_app.chats = ChatDomainService().DeduplicateChatsById(std::move(l_merged));
	ChatBranching::Normalize(p_app.chats);
	ChatDomainService().NormalizeChatFolderAssignments(p_app);
}

void ChatHistorySyncService::RefreshNativeSessionDirectory(AppState& p_app) const
{
	const ChatSession* lcp_selected = ChatDomainService().SelectedChat(p_app);

	if (lcp_selected != nullptr)
	{
		const auto l_selectedChatsDir = ResolveNativeHistoryChatsDirForWorkspace(ResolveWorkspaceRootPath(p_app, *lcp_selected));

		if (l_selectedChatsDir.has_value())
		{
			p_app.native_history_chats_dir = l_selectedChatsDir.value();
			return;
		}
	}

	std::error_code l_cwdEc;
	const fs::path l_cwd = fs::current_path(l_cwdEc);
	const auto l_tmpDir = l_cwdEc ? std::nullopt : ResolveNativeHistoryChatsDirForWorkspace(l_cwd);

	if (l_tmpDir.has_value())
	{
		p_app.native_history_chats_dir = l_tmpDir.value();
	}
	else
	{
		p_app.native_history_chats_dir.clear();
	}
}

bool ChatHistorySyncService::ExportChatToNative(const AppState& p_app, const ChatSession& p_chat) const
{
	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);

	if (!ProviderRuntime::SupportsGeminiJsonHistory(l_provider))
	{
		return false;
	}

	const fs::path l_chatsDir = ResolveNativeHistoryChatsDirForChat(p_app, p_chat);
	if (l_chatsDir.empty())
	{
		return false;
	}

	const std::string l_sessionId = p_chat.native_session_id.empty() ? p_chat.id : p_chat.native_session_id;
	const fs::path l_destFile = l_chatsDir / (l_sessionId + ".json");

#if UAM_ENABLE_ANY_GEMINI_PROVIDER
	return GeminiJsonHistoryStore::SaveFile(l_destFile, p_chat);
#else
	return false;
#endif
}

bool ChatHistorySyncService::TruncateNativeSessionFromDisplayedMessage(const AppState& p_app, const ChatSession& p_chat, const int p_displayedMessageIndex, std::string* p_errorOut) const
{
	if (p_errorOut != nullptr)
	{
		p_errorOut->clear();
	}

	if (p_chat.native_session_id.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Chat is not linked to a native runtime session.";
		}

		return false;
	}

	const fs::path l_chatsDir = ResolveNativeHistoryChatsDirForChat(p_app, p_chat);
	const auto l_sessionFile = FindNativeSessionFilePath(l_chatsDir, p_chat.native_session_id);

	if (!l_sessionFile.has_value())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Native runtime session file not found.";
		}

		return false;
	}

	JsonValue l_root;

	const std::string l_fileText = ReadTextFile(l_sessionFile.value());
	const std::optional<JsonValue> l_parsedRoot = ParseJson(l_fileText);

	if (!l_parsedRoot.has_value())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to parse native runtime session file.";
		}

		return false;
	}

	l_root = l_parsedRoot.value();

	JsonValue* lp_contents = nullptr;
	const auto l_contentsIt = l_root.object_value.find("contents");

	if (l_contentsIt != l_root.object_value.end())
	{
		lp_contents = &l_contentsIt->second;
	}

	if (lp_contents == nullptr || lp_contents->type != JsonValue::Type::Array)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Native runtime session does not contain a contents array.";
		}

		return false;
	}

	const int l_keepMessages = std::max(0, p_displayedMessageIndex + 1);
	int l_visibleMessages = 0;
	std::size_t l_truncateIndex = lp_contents->array_value.size();

	for (std::size_t l_i = 0; l_i < lp_contents->array_value.size(); ++l_i)
	{
		JsonValue* lp_role = nullptr;
		auto& l_item = lp_contents->array_value[l_i];
		const auto l_roleIt = l_item.object_value.find("role");

		if (l_roleIt != l_item.object_value.end())
		{
			lp_role = &l_roleIt->second;
		}

		if (lp_role == nullptr || lp_role->type != JsonValue::Type::String)
		{
			continue;
		}

		const MessageRole l_role = ProviderRuntime::RoleFromNativeType(ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat), lp_role->string_value);

		if (l_visibleMessages >= l_keepMessages)
		{
			l_truncateIndex = l_i;
			break;
		}

		++l_visibleMessages;
	}

	lp_contents->array_value.erase(lp_contents->array_value.begin() + static_cast<std::ptrdiff_t>(l_truncateIndex), lp_contents->array_value.end());

	if (!WriteTextFile(l_sessionFile.value(), SerializeJson(l_root)))
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to write updated native runtime session.";
		}

		return false;
	}

	return true;
}
