#pragma once
// Async bridge infrastructure for handler files that offload work to background threads.
// Include this header (after uam_query_handler.h) in handler files that call RunAsyncCefQuery.

#include "cef/uam_query_handler.h"

#include "include/cef_task.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace uam::query_handler_async
{

struct AsyncCefResult
{
	bool ok = true;
	int status = 500;
	std::string body;
	std::string error;
	// A completion may hand the callback to a second asynchronous query.
	bool callback_deferred = false;
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
	CefQueryCallbackTask(std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
	                     AsyncCefResult result,
	                     std::function<void(AsyncCefResult&)> completion = {})
	    : m_lifetime(std::move(lifetime)), m_callback(std::move(callback)), m_result(std::move(result)), m_completion(std::move(completion))
	{
	}

	void Execute() override
	{
		if (m_lifetime.expired()) return;
		CEF_REQUIRE_UI_THREAD();
		if (m_completion)
		{
			try
			{
				m_completion(m_result);
			}
			catch (const std::exception& ex)
			{
				if (!m_result.callback_deferred) m_result = AsyncFailure(500, ex.what());
			}
			catch (...)
			{
				if (!m_result.callback_deferred) m_result = AsyncFailure(500, "Async bridge completion failed.");
			}
		}
		if (!m_callback || m_result.callback_deferred) return;
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
	std::weak_ptr<void> m_lifetime;
	CefRefPtr<CefMessageRouterBrowserSide::Callback> m_callback;
	AsyncCefResult m_result;
	std::function<void(AsyncCefResult&)> m_completion;
	IMPLEMENT_REFCOUNTING(CefQueryCallbackTask);
};

class CefQueryWorkerTask : public CefTask
{
  public:
	CefQueryWorkerTask(std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
	                   std::function<AsyncCefResult()> worker,
	                   std::function<void(AsyncCefResult&)> completion = {})
	    : m_lifetime(std::move(lifetime)), m_callback(std::move(callback)), m_worker(std::move(worker)), m_completion(std::move(completion))
	{
	}

	void Execute() override
	{
		if (m_lifetime.expired()) return;
		AsyncCefResult result;
		try
		{
			result = m_worker();
		}
		catch (const std::exception& ex)
		{
			result = AsyncFailure(500, ex.what());
		}
		catch (...)
		{
			result = AsyncFailure(500, "Async bridge request failed.");
		}
		(void)CefPostTask(TID_UI, new CefQueryCallbackTask(m_lifetime, m_callback, std::move(result), std::move(m_completion)));
	}

  private:
	std::weak_ptr<void> m_lifetime;
	CefRefPtr<CefMessageRouterBrowserSide::Callback> m_callback;
	std::function<AsyncCefResult()> m_worker;
	std::function<void(AsyncCefResult&)> m_completion;
	IMPLEMENT_REFCOUNTING(CefQueryWorkerTask);
};

template <typename Worker>
bool RunAsyncCefQuery(std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker)
{
	if (!CefPostTask(TID_FILE_BACKGROUND, new CefQueryWorkerTask(std::move(lifetime), callback, std::move(worker))))
	{
		callback->Failure(503, "The background task queue is unavailable.");
		return false;
	}
	return true;
}

template <typename Worker, typename Completion>
bool RunAsyncCefQuery(std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker, Completion completion)
{
	if (!CefPostTask(TID_FILE_BACKGROUND,
	                 new CefQueryWorkerTask(std::move(lifetime), callback, std::move(worker), std::move(completion))))
	{
		callback->Failure(503, "The background task queue is unavailable.");
		return false;
	}
	return true;
}

} // namespace uam::query_handler_async
