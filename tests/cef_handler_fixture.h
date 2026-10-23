#pragma once

#include "test_harness.h"
#include "cef/uam_query_handler.h"

// For unrelated CEF handler tests only. Usage/allowance tests must inject their own
// services; invoking a usage route here would leave this isolation boundary.
struct UamQueryHandlerTestAccess
{
	static std::unique_ptr<UamQueryHandler> Create(uam::AppState& app, std::string trusted_ui_index_url)
	{
		auto handler = std::unique_ptr<UamQueryHandler>(new UamQueryHandler(app, std::move(trusted_ui_index_url), false));
		UAM_ASSERT(!handler->m_usageHistory);
		UAM_ASSERT(!handler->m_providerAllowances);
		return handler;
	}
};
