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
	CefQueryCallbackTask(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
	                     AsyncCefResult result,
	                     std::function<void(AsyncCefResult&)> completion = {})
	    : m_callback(std::move(callback)), m_result(std::move(result)), m_completion(std::move(completion))
	{
	}

	void Execute() override
	{
		CEF_REQUIRE_UI_THREAD();
		if (m_completion)
		{
			try
			{
				m_completion(m_result);
			}
			catch (const std::exception& ex)
			{
				m_result = AsyncFailure(500, ex.what());
			}
			catch (...)
			{
				m_result = AsyncFailure(500, "Async bridge completion failed.");
			}
		}
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
	std::function<void(AsyncCefResult&)> m_completion;
	IMPLEMENT_REFCOUNTING(CefQueryCallbackTask);
};

class CefQueryWorkerTask : public CefTask
{
  public:
	CefQueryWorkerTask(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
	                   std::function<AsyncCefResult()> worker,
	                   std::function<void(AsyncCefResult&)> completion = {})
	    : m_callback(std::move(callback)), m_worker(std::move(worker)), m_completion(std::move(completion))
	{
	}

	void Execute() override
	{
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
		(void)CefPostTask(TID_UI, new CefQueryCallbackTask(m_callback, std::move(result), std::move(m_completion)));
	}

  private:
	CefRefPtr<CefMessageRouterBrowserSide::Callback> m_callback;
	std::function<AsyncCefResult()> m_worker;
	std::function<void(AsyncCefResult&)> m_completion;
	IMPLEMENT_REFCOUNTING(CefQueryWorkerTask);
};

template <typename Worker>
void RunAsyncCefQuery(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker)
{
	if (!CefPostTask(TID_FILE_BACKGROUND, new CefQueryWorkerTask(callback, std::move(worker))))
	{
		callback->Failure(503, "The background task queue is unavailable.");
	}
}

template <typename Worker, typename Completion>
void RunAsyncCefQuery(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker, Completion completion)
{
	if (!CefPostTask(TID_FILE_BACKGROUND,
	                 new CefQueryWorkerTask(callback, std::move(worker), std::move(completion))))
	{
		callback->Failure(503, "The background task queue is unavailable.");
	}
}

} // namespace uam::query_handler_async
