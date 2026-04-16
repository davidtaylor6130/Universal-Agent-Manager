#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"

#include "app/application_core_helpers.h"
#include "common/paths/app_paths.h"
#include "common/platform/platform_services.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/json_runtime.h"

#include <sstream>

namespace fs = std::filesystem;

using namespace provider_runtime_internal;

const char* GeminiCliProviderRuntime::RuntimeId() const
{
	return "gemini-cli";
}

bool GeminiCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* GeminiCliProviderRuntime::DisabledReason() const
{
	return "";
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
	bool has_tool_calls = false;
	for (const auto& m : chat.messages)
	{
		if (!m.tool_calls.empty())
		{
			has_tool_calls = true;
			break;
		}
	}
	bool is_subagent = !chat.parent_chat_id.empty();
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
	if (has_tool_calls || is_subagent)
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
		JsonValue note_msg;
		note_msg.type = JsonValue::Type::Object;
		note_msg.object_value["id"].type = JsonValue::Type::String;
		note_msg.object_value["id"].string_value = GenerateSessionUUID();
		note_msg.object_value["timestamp"].type = JsonValue::Type::String;
		note_msg.object_value["timestamp"].string_value = chat.created_at;
		note_msg.object_value["type"].type = JsonValue::Type::String;
		note_msg.object_value["type"].string_value = "info";
		JsonValue note_content;
		note_content.type = JsonValue::Type::Object;
		note_content.object_value["text"].type = JsonValue::Type::String;
		note_content.object_value["text"].string_value = note_text;
		note_msg.object_value["content"] = note_content;
		JsonValue note_thoughts;
		note_thoughts.type = JsonValue::Type::Array;
		note_msg.object_value["thoughts"] = note_thoughts;
		JsonValue note_tool_calls;
		note_tool_calls.type = JsonValue::Type::Array;
		note_msg.object_value["toolCalls"] = note_tool_calls;
		msgs.array_value.push_back(std::move(note_msg));
	}
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
		if (!m.thoughts.empty())
		{
			JsonValue thoughts;
			thoughts.type = JsonValue::Type::Array;
			std::istringstream thought_stream(m.thoughts);
			std::string line;
			while (std::getline(thought_stream, line))
			{
				if (!line.empty())
				{
					JsonValue thought_obj;
					thought_obj.type = JsonValue::Type::Object;
					thought_obj.object_value["text"].type = JsonValue::Type::String;
					thought_obj.object_value["text"].string_value = line;
					thoughts.array_value.push_back(std::move(thought_obj));
				}
			}
			msg.object_value["thoughts"] = std::move(thoughts);
		}
		else
		{
			JsonValue thoughts;
			thoughts.type = JsonValue::Type::Array;
			msg.object_value["thoughts"] = thoughts;
		}
		if (!m.tool_calls.empty())
		{
			JsonValue toolCalls;
			toolCalls.type = JsonValue::Type::Array;
			for (const auto& tc : m.tool_calls)
			{
				JsonValue tc_obj;
				tc_obj.type = JsonValue::Type::Object;
				tc_obj.object_value["id"].type = JsonValue::Type::String;
				tc_obj.object_value["id"].string_value = tc.id;
				tc_obj.object_value["name"].type = JsonValue::Type::String;
				tc_obj.object_value["name"].string_value = tc.name;
				tc_obj.object_value["status"].type = JsonValue::Type::String;
				tc_obj.object_value["status"].string_value = tc.status;
				tc_obj.object_value["timestamp"].type = JsonValue::Type::String;
				tc_obj.object_value["timestamp"].string_value = m.created_at;
				const auto args_opt = ParseJson(tc.args_json);
				if (args_opt.has_value())
				{
					tc_obj.object_value["args"] = args_opt.value();
				}
				else
				{
					JsonValue args;
					args.type = JsonValue::Type::Object;
					tc_obj.object_value["args"] = args;
				}
				JsonValue result;
				result.type = JsonValue::Type::Object;
				result.object_value["text"].type = JsonValue::Type::String;
				result.object_value["text"].string_value = tc.result_text;
				tc_obj.object_value["result"] = result;
				toolCalls.array_value.push_back(std::move(tc_obj));
			}
			msg.object_value["toolCalls"] = std::move(toolCalls);
		}
		else
		{
			JsonValue toolCalls;
			toolCalls.type = JsonValue::Type::Array;
			msg.object_value["toolCalls"] = toolCalls;
		}
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
