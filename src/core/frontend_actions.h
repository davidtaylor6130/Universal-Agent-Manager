#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace uam {

// A small, hand-editable action description that can be consumed by any UI layer.
struct FrontendAction {
  std::string key;
  std::string label;
  std::string group;
  bool visible = true;
  int order = 0;
  std::map<std::string, std::string> properties;
};

// Global metadata plus the ordered action list.
struct FrontendActionMap {
  std::map<std::string, std::string> metadata;
  std::vector<FrontendAction> actions;
};

FrontendActionMap DefaultFrontendActionMap();

FrontendAction* FindAction(FrontendActionMap& action_map, const std::string& key);
const FrontendAction* FindAction(const FrontendActionMap& action_map, const std::string& key);

void NormalizeFrontendActionMap(FrontendActionMap& action_map);

bool ParseFrontendActionMap(const std::string& text,
                            FrontendActionMap& out_map,
                            std::string* error_out = nullptr);

std::string SerializeFrontendActionMap(const FrontendActionMap& action_map);

bool LoadFrontendActionMap(const std::filesystem::path& path,
                           FrontendActionMap& out_map,
                           std::string* error_out = nullptr);

bool SaveFrontendActionMap(const std::filesystem::path& path,
                           const FrontendActionMap& action_map,
                           std::string* error_out = nullptr);

}  // namespace uam
