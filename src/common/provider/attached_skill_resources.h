#pragma once

#include "common/models/app_models.h"

#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uam::attached_skills
{
	/// <summary>Identifies captured skill bytes inside the immutable logical prompt.</summary>
	struct Span
	{
		std::size_t offset = 0;
		std::size_t length = 0;
		std::string digest;
		std::string title;
		bool operator==(const Span&) const = default;
	};

	Span Capture(std::size_t offset, std::string_view captured_block);
	void Shift(std::vector<Span>& spans, std::size_t amount);
	bool Validate(std::string_view prompt, const std::vector<Span>& spans, std::string& error);
	/// <summary>Publishes captured bytes only, then renders ordinary native file references.</summary>
	bool Prepare(const std::filesystem::path& data_root, const ExecutionHost& host,
	             const std::string& workspace, const std::string& prompt,
	             const std::vector<Span>& spans, std::string& wire_prompt,
	             std::string& error, std::stop_token stop = {});
}
