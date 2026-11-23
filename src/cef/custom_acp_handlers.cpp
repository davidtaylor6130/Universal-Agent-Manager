#include "cef/uam_query_handler.h"

#include "cef/cef_push.h"
#include "cef/uam_query_handler_internal.h"
#include "common/provider/custom_acp/custom_acp_config.h"
#include "common/provider/custom_acp/custom_acp_provider_runtime.h"

#include <nlohmann/json.hpp>

#include <string>

// Settings form for the opt-in Custom ACP agent (#523). These desktop bridge actions are the only writers of its
// configuration; nothing a repository, chat or phone sends reaches them.
namespace
{
	nlohmann::json CustomAcpAgentView(const uam::custom_acp::AgentConfig& config)
	{
		nlohmann::json view = uam::custom_acp::ToJson(config);
		view.erase("version");
		view.erase("approvedFingerprint");
		view["configured"] = uam::custom_acp::IsConfigured(config);
		view["name"] = uam::custom_acp::DisplayName(config);
		view["displayCommand"] = uam::custom_acp::DisplayCommand(config);
		view["fingerprint"] = uam::custom_acp::LaunchFingerprint(config);
		view["allowed"] = uam::custom_acp::IsApproved(config);
		return view;
	}

	void ReplyWithView(CefRefPtr<CefMessageRouterBrowserSide::Callback> cb, const uam::custom_acp::AgentConfig& config)
	{
		cb->Success(CustomAcpAgentView(config).dump());
	}
} // namespace

void UamQueryHandler::HandleGetCustomAcpAgent(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	ReplyWithView(cb, uam::custom_acp::ActiveConfig());
}

void UamQueryHandler::HandleSetCustomAcpAgent(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.is_object())
	{
		cb->Failure(400, "The custom ACP agent settings are not valid.");
		return;
	}
	nlohmann::json saved = nlohmann::json::object();
	saved["version"] = 1;
	for (const char* field : {"displayName", "command", "arguments", "environment"})
		if (payload.contains(field)) saved[field] = payload[field];
	uam::custom_acp::AgentConfig next;
	std::string error;
	if (!uam::custom_acp::FromJson(saved, next, &error))
	{
		cb->Failure(400, error);
		return;
	}
	const uam::custom_acp::AgentConfig previous = uam::custom_acp::ActiveConfig();
	// Any change to what would be launched needs the user to see and allow the exact command again.
	next = uam::custom_acp::WithApprovalCarriedOver(std::move(next), previous);
	if (!uam::custom_acp::Save(m_app.data_root, next, &error))
	{
		cb->Failure(500, error);
		return;
	}
	ApplyCustomAcpConfiguration(m_app.provider_profiles, next);
	uam::PushStateUpdateIfChanged(browser, m_app);
	ReplyWithView(cb, next);
}

void UamQueryHandler::HandleAllowCustomAcpAgent(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	uam::custom_acp::AgentConfig config = uam::custom_acp::ActiveConfig();
	if (!uam::custom_acp::IsConfigured(config))
	{
		cb->Failure(400, "Set a command for the custom ACP agent before allowing it.");
		return;
	}
	// The fingerprint names the command the user was shown, so a launch changed elsewhere is never allowed unseen.
	const std::string shown = payload.is_object() && payload.contains("fingerprint") && payload["fingerprint"].is_string() ? payload["fingerprint"].get<std::string>() : std::string{};
	if (shown != uam::custom_acp::LaunchFingerprint(config))
	{
		cb->Failure(409, "The custom ACP agent command changed. Review it again before allowing it.");
		return;
	}
	config.approved_fingerprint = shown;
	std::string error;
	if (!uam::custom_acp::Save(m_app.data_root, config, &error))
	{
		cb->Failure(500, error);
		return;
	}
	ApplyCustomAcpConfiguration(m_app.provider_profiles, config);
	uam::PushStateUpdateIfChanged(browser, m_app);
	ReplyWithView(cb, config);
}
