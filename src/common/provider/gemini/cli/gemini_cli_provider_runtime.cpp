#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"

#include "app/application_core_helpers.h"
#include "common/paths/app_paths.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/json_runtime.h"

#include <Security/Security.h>

namespace fs = std::filesystem;

using namespace provider_runtime_internal;

const char* GeminiCliProviderRuntime::RuntimeId() const
{
	return "gemini-cli";
}

bool GeminiCliProviderRuntime::IsEnabled() const
{
	return UAM_ENABLE_RUNTIME_GEMINI_CLI != 0;
}

const char* GeminiCliProviderRuntime::DisabledReason() const
{
	return "Runtime 'gemini-cli' is disabled in this build (UAM_ENABLE_RUNTIME_GEMINI_CLI=OFF).";
}

std::string GeminiCliProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string GeminiCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	const AppSettings provider_settings = MergeProviderSettings(profile, settings);
	const std::string effective_resume_session_id = profile.supports_resume ? resume_session_id : "";
	return BuildCommandFromTemplate(provider_settings, prompt, files, effective_resume_session_id, "gemini -r {resume} {flags} {prompt}");
}

std::vector<std::string> GeminiCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	return provider_runtime_internal::BuildInteractiveArgv(profile, chat, MergeProviderSettings(profile, settings));
}

MessageRole GeminiCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> GeminiCliProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path&, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const
{
	return LoadGeminiJsonHistoryForRuntime(native_history_chats_dir, profile, options);
}

bool GeminiCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool GeminiCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return false;
}

bool GeminiCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool GeminiCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::UsesStructuredOutput(const ProviderProfile&) const
{
	return false;
}

bool GeminiCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return true;
}

std::string GeminiCliProviderRuntime::GenerateSessionUUID() const
{
	uint8_t randomBytes[16];
	if (SecRandomCopyBytes(kSecRandomDefault, 16, randomBytes) != errSecSuccess)
	{
		return "";
	}
	randomBytes[6] = (randomBytes[6] & 0x0f) | 0x40;
	randomBytes[8] = (randomBytes[8] & 0x3f) | 0x80;
	const char* hexDigits = "0123456789abcdef";
	char uuid[37];
	for (int i = 0; i < 16; ++i)
	{
		int byte = randomBytes[i];
		uuid[i * 2] = hexDigits[(byte >> 4) & 0x0f];
		uuid[i * 2 + 1] = hexDigits[byte & 0x0f];
	}
	uuid[8] = uuid[13] = uuid[18] = uuid[23] = '-';
	uuid[36] = '\0';
	return std::string(uuid);
}

std::string GeminiCliProviderRuntime::BuildSessionFilename(const ChatSession& chat) const
{
	std::string datePart = "unknown";
	if (!chat.created_at.empty())
	{
		std::string ts = chat.created_at;
		std::size_t tPos = ts.find('T');
		if (tPos != std::string::npos && tPos + 8 <= ts.size())
		{
			datePart = ts.substr(0, tPos + 8);
			for (char& c : datePart)
			{
				if (c == ':')
				{
					c = '-';
				}
			}
		}
	}
	std::string shortId = chat.native_session_id.substr(0, 8);
	return "session-" + datePart + "-" + shortId + ".json";
}

std::string GeminiCliProviderRuntime::NativeTypeFromRole(MessageRole role) const
{
	switch (role)
	{
	case MessageRole::User:
		return "user";
	case MessageRole::Assistant:
		return "gemini";
	case MessageRole::System:
		return "info";
	default:
		return "info";
	}
}

std::filesystem::path GeminiCliProviderRuntime::GetNativeSessionDirectory(const std::filesystem::path& workspacePath) const
{
	const auto tmpDir = AppPaths::ResolveGeminiProjectTmpDir(workspacePath);
	if (!tmpDir.has_value())
	{
		return {};
	}
	return tmpDir.value() / "chats";
}

bool GeminiCliProviderRuntime::RebuildNativeSessionFile(const ProviderProfile&, const ChatSession& chat, const std::filesystem::path& workspacePath) const
{
	if (chat.native_session_id.empty() || workspacePath.empty())
	{
		return false;
	}
	fs::path chatsDir = GetNativeSessionDirectory(workspacePath);
	if (chatsDir.empty())
	{
		return false;
	}
	std::error_code l_ec;
	fs::create_directories(chatsDir, l_ec);
	if (l_ec)
	{
		return false;
	}
	JsonValue root;
	root.type = JsonValue::Type::Object;
	root.object_value["sessionId"].type = JsonValue::Type::String;
	root.object_value["sessionId"].string_value = chat.native_session_id;
	root.object_value["startTime"].type = JsonValue::Type::String;
	root.object_value["startTime"].string_value = chat.created_at;
	root.object_value["lastUpdated"].type = JsonValue::Type::String;
	root.object_value["lastUpdated"].string_value = chat.updated_at;
	JsonValue msgs;
	msgs.type = JsonValue::Type::Array;
	for (const auto& m : chat.messages)
	{
		JsonValue msg;
		msg.type = JsonValue::Type::Object;
		msg.object_value["id"].type = JsonValue::Type::String;
		msg.object_value["id"].string_value = GenerateSessionUUID();
		msg.object_value["timestamp"].type = JsonValue::Type::String;
		msg.object_value["timestamp"].string_value = m.created_at;
		msg.object_value["type"].type = JsonValue::Type::String;
		msg.object_value["type"].string_value = NativeTypeFromRole(m.role);
		JsonValue content;
		content.type = JsonValue::Type::Object;
		content.object_value["text"].type = JsonValue::Type::String;
		content.object_value["text"].string_value = m.content;
		msg.object_value["content"] = content;
		JsonValue thoughts;
		thoughts.type = JsonValue::Type::Array;
		msg.object_value["thoughts"] = thoughts;
		JsonValue toolCalls;
		toolCalls.type = JsonValue::Type::Array;
		msg.object_value["toolCalls"] = toolCalls;
		msgs.array_value.push_back(std::move(msg));
	}
	root.object_value["messages"] = std::move(msgs);
	std::string filename = BuildSessionFilename(chat);
	return WriteTextFile(chatsDir / filename, SerializeJson(root));
}

const IProviderRuntime& GetGeminiCliProviderRuntime()
{
	static const GeminiCliProviderRuntime runtime;
	return runtime;
}

ProviderDiscoveryResult GeminiCliProviderRuntime::DiscoverChatSources(const ProviderProfile&) const
{
	ProviderDiscoveryResult result;
	result.sources = DiscoverGeminiTmpChatSources();
	return result;
}