#pragma once

#include "common/state/app_state.h"

// nlohmann/json is fetched by CMake into _deps/nlohmann_json/json.hpp
// and the include path is added to the target, so this include works
// once CMake has run FetchContent.
#include <nlohmann/json.hpp>

namespace uam
{

/// <summary>
/// Converts AppState to a nlohmann::json object suitable for serialising
/// to window.uamPush() or returning from a getInitialState CEF query.
///
/// Only the fields the React frontend needs are included — internal runtime
/// state (pending calls, VCS snapshots, RAG scan state, etc.) is omitted.
/// </summary>
class StateSerializer
{
  public:
	/// Serialise the full application state.
	static nlohmann::json Serialize(const AppState& app);

	/// Serialise a single chat session (messages included).
	static nlohmann::json SerializeSession(const ChatSession& session);

	/// Serialise a single folder.
	static nlohmann::json SerializeFolder(const ChatFolder& folder);

	/// Serialise a provider profile.
	static nlohmann::json SerializeProvider(const ProviderProfile& profile);
};

} // namespace uam
