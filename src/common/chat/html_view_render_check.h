#pragma once

#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Runtime check for agent HTML views: the page runs once in a hidden browser, under the
/// same sandbox as its visible card, and its console errors go back in the tool result.
namespace uam::html_view
{
	/// The limits below are interim defaults (open decision, documentation-review.md).
	/// Well inside the 20 seconds the UAM tool proxy waits for one of these replies.
	inline constexpr int kRenderCheckTimeoutMs = 5000;
	/// Time the page gets to run after it is delivered, so errors from short timers are seen too.
	inline constexpr int kRenderCheckSettleMs = 1000;
	/// Hidden browsers alive at once. Each runs an agent's page, so publishes past this limit skip the check.
	inline constexpr std::size_t kMaxRenderChecks = 3;
	inline constexpr std::size_t kMaxRenderCheckErrors = 20;
	/// Console messages of any level before the check stops, so a page logging in a loop cannot keep loading the UI thread.
	inline constexpr std::size_t kMaxRenderCheckConsoleMessages = 2000;
	/// Keeps the reply inside the 64 KiB control response limit even when every byte needs a JSON escape.
	inline constexpr std::size_t kMaxRenderCheckErrorBytes = 512;

	enum class RenderCheckStatus
	{
		Completed,
		TimedOut,
		NotRun,
	};

	struct RenderCheckError
	{
		std::string message;
		int line = 0;
	};

	struct RenderCheckResult
	{
		RenderCheckStatus status = RenderCheckStatus::NotRun;
		std::vector<RenderCheckError> errors;
		std::size_t omitted_errors = 0;
		std::string not_run_reason;
	};

	/// <summary>Records one page error, keeping the count and the text bounded.</summary>
	inline void AddRenderCheckError(RenderCheckResult& result, std::string_view message, int line)
	{
		if (result.errors.size() == kMaxRenderCheckErrors)
		{
			++result.omitted_errors;
			return;
		}
		std::string text(message);
		for (char& ch : text)
			if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
		result.errors.push_back({uam::strings::SafeLine(text, kMaxRenderCheckErrorBytes, true), line});
	}

	/// <summary>The `runtimeCheck` object an agent receives. `completed` is false whenever the check did not finish.</summary>
	inline nlohmann::json RenderCheckJson(const RenderCheckResult& result)
	{
		nlohmann::json errors = nlohmann::json::array();
		for (const RenderCheckError& error : result.errors)
			errors.push_back({{"message", error.message}, {"line", error.line}});
		const std::size_t total = result.errors.size() + result.omitted_errors;
		std::string status = "not_run";
		std::string summary = "The runtime check did not complete: " + result.not_run_reason;
		if (result.status == RenderCheckStatus::Completed)
		{
			status = "completed";
			summary = total == 0 ? "The page rendered in a hidden sandbox and reported no runtime errors."
			                     : "The page rendered in a hidden sandbox and reported " + std::to_string(total) + " runtime error" + (total == 1 ? "." : "s.");
		}
		else if (result.status == RenderCheckStatus::TimedOut)
		{
			status = "timed_out";
			summary = "The runtime check did not complete: the page did not finish within " + std::to_string(kRenderCheckTimeoutMs / 1000) +
			          " seconds. It may be stuck in a loop, or an earlier stuck page may still hold the renderer. Any errors listed were seen before the limit.";
		}
		return {{"status", std::move(status)},
		        {"completed", result.status == RenderCheckStatus::Completed},
		        {"errors", std::move(errors)},
		        {"omittedErrors", result.omitted_errors},
		        {"summary", std::move(summary)}};
	}

	using RenderCheckDone = std::function<void(RenderCheckResult)>;

	/// Implemented by the desktop app's hidden browser. Every call happens on the UI thread.
	class RenderChecker
	{
	  public:
		virtual ~RenderChecker() = default;
		/// <summary>Runs the check without blocking. `done` runs exactly once, possibly before this returns.</summary>
		virtual void Start(std::string owner, std::string html, RenderCheckDone done) = 0;
		/// <summary>Ends and destroys the checks started for `owner`, or every check when `owner` is empty.</summary>
		virtual void Cancel(std::string_view owner) = 0;
		/// <summary>True once every hidden browser has been destroyed.</summary>
		virtual bool Idle() const = 0;
	};
} // namespace uam::html_view
