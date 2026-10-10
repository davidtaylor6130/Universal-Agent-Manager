#include "uam_query_handler.h"
#include "app/organization_service.h"
#include "cef/cef_push.h"

void UamQueryHandler::HandleOrganizationAction(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string error;
	if (!uam::ApplyOrganizationAction(m_app, payload, &error))
	{
		cb->Failure(409, error.empty() ? "Organization change could not be applied." : error);
		return;
	}
	cb->Success("{}");
	uam::PushStateUpdateIfChanged(browser, m_app);
}
