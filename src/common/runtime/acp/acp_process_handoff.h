#pragma once

#include "common/platform/platform_state_fields.h"
#include <memory>
#include <utility>
#include <vector>

namespace uam
{
	void TransferStdioProcessFields(platform::StdioProcessPlatformFields& source, platform::StdioProcessPlatformFields& destination);

	namespace acp_detail
	{
		// The destination must be empty and exclusively owned. Until worker creation or publication
		// succeeds, unwinding returns the actual fields to their original owner.
		class ScopedStdioProcessTransfer
		{
			platform::StdioProcessPlatformFields& source;
			platform::StdioProcessPlatformFields& destination;
			bool committed = false;

		  public:
			ScopedStdioProcessTransfer(platform::StdioProcessPlatformFields& from, platform::StdioProcessPlatformFields& to)
			    : source(from), destination(to)
			{
				TransferStdioProcessFields(source, destination);
			}
			ScopedStdioProcessTransfer(const ScopedStdioProcessTransfer&) = delete;
			ScopedStdioProcessTransfer& operator=(const ScopedStdioProcessTransfer&) = delete;
			~ScopedStdioProcessTransfer() noexcept
			{
				if (!committed) TransferStdioProcessFields(destination, source);
			}
			void Commit() noexcept
			{
				committed = true;
			}
		};

		template <typename Pending, typename Allocator>
		void PublishStdioProcessTransfer(platform::StdioProcessPlatformFields& source,
		                                 std::unique_ptr<Pending> pending,
		                                 std::vector<std::unique_ptr<Pending>, Allocator>& destination)
		{
			// unique_ptr moves cannot throw: vector allocation failure leaves pending
			// alive here, so the guard restores the original fields before it dies.
			ScopedStdioProcessTransfer handoff(source, *pending);
			destination.push_back(std::move(pending));
			handoff.Commit();
		}
	}
}
