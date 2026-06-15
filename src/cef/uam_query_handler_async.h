#pragma once
// Async bridge infrastructure for handler files that offload work to background threads.
// Include this header (after uam_query_handler.h) in handler files that call RunAsyncCefQuery.

#include "cef/uam_query_handler.h"

#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace uam::query_handler_async
{

struct AsyncCefResult
{
	bool ok = true;
	int status = 500;
	std::string body;
	std::string error;
};

inline AsyncCefResult AsyncSuccess(nlohmann::json body)
{
	return {true, 200, body.dump(), ""};
}

inline AsyncCefResult AsyncFailure(int status, std::string error)
{
	return {false, status, "", std::move(error)};
}

class CefQueryCallbackTask : public CefTask
{
  public:
	CefQueryCallbackTask(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, AsyncCefResult result)
	    : m_callback(std::move(callback)), m_result(std::move(result))
	{
	}

	void Execute() override
	{
		CEF_REQUIRE_UI_THREAD();
		if (!m_callback) return;
		if (m_result.ok)
		{
			m_callback->Success(m_result.body);
		}
		else
		{
			m_callback->Failure(m_result.status, m_result.error);
		}
	}

  private:
	CefRefPtr<CefMessageRouterBrowserSide::Callback> m_callback;
	AsyncCefResult m_result;
	IMPLEMENT_REFCOUNTING(CefQueryCallbackTask);
};

template <typename Worker>
void RunAsyncCefQuery(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker)
{
	std::thread(
	    [callback = std::move(callback), worker = std::move(worker)]() mutable
	    {
		    AsyncCefResult result;
		    try
		    {
			    result = worker();
		    }
		    catch (const std::exception& ex)
		    {
			    result = AsyncFailure(500, ex.what());
		    }
		    catch (...)
		    {
			    result = AsyncFailure(500, "Async bridge request failed.");
		    }
		    CefPostTask(TID_UI, new CefQueryCallbackTask(callback, std::move(result)));
	    })
	    .detach();
}

} // namespace uam::query_handler_async
