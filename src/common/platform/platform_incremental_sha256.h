#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace uam::platform
{
	/// Incremental native SHA-256 only; no file IO, media store or execution authority.
	class IncrementalSha256 final
	{
	public:
		IncrementalSha256();
		~IncrementalSha256();
		IncrementalSha256(const IncrementalSha256&) = delete;
		IncrementalSha256& operator=(const IncrementalSha256&) = delete;
		bool Add(std::string_view bytes);
		std::string Finish();
	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
