#pragma once

#include "common/models/app_models.h"
#include "common/provider/native_child_timing.h"
#include "common/usage/transcript_usage.h"
#include "common/utils/nlohmann_json_utils.h"

#include <algorithm>
#include <iterator>

namespace uam::copilot
{

// Both the live stream and native journal carry the same child-local lifecycle.
// Parent tool timestamps and durations are deliberately not inputs here.
inline void ApplyChildLifecycle(std::vector<SubAgentChild>& children, const std::string& child_id,
    const std::string& title, const std::string& type, const nlohmann::json& event)
{
	if (child_id.empty() || (type != "subagent.started" && type != "subagent.completed" && type != "subagent.failed")) return;
	auto found = std::ranges::find(children, child_id, &SubAgentChild::id);
	if (found == children.end())
	{
		children.push_back({child_id, title, ""});
		found = std::prev(children.end());
	}
	SubAgentChild& child = *found;
	if (!title.empty()) child.title = title;
	const bool terminal = child.status == "completed" || child.status == "failed" || child.status == "cancelled";
	const auto timestamp = usage::TimestampMilliseconds(nlohmann_json::StringViewOrEmpty(event, "timestamp"));
	if (type == "subagent.started")
	{
		if (!terminal) child.status = "running";
		if (!child.started_at_epoch_ms && timestamp && (!child.finished_at_epoch_ms || *timestamp <= *child.finished_at_epoch_ms)) child.started_at_epoch_ms = timestamp;
		return;
	}
	if (terminal) return;
	const auto data = event.find("data");
	const bool cancelled = data != event.end() && nlohmann_json::BoolFieldStrict(*data, "cancelled").value_or(false);
	child.status = type == "subagent.completed" ? cancelled ? "cancelled" : "completed" : "failed";
	if (timestamp && (!child.started_at_epoch_ms || *timestamp >= *child.started_at_epoch_ms)) child.finished_at_epoch_ms = timestamp;
	if (data != event.end()) child.duration_ms = native_agents::ReportedDurationMilliseconds(*data, "durationMs");
}

}
