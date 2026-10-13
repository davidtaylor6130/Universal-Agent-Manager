#pragma once

#include "common/state/app_state.h"

// nlohmann/json is provided by the CMake dependency target.
// and the include path is added to the target, so this include works
// once CMake has run FetchContent.
#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>

struct ChatMessagePage;

namespace uam
{
struct StartupNativeImportSnapshot;

/// <summary>
/// Converts AppState to a nlohmann::json object suitable for serialising
/// to window.uamPush() or returning from a getInitialState CEF query.
///
/// Only the fields the React frontend needs are included — internal runtime
/// state such as pending calls and platform task handles is omitted.
/// </summary>
class StateSerializer
{
  public:
	/// Serialise the application state, optionally omitting loaded chat messages.
	static nlohmann::json Serialize(const AppState& app, bool summary_only = false);

	/// Serialise a compact state summary for push fingerprinting.
	static nlohmann::json SerializeFingerprint(const AppState& app);

	/// <summary>Serializes bounded startup discovery metadata without private transcript payloads.</summary>
	static nlohmann::json SerializeStartupNativeImports(const StartupNativeImportSnapshot& snapshot);

	/// Serialise a single chat session (messages included).
	static nlohmann::json SerializeSession(const ChatSession& session, bool reuse_selected_history = false);

	/// Return the message digest used in serialized chat state.
	static std::string MessageDigest(const ChatSession& session, bool reuse_selected_history = false);

	/// <summary>Number of messages hashed by the latest selected-history summary, for performance diagnostics.</summary>
	static std::size_t SelectedHistoryRehashedMessageCount();

	/// Serialise one bounded message page without serialising the rest of the session.
	static nlohmann::json SerializeMessagePage(const ChatSession& session,
	                                           std::size_t limit,
	                                           std::optional<std::size_t> before,
	                                           bool defer_tool_call_content = false,
	                                           std::string_view known_digest = {}, bool reuse_selected_history = false);

	/// Build the full persisted tool-call detail returned on demand.
	static std::string ToolCallContentForFrontend(const ToolCall& tool_call);
	static std::string ToolCallContentForFrontend(const AcpToolCallState& tool_call);

	/// Return one bounded, UTF-8-safe page of tool-call output.
	/// <summary>Serializes detached global page coordinates without digesting or hydrating the full transcript.</summary>
	static nlohmann::json SerializeDetachedMessagePage(const ChatMessagePage& page,
	    bool defer_tool_call_content, std::string_view known_digest = {}, bool allow_unchanged = true);
	static nlohmann::json ToolCallContentPageForFrontend(std::string_view content,
	                                                     std::size_t offset);
	static nlohmann::json ToolCallContentPageForFrontend(const ToolCall& tool, std::size_t offset);
	static nlohmann::json ToolCallContentPageForFrontend(const AcpToolCallState& tool, std::size_t offset);

	/// Serialise a single folder.
	static nlohmann::json SerializeFolder(const ChatFolder& folder, bool probe_directory = true);

	/// Serialise a resource collection and its ordered references.
	static nlohmann::json SerializeResourceCollection(const ResourceCollection& collection);

	/// Serialise a single resource reference.
	static nlohmann::json SerializeResourceReference(const ResourceReference& reference);

	/// Serialise a provider profile.
	static nlohmann::json SerializeProvider(const ProviderProfile& profile);
};

} // namespace uam
