#pragma once

#include "vectorised_rag_sqlite_persistence.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ollama_engine::internal::vectorised_rag
{
	namespace fs = std::filesystem;

	inline double CosineSimilarity(const std::vector<float>& pVecfLhs, const std::vector<float>& pVecfRhs)
	{
		if (pVecfLhs.empty() || pVecfRhs.empty() || pVecfLhs.size() != pVecfRhs.size())
		{
			return 0.0;
		}

		double ldDot = 0.0;
		double ldNormLhs = 0.0;
		double ldNormRhs = 0.0;

		for (std::size_t liIndex = 0; liIndex < pVecfLhs.size(); ++liIndex)
		{
			const double ldLhs = pVecfLhs[liIndex];
			const double ldRhs = pVecfRhs[liIndex];
			ldDot += (ldLhs * ldRhs);
			ldNormLhs += (ldLhs * ldLhs);
			ldNormRhs += (ldRhs * ldRhs);
		}

		if (ldNormLhs <= 0.0 || ldNormRhs <= 0.0)
		{
			return 0.0;
		}

		return ldDot / (std::sqrt(ldNormLhs) * std::sqrt(ldNormRhs));
	}

	inline double ChunkTypeScoreBias(const std::string& pSChunkType)
	{
		if (pSChunkType == "function" || pSChunkType == "method")
		{
			return 0.08;
		}

		if (pSChunkType == "class_overview" || pSChunkType == "struct_overview")
		{
			return 0.04;
		}

		if (pSChunkType == "enum")
		{
			return 0.02;
		}

		if (pSChunkType == "namespace")
		{
			return -0.12;
		}

		if (pSChunkType == "global_block")
		{
			return -0.16;
		}

		return 0.0;
	}

	inline double RichnessScoreBonus(const std::string& pSRawText)
	{
		const std::string lSTrimmed = TrimAscii(pSRawText);

		if (lSTrimmed.empty())
		{
			return -0.2;
		}

		const std::size_t liLineCount = CountNonEmptyLines(lSTrimmed);
		const std::size_t liCharCount = lSTrimmed.size();
		double ldBonus = 0.0;

		if (liLineCount >= 3)
		{
			ldBonus += std::min<double>(0.06, static_cast<double>(liLineCount - 2) * 0.01);
		}

		if (liCharCount >= 160)
		{
			ldBonus += std::min<double>(0.06, static_cast<double>(liCharCount - 160) / 1400.0);
		}

		return ldBonus;
	}

	inline bool IsLowInformationChunk(const std::string& pSChunkType, const std::string& pSRawText)
	{
		const std::string lSTrimmed = TrimAscii(pSRawText);

		if (lSTrimmed.empty())
		{
			return true;
		}

		if (pSChunkType == "function" || pSChunkType == "method")
		{
			return false;
		}

		const std::size_t liLineCount = CountNonEmptyLines(lSTrimmed);
		const std::size_t liCharCount = lSTrimmed.size();

		if (pSChunkType == "namespace" || pSChunkType == "global_block")
		{
			return liLineCount < 3 || liCharCount < 180;
		}

		if (pSChunkType == "class_overview" || pSChunkType == "struct_overview")
		{
			return liLineCount < 2 || liCharCount < 140;
		}

		if (pSChunkType == "enum")
		{
			return liLineCount < 2 || liCharCount < 100;
		}

		return liLineCount < 2 || liCharCount < 100;
	}

	inline std::string FormatSnippet(const std::string& pSSourceId, const std::string& pSFilePath, const int piStartLine, const int piEndLine, const std::string& pSChunkType, const std::string& pSSymbolName, const std::string& pSParentSymbol, std::string pSRawText)
	{
		constexpr std::size_t kiMaxSnippetChars = 1800;

		if (pSRawText.size() > kiMaxSnippetChars)
		{
			pSRawText = pSRawText.substr(0, kiMaxSnippetChars) + "\n// ... truncated ...";
		}

		std::ostringstream lStream;

		if (!pSSourceId.empty())
		{
			lStream << "[" << pSSourceId << "] ";
		}

		lStream << pSFilePath << ":" << piStartLine << "-" << piEndLine << " [" << pSChunkType;

		if (!pSSymbolName.empty())
		{
			lStream << " " << pSSymbolName;
		}

		if (!pSParentSymbol.empty())
		{
			lStream << " parent=" << pSParentSymbol;
		}

		lStream << "]\n" << pSRawText;
		return lStream.str();
	}

} // namespace ollama_engine::internal::vectorised_rag
