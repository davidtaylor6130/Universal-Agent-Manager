#pragma once

/// <summary>
/// Template override actions and pending template-change state reset.
/// </summary>
inline void ClearPendingTemplateChange(AppState& app)
{
	app.pending_template_change_chat_id.clear();
	app.pending_template_change_override_id.clear();
	app.open_template_change_warning_popup = false;
}

inline bool ApplyChatTemplateOverride(AppState& app, ChatSession& chat, const std::string& override_id, const bool send_control_message_for_started_chat)
{
	if (!override_id.empty() && FindTemplateEntryById(app, override_id) == nullptr)
	{
		app.status_line = "Template not found in catalog: " + override_id;
		app.open_template_manager_popup = true;
		return false;
	}

	if (chat.template_override_id == override_id)
	{
		return true;
	}

	chat.template_override_id = override_id;
	chat.updated_at = TimestampNow();
	SaveAndUpdateStatus(app, chat, "Chat template updated.", "Template changed in UI, but failed to save chat.");

	std::string template_status;
	const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);
	const TemplatePreflightOutcome outcome = PreflightWorkspaceTemplateForChat(app, provider, chat, nullptr, &template_status);

	if (outcome == TemplatePreflightOutcome::BlockingError)
	{
		app.status_line = template_status.empty() ? "Template changed, but local sync failed." : template_status;
		return false;
	}

	if (send_control_message_for_started_chat && !chat.messages.empty())
	{
		app.status_line = "Template updated for this chat. Bootstrap content applies on the next provider request.";
		return true;
	}

	if (outcome == TemplatePreflightOutcome::ReadyWithoutTemplate && !template_status.empty())
	{
		app.status_line = template_status;
	}

	return true;
}
