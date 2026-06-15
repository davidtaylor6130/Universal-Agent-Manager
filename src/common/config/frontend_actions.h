#pragma once

#include "common/utils/string_utils.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

	/// <summary>
	/// Small, hand-editable action description consumed by the UI layer.
	/// </summary>
	struct FrontendAction
	{
		std::string key;
		std::string label;
		std::string group;
		bool visible = true;
		int order = 0;
		std::map<std::string, std::string> properties;
	};

	/// <summary>
	/// Global metadata plus ordered frontend action entries.
	/// </summary>
	struct FrontendActionMap
	{
		std::map<std::string, std::string> metadata;
		std::vector<FrontendAction> actions;
	};

	/// <summary>Returns the default action map.</summary>
	FrontendActionMap DefaultFrontendActionMap();

	/// <summary>Finds a mutable action by key.</summary>
	FrontendAction* FindAction(FrontendActionMap& action_map, std::string_view key);
	/// <summary>Finds a read-only action by key.</summary>
	const FrontendAction* FindAction(const FrontendActionMap& action_map, std::string_view key);

	/// <summary>Normalizes ordering and defaults within the action map.</summary>
	void NormalizeFrontendActionMap(FrontendActionMap& action_map);

	/// <summary>Parses textual frontend action configuration.</summary>
	bool ParseFrontendActionMap(const std::string& text, FrontendActionMap& out_map, std::string* error_out = nullptr);

	/// <summary>Serializes the frontend action map to text.</summary>
	std::string SerializeFrontendActionMap(const FrontendActionMap& action_map);

	/// <summary>Loads a frontend action map from disk.</summary>
	bool LoadFrontendActionMap(const std::filesystem::path& path, FrontendActionMap& out_map, std::string* error_out = nullptr);

	/// <summary>Saves a frontend action map to disk.</summary>
	bool SaveFrontendActionMap(const std::filesystem::path& path, const FrontendActionMap& action_map, std::string* error_out = nullptr);

	/// <summary>Returns visibility for a UI action with fallback when missing.</summary>
	inline bool FrontendActionVisible(const FrontendActionMap& action_map, std::string_view key, bool fallback_visible = true)
	{
		const FrontendAction* action = FindAction(action_map, key);
		return (action == nullptr) ? fallback_visible : action->visible;
	}

	/// <summary>Returns label text for a UI action with fallback when missing.</summary>
	inline std::string FrontendActionLabel(const FrontendActionMap& action_map, std::string_view key, std::string_view fallback_label)
	{
		const FrontendAction* action = FindAction(action_map, key);

		if (action == nullptr)
		{
			return std::string(fallback_label);
		}

		const std::string label = uam::strings::Trim(action->label);
		if (label.empty())
		{
			return std::string(fallback_label);
		}

		return label;
	}

} // namespace uam
