#pragma once

#include "vectorised_rag.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ollama_engine::internal::vectorised_rag
{
	namespace fs = std::filesystem;

	inline std::string ToLowerAscii(std::string pSValue)
	{
		std::transform(pSValue.begin(), pSValue.end(), pSValue.begin(), [](const unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });
		return pSValue;
	}

	inline bool StartsWith(const std::string& pSValue, const std::string& pSPrefix)
	{
		return pSValue.size() >= pSPrefix.size() && pSValue.compare(0, pSPrefix.size(), pSPrefix) == 0;
	}

	inline bool EndsWithIgnoreCase(const std::string& pSValue, const std::string& pSSuffix)
	{
		if (pSValue.size() < pSSuffix.size())
		{
			return false;
		}

		const std::size_t liOffset = pSValue.size() - pSSuffix.size();

		for (std::size_t liIndex = 0; liIndex < pSSuffix.size(); ++liIndex)
		{
			if (static_cast<char>(std::tolower(static_cast<unsigned char>(pSValue[liOffset + liIndex]))) != static_cast<char>(std::tolower(static_cast<unsigned char>(pSSuffix[liIndex]))))
			{
				return false;
			}
		}

		return true;
	}

	inline std::string TrimAscii(const std::string& pSValue)
	{
		std::size_t liBegin = 0;

		while (liBegin < pSValue.size() && std::isspace(static_cast<unsigned char>(pSValue[liBegin])) != 0)
		{
			++liBegin;
		}

		std::size_t liEnd = pSValue.size();

		while (liEnd > liBegin && std::isspace(static_cast<unsigned char>(pSValue[liEnd - 1])) != 0)
		{
			--liEnd;
		}

		return pSValue.substr(liBegin, liEnd - liBegin);
	}

	inline bool IsAllowedDatabaseNameCharacter(const char pCChar)
	{
		const unsigned char lCUChar = static_cast<unsigned char>(pCChar);
		return std::isalnum(lCUChar) != 0 || pCChar == '_' || pCChar == '-' || pCChar == '.';
	}

	inline std::string SanitizeDatabaseName(const std::string& pSDatabaseName)
	{
		std::string lSDatabaseName = TrimAscii(pSDatabaseName);

		if (EndsWithIgnoreCase(lSDatabaseName, ".sqlite3"))
		{
			lSDatabaseName.resize(lSDatabaseName.size() - std::string(".sqlite3").size());
		}

		std::string lSSanitized;
		lSSanitized.reserve(lSDatabaseName.size());

		for (const char lCChar : lSDatabaseName)
		{
			if (IsAllowedDatabaseNameCharacter(lCChar))
			{
				lSSanitized.push_back(lCChar);
			}
			else
			{
				lSSanitized.push_back('_');
			}
		}

		while (!lSSanitized.empty() && (lSSanitized.back() == '.' || lSSanitized.back() == '_' || lSSanitized.back() == '-'))
		{
			lSSanitized.pop_back();
		}

		return lSSanitized;
	}

	inline std::string ShellQuote(const std::string& pSValue)
	{
		std::string lSQuoted = "'";

		for (const char lCChar : pSValue)
		{
			if (lCChar == '\'')
			{
				lSQuoted += "'\"'\"'";
			}
			else
			{
				lSQuoted.push_back(lCChar);
			}
		}

		lSQuoted.push_back('\'');
		return lSQuoted;
	}

	inline int RunShellCommand(const std::string& pSCommand, std::string* pSOutput = nullptr)
	{
		std::array<char, 4096> lArrCBuffer{};
		const std::string lSFullCommand = pSCommand + " 2>&1";
		FILE* lPtrPipe = popen(lSFullCommand.c_str(), "r");

		if (lPtrPipe == nullptr)
		{
			return -1;
		}

		std::string lSOutput;

		while (std::fgets(lArrCBuffer.data(), static_cast<int>(lArrCBuffer.size()), lPtrPipe) != nullptr)
		{
			lSOutput += lArrCBuffer.data();
		}

		const int liExitCode = pclose(lPtrPipe);

		if (pSOutput != nullptr)
		{
			*pSOutput = std::move(lSOutput);
		}

		return liExitCode;
	}

	inline std::string NormalizePathKey(const fs::path& pPathValue)
	{
		std::error_code lErrorCode;
		const fs::path lPathAbsolute = fs::absolute(pPathValue, lErrorCode);
		const fs::path lPathNormalized = (lErrorCode ? pPathValue.lexically_normal() : lPathAbsolute.lexically_normal());
		return lPathNormalized.generic_string();
	}

	inline bool HasPathSeparator(const std::string& pSValue)
	{
		return pSValue.find('/') != std::string::npos || pSValue.find('\\') != std::string::npos;
	}

	inline std::vector<std::string> SplitLines(const std::string& pSText)
	{
		std::vector<std::string> lVecSLines;
		std::stringstream lStream(pSText);
		std::string lSLine;

		while (std::getline(lStream, lSLine))
		{
			lVecSLines.push_back(lSLine);
		}

		if (!pSText.empty() && pSText.back() == '\n')
		{
			lVecSLines.push_back("");
		}

		return lVecSLines;
	}

	inline std::string JoinLines(const std::vector<std::string>& pVecSLines, const std::size_t piBegin, const std::size_t piEnd)
	{
		std::string lSOut;

		for (std::size_t liIndex = piBegin; liIndex < piEnd; ++liIndex)
		{
			if (!lSOut.empty())
			{
				lSOut.push_back('\n');
			}

			lSOut += pVecSLines[liIndex];
		}

		return lSOut;
	}

	inline std::size_t CountNonEmptyLines(const std::string& pSText)
	{
		if (pSText.empty())
		{
			return 0;
		}

		std::istringstream lStream(pSText);
		std::string lSLine;
		std::size_t liCount = 0;

		while (std::getline(lStream, lSLine))
		{
			if (!TrimAscii(lSLine).empty())
			{
				++liCount;
			}
		}

		return liCount;
	}

	inline std::string BuildOverviewText(const std::string& pSRawText, const int piMaxLines, const std::size_t piMaxChars)
	{
		const std::vector<std::string> lVecSLines = SplitLines(pSRawText);
		const std::size_t liLineCap = std::min<std::size_t>(lVecSLines.size(), static_cast<std::size_t>(std::max(1, piMaxLines)));
		std::string lSResult = JoinLines(lVecSLines, 0, liLineCap);

		if (lSResult.size() > piMaxChars)
		{
			lSResult = lSResult.substr(0, piMaxChars);
		}

		return TrimAscii(lSResult);
	}

} // namespace ollama_engine::internal::vectorised_rag
