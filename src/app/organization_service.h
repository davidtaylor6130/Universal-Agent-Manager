#pragma once
#include "common/state/app_state.h"
#include <nlohmann/json.hpp>
#include <string>

namespace uam
{
/// <summary>Applies guarded collection order or resource membership changes with one durable save.</summary>
bool ApplyOrganizationAction(AppState& app, const nlohmann::json& payload, std::string* error_out);
}
