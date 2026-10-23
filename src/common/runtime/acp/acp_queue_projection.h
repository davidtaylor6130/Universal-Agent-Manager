#pragma once

#include "common/models/app_models.h"

namespace uam
{
	/// Future input has visible queue coordinates; direct receipts and prepared/native deliveries do not.
	inline bool IsVisibleAcpQueuedPrompt(const AcpQueuedUserPromptState& prompt)
	{
		return !prompt.direct_admission && !prompt.prepared_for_delivery && prompt.claude_quota_continuation_id.empty();
	}
}
