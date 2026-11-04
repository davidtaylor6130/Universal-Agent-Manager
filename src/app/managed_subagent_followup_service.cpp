#include "app/managed_subagent_followup_service.h"
#include "common/utils/uuid.h"
#include "app/agent_run_scheduler.h"

#include <algorithm>
#include <utility>

namespace uam
{
	ManagedFollowupState ManagedSubagentFollowupService::GetState(const AppState& app,
	    const std::string& root_chat_id, const std::string& run_id)
	{
		return AgentRunScheduler::GetFollowupState(app, root_chat_id, run_id);
	}

	ManagedFollowupAdmission ManagedSubagentFollowupService::TrySubmit(AppState& app,
	    const std::string& root_chat_id, const std::string& run_id,
	    const ManagedFollowupRequest& request,
	    std::function<bool(const ManagedFollowupSubmitResult&)> completion,
	    ManagedFollowupCode* rejection_code)
	{
		return AgentRunScheduler::TryBeginFollowup(app, root_chat_id, run_id, request, std::move(completion), rejection_code);
	}

	ManagedFollowupSubmitResult ManagedSubagentFollowupService::Submit(AppState& app,
	    const std::string& root_chat_id, const std::string& run_id,
	    const ManagedFollowupRequest& request)
	{
		(void)request;
		ManagedFollowupSubmitResult result;
		result.code = GetState(app, root_chat_id, run_id).code;
		if (result.code != ManagedFollowupCode::ScopeChanged && result.code != ManagedFollowupCode::LegacyIdentityUnverified) result.code = ManagedFollowupCode::NotImplemented;
		return result;
	}
} // namespace uam
