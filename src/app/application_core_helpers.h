#ifndef UAM_APP_APPLICATION_CORE_HELPERS_H
#define UAM_APP_APPLICATION_CORE_HELPERS_H


#include "common/state/app_state.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

std::string Trim(const std::string& p_value);
std::string NormalizeVectorDatabaseName(std::string p_value);
std::string TimestampNow();
std::string NewSessionId();
std::string ReadTextFile(const std::filesystem::path& p_path);
bool WriteTextFile(const std::filesystem::path& p_path, const std::string& p_content);
std::filesystem::path ResolvePromptProfileRootPath(const AppSettings& p_settings);
std::filesystem::path ResolveWorkspaceRootPath(const uam::AppState& p_app, const ChatSession& p_chat);
std::filesystem::path WorkspacePromptProfileRootPath(const uam::AppState& p_app, const ChatSession& p_chat);
std::filesystem::path WorkspacePromptProfileTemplatePath(const uam::AppState& p_app, const ChatSession& p_chat);
std::filesystem::path ResolveCurrentRagFallbackSourceRoot(const uam::AppState& p_app);
std::uint64_t Fnv1a64(const std::string& p_text);
std::string Hex64(std::uint64_t p_value);
std::string ToLowerAscii(std::string p_value);
bool IsLikelyBinaryBlob(const std::string& p_content);
bool IsRagIndexableTextFile(const std::filesystem::path& p_path);
std::string TruncateToApproxTokenCount(const std::string& p_content, std::size_t p_maxTokens);
std::filesystem::path BuildRagTokenCappedStagingRoot(const uam::AppState& p_app, const std::string& p_workspaceKey);
bool BuildRagTokenCappedStagingTree(const std::filesystem::path& p_sourceRoot,
                                    const std::filesystem::path& p_stagingRoot,
                                    int p_maxTokens,
                                    std::size_t* p_indexedFilesOut,
                                    std::string* p_errorOut);
std::filesystem::path ResolveProjectRagSourceRoot(const uam::AppState& p_app, const std::filesystem::path& p_fallbackSourceRoot = {});
std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& p_path);
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
std::string BuildRagContextBlock(const std::vector<RagSnippet>& p_snippets);
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

#endif // UAM_APP_APPLICATION_CORE_HELPERS_H
