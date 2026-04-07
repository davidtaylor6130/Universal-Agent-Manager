#pragma once

#include "common/rag/rag_index_service.h"
#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

bool IsRagIndexableTextFile(const std::filesystem::path& p_path);
std::filesystem::path BuildRagTokenCappedStagingRoot(const uam::AppState& p_app, const std::string& p_workspaceKey);
bool BuildRagTokenCappedStagingTree(const std::filesystem::path& p_sourceRoot,
                                    const std::filesystem::path& p_stagingRoot,
                                    int p_maxTokens,
                                    std::size_t* p_indexedFilesOut,
                                    std::string* p_errorOut);
std::filesystem::path ResolveProjectRagSourceRoot(const uam::AppState& p_app, const std::filesystem::path& p_fallbackSourceRoot = {});
std::vector<std::filesystem::path> ResolveRagSourceRootsForChat(const uam::AppState& p_app,
                                                                const ChatSession& p_chat,
                                                                const std::filesystem::path& p_fallbackSourceRoot = {});
std::vector<std::filesystem::path> DiscoverRagSourceFolders(const std::filesystem::path& p_workspaceRoot);
std::string RagDatabaseNameForSourceRoot(const AppSettings& p_settings, const std::filesystem::path& p_sourceRoot);
bool ChatHasRagSourceDirectory(const ChatSession& p_chat, const std::filesystem::path& p_sourceRoot);
bool AddChatRagSourceDirectory(ChatSession& p_chat, const std::filesystem::path& p_sourceRoot);
bool RemoveChatRagSourceDirectoryAt(ChatSession& p_chat, std::size_t p_index);
bool RemoveChatRagSourceDirectory(ChatSession& p_chat, const std::filesystem::path& p_sourceRoot);
bool DirectoryContainsGguf(const std::filesystem::path& p_directory);
std::filesystem::path ResolveRagModelFolder(const uam::AppState& p_app, const AppSettings* p_settingsOverride = nullptr);
RagIndexService::Config RagConfigFromSettings(const AppSettings& p_settings);
void SyncRagServiceConfig(uam::AppState& p_app);
bool IsRagEnabledForChat(const uam::AppState& p_app, const ChatSession& p_chat);
std::string BuildRagEnhancedPrompt(uam::AppState& p_app, const ChatSession& p_chat, const std::string& p_promptText);
bool TriggerProjectRagScan(uam::AppState& p_app,
                           bool p_reusePreviousSource,
                           const std::filesystem::path& p_fallbackSourceRoot,
                           std::string* p_errorOut = nullptr);
void PollRagScanState(uam::AppState& p_app);
RagScanState EffectiveRagScanState(const uam::AppState& p_app);
std::string BuildRagStatusText(const uam::AppState& p_app);
void EnsureRagManualQueryWorkspaceState(uam::AppState& p_app, const std::string& p_workspaceKey);
void AppendRagScanReport(uam::AppState& p_app, const std::string& p_message);
void RunRagManualTestQuery(uam::AppState& p_app, const std::filesystem::path& p_workspaceRoot);
std::string BuildRagContextBlock(const std::vector<RagSnippet>& p_snippets);
