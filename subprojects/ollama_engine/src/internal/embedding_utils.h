#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ollama_engine::internal
{

	/// <summary>Builds a normalized embedding vector from prompt text.</summary>
	/// <param name="pSText">Prompt or seed text.</param>
	/// <param name="piDimensions">Embedding dimensions.</param>
	/// <returns>Dense normalized float embedding.</returns>
	std::vector<float> BuildEmbedding(const std::string& pSText, std::size_t piDimensions);

} // namespace ollama_engine::internal
