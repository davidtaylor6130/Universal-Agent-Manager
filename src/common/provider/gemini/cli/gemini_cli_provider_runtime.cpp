#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"

#include "common/config/approval_modes.h"
#include "common/chat/chat_ids.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/json_runtime.h"
#include "common/utils/io_utils.h"
#include "computer_use/computer_use_mcp_config.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	std::string RebuiltGeminiSessionNoteText(bool has_tool_calls, bool is_subagent)
	{
		std::string note_text = "Note: This session was rebuilt from UAM's source of truth.";
		if (has_tool_calls)
		{
			note_text += " Tool execution details and results were preserved but subagent conversations were stripped to reduce context.";
		}
		if (is_subagent)
		{
			note_text += " This is a subagent session extracted from a parent conversation.";
		}
		note_text += " The main conversation thread contains the essential information.";
		return note_text;
	}

	JsonValue TextContentJson(std::string_view text)
	{
		JsonValue content_json = uam::json::Object();
		uam::json::SetString(content_json, "text", text);
		return content_json;
	}

	JsonValue GeminiThoughtsJson(std::string_view thoughts)
	{
		JsonValue thoughts_json = uam::json::Array();
		std::istringstream thought_stream{std::string(thoughts)};
		std::string line;
		while (std::getline(thought_stream, line))
		{
			if (!line.empty())
			{
				JsonValue thought_json = uam::json::Object();
				uam::json::SetString(thought_json, "text", line);
				uam::json::PushValue(thoughts_json, std::move(thought_json));
			}
		}
		return thoughts_json;
	}

	JsonValue GeminiToolCallsJson(const std::vector<ToolCall>& tool_calls, std::string_view timestamp)
	{
		JsonValue tool_calls_json = uam::json::Array();
		for (const ToolCall& tool_call : tool_calls)
		{
			JsonValue tool_call_json = uam::json::Object();
			uam::json::SetString(tool_call_json, "id", tool_call.id);
			uam::json::SetString(tool_call_json, "name", tool_call.name);
			uam::json::SetString(tool_call_json, "status", tool_call.status);
			uam::json::SetString(tool_call_json, "timestamp", timestamp);

			const auto parsed_args = ParseJson(tool_call.args_json);
			uam::json::SetValue(tool_call_json, "args", parsed_args ? *parsed_args : uam::json::Object());
			uam::json::SetValue(tool_call_json, "result", TextContentJson(tool_call.result_text));
			uam::json::PushValue(tool_calls_json, std::move(tool_call_json));
		}
		return tool_calls_json;
	}

} // namespace

const char* GeminiCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kGeminiCli;
}


std::vector<std::string> GeminiCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	return uam::provider_runtime_internal::BuildInteractiveArgv(profile, chat, uam::provider_runtime_internal::MergeProviderSettings(profile, settings));
}

MessageRole GeminiCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> GeminiCliProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path&, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const
{
	return LoadGeminiJsonHistoryForRuntime(native_history_chats_dir, profile, options);
}

bool GeminiCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
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

bool GeminiCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return true;
}

std::string GeminiCliProviderRuntime::GenerateSessionUUID() const
{
	return PlatformServicesFactory::Instance().process_service.GenerateUuid();
}

std::string GeminiCliProviderRuntime::BuildSessionFilename(const ChatSession& chat) const
{
	return chat.native_session_id + ".json";
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

std::filesystem::path GeminiCliProviderRuntime::GetNativeSessionDirectory(const std::filesystem::path& workspace_path) const
{
	const auto tmp_dir = AppPaths::ResolveGeminiProjectTmpDir(workspace_path);
	if (!tmp_dir)
	{
		return {};
	}
	return *tmp_dir / "chats";
}

bool GeminiCliProviderRuntime::RebuildNativeSessionFile(const ProviderProfile&, const ChatSession& chat, const std::filesystem::path& workspace_path) const
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat.native_session_id) || workspace_path.empty())
	{
		return false;
	}

	const fs::path chats_dir = GetNativeSessionDirectory(workspace_path);
	if (chats_dir.empty())
	{
		return false;
	}

	std::error_code error;
	if (!uam::paths::CreateDirectoriesNoThrow(chats_dir, &error))
	{
		return false;
	}

	const bool has_tool_calls = std::ranges::any_of(chat.messages, [](const Message& message) { return !message.tool_calls.empty(); });
	const bool is_subagent = !chat.parent_chat_id.empty();

	JsonValue root = uam::json::Object();
	uam::json::SetString(root, "sessionId", chat.native_session_id);
	uam::json::SetString(root, "startTime", chat.created_at);
	uam::json::SetString(root, "lastUpdated", chat.updated_at);

	JsonValue messages_json = uam::json::Array();
	if (has_tool_calls || is_subagent)
	{
		JsonValue note_message_json = uam::json::Object();
		uam::json::SetString(note_message_json, "id", GenerateSessionUUID());
		uam::json::SetString(note_message_json, "timestamp", chat.created_at);
		uam::json::SetString(note_message_json, "type", "info");

		uam::json::SetValue(note_message_json, "content", TextContentJson(RebuiltGeminiSessionNoteText(has_tool_calls, is_subagent)));
		uam::json::SetValue(note_message_json, "thoughts", uam::json::Array());
		uam::json::SetValue(note_message_json, "toolCalls", uam::json::Array());
		uam::json::PushValue(messages_json, std::move(note_message_json));
	}

	for (const Message& message : chat.messages)
	{
		JsonValue message_json = uam::json::Object();
		uam::json::SetString(message_json, "id", GenerateSessionUUID());
		uam::json::SetString(message_json, "timestamp", message.created_at);
		uam::json::SetString(message_json, "type", NativeTypeFromRole(message.role));

		uam::json::SetValue(message_json, "content", TextContentJson(message.content));
		uam::json::SetValue(message_json, "thoughts", GeminiThoughtsJson(message.thoughts));
		uam::json::SetValue(message_json, "toolCalls", GeminiToolCallsJson(message.tool_calls, message.created_at));

		uam::json::PushValue(messages_json, std::move(message_json));
	}

	uam::json::SetValue(root, "messages", std::move(messages_json));
	const std::string session_filename = BuildSessionFilename(chat);
	return uam::io::WriteTextFile(chats_dir / session_filename, SerializeJson(root));
}

const IProviderRuntime& GetGeminiCliProviderRuntime()
{
	static const GeminiCliProviderRuntime runtime;
	return runtime;
}

ProviderDiscoveryResult GeminiCliProviderRuntime::DiscoverChatSources(const ProviderProfile&) const
{
	ProviderDiscoveryResult result;
	result.sources = DiscoverGeminiTmpChatSources(&result.error);
	return result;
}

std::vector<std::string> GeminiCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"gemini"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);
	argv.push_back("-p");
	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> GeminiCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession& chat) const
{
	std::vector<std::string> argv = {"gemini", "--acp"};
	const std::string approval_mode = uam::approval_modes::GeminiProviderApprovalModeFromAppModeId(uam::approval_modes::AppApprovalModeOrEmpty(chat.approval_mode));
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--approval-mode", approval_mode);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	uam::computer_use::AppendGeminiMcpLaunchArguments(argv, chat);
	return argv;
}
