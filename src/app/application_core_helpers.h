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
std::string TruncateToApproxTokenCount(const std::string& p_content, std::size_t p_maxTokens);
std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& p_path);

#endif // UAM_APP_APPLICATION_CORE_HELPERS_H
