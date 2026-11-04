#pragma once

#include "common/provider/opencode_native_http.h"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace uam::opencode_permissions
{
	using Json = nlohmann::json;

	// Pinned producer contract: OpenCode v1.18.35 session/processor.ts348-355
	// and acp/permission.ts58-78. A normal MCP doom/loop action instead has
	// metadata {}, wildcard patterns, and a native tool link (session/tools.ts).
	// This recognizes a trusted native producer shape, not plugin attestation.
	inline bool QuestionMetadata(const Json& metadata)
	{
		return metadata.is_object() && metadata.size() == 2 &&
		    metadata.contains("tool") && metadata["tool"].is_string() &&
		    !metadata["tool"].get_ref<const std::string&>().empty() &&
		    metadata["tool"].get_ref<const std::string&>().size() <= 1024 &&
		    metadata.contains("input") && metadata["input"].is_object();
	}

	inline bool ToolPattern(const Json& patterns, const Json& tool)
	{
		return patterns.is_array() && patterns.size() == 1 && patterns.front() == tool;
	}

	inline bool AcpDoomLoopQuestion(std::string_view method, std::string_view title,
	    std::string_view kind, const std::string& input_json)
	{
		if (title != "doom_loop" || kind != "other" || input_json.size() > 12 * 1024) return false;
		const Json input = Json::parse(input_json, nullptr, false);
		if (method == "session/request_permission") return QuestionMetadata(input);
		// UAM's authenticated native companion relay flattens metadata and adds
		// exactly these three fields; never accept its shape on ordinary ACP.
		if (method != "opencode/nativePermission" || !input.is_object() || input.size() != 5 ||
		    !input.contains("nativeSessionId") || !input["nativeSessionId"].is_string() ||
		    !opencode_native::ValidNativeId(input["nativeSessionId"].get_ref<const std::string&>()) ||
		    !input.contains("permission") || input["permission"] != "doom_loop" ||
		    !input.contains("patterns") || !input.contains("tool") || !input.contains("input")) return false;
		return QuestionMetadata(Json{{"tool", input["tool"]}, {"input", input["input"]}}) &&
		    ToolPattern(input["patterns"], input["tool"]);
	}

	inline bool NativeDoomLoopQuestion(const Json& permission)
	{
		// permission/index.ts list() retains the native Request snapshot. Unknown
		// fields/versions and linked tool actions retain denial under this ceiling.
		if (!permission.is_object() || permission.size() != 6 ||
		    !permission.contains("id") || !permission["id"].is_string() ||
		    !opencode_native::ValidNativeId(permission["id"].get_ref<const std::string&>()) ||
		    !permission.contains("sessionID") || !permission["sessionID"].is_string() ||
		    !opencode_native::ValidNativeId(permission["sessionID"].get_ref<const std::string&>()) ||
		    !permission.contains("permission") || permission["permission"] != "doom_loop" ||
		    !permission.contains("metadata") || !QuestionMetadata(permission["metadata"]) ||
		    !permission.contains("patterns") || !permission.contains("always") || permission.contains("tool")) return false;
		return ToolPattern(permission["patterns"], permission["metadata"]["tool"]) &&
		    ToolPattern(permission["always"], permission["metadata"]["tool"]);
	}
}
