#include "ollama_engine_cli_tests_internal.h"

#include "ollama_engine_cli_common_internal.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace ollama_engine_cli::tests
{

	std::vector<QuestionAnswerCase> LoadQuestionAnswerCases(const std::filesystem::path& pPathDirectory)
	{
		std::vector<QuestionAnswerCase> lVecQuestionAnswerCases;
		const std::regex lQuestionPattern("^Question_([0-9]+)\\.txt$", std::regex::icase);
		std::error_code lErrorCode;

		if (!std::filesystem::exists(pPathDirectory, lErrorCode) || !std::filesystem::is_directory(pPathDirectory, lErrorCode))
		{
			return lVecQuestionAnswerCases;
		}

		for (const std::filesystem::directory_entry& lDirectoryEntry : std::filesystem::directory_iterator(pPathDirectory, lErrorCode))
		{
			if (lErrorCode || !lDirectoryEntry.is_regular_file(lErrorCode))
			{
				continue;
			}

			const std::string lSFileName = lDirectoryEntry.path().filename().string();
			std::smatch lMatch;

			if (!std::regex_match(lSFileName, lMatch, lQuestionPattern) || lMatch.size() != 2)
			{
				continue;
			}

			const int liQuestionNumber = std::atoi(lMatch[1].str().c_str());

			if (liQuestionNumber <= 0)
			{
				continue;
			}

			const std::filesystem::path lPathAnswerFile = pPathDirectory / ("Question_" + std::to_string(liQuestionNumber) + "_Answer.txt");
			const std::filesystem::path lPathAnserFile = pPathDirectory / ("Question_" + std::to_string(liQuestionNumber) + "_Anser.txt");

			std::filesystem::path lPathResolvedAnswerFile;

			if (std::filesystem::exists(lPathAnswerFile, lErrorCode) && !lErrorCode)
			{
				lPathResolvedAnswerFile = lPathAnswerFile;
			}
			else if (std::filesystem::exists(lPathAnserFile, lErrorCode) && !lErrorCode)
			{
				lPathResolvedAnswerFile = lPathAnserFile;
			}
			else
			{
				continue;
			}

			const std::optional<std::string> lOptQuestionText = common::ReadTextFile(lDirectoryEntry.path());
			const std::optional<std::string> lOptAnswerText = common::ReadTextFile(lPathResolvedAnswerFile);

			if (!lOptQuestionText.has_value() || !lOptAnswerText.has_value())
			{
				continue;
			}

			QuestionAnswerCase lQuestionAnswerCase;
			lQuestionAnswerCase.piQuestionNumber = liQuestionNumber;
			lQuestionAnswerCase.pPathQuestionFile = lDirectoryEntry.path();
			lQuestionAnswerCase.pPathAnswerFile = lPathResolvedAnswerFile;
			lQuestionAnswerCase.pSQuestionText = *lOptQuestionText;
			lQuestionAnswerCase.pSAnswerText = *lOptAnswerText;
			lVecQuestionAnswerCases.push_back(std::move(lQuestionAnswerCase));
		}

		std::sort(lVecQuestionAnswerCases.begin(), lVecQuestionAnswerCases.end(), [](const QuestionAnswerCase& pLhs, const QuestionAnswerCase& pRhs) { return pLhs.piQuestionNumber < pRhs.piQuestionNumber; });
		return lVecQuestionAnswerCases;
	}

	bool RunQuestionAnswerTests(ollama_engine::EngineInterface* pPtrEngine, const std::filesystem::path& pPathDirectory)
	{
		const std::vector<QuestionAnswerCase> lVecQuestionAnswerCases = LoadQuestionAnswerCases(pPathDirectory);

		if (lVecQuestionAnswerCases.empty())
		{
			std::cout << "tests> no Question_N.txt + Question_N_Answer.txt pairs found in " << pPathDirectory << "\n";
			return false;
		}

		std::size_t liPassCount = 0;
		std::size_t liFailCount = 0;
		std::cout << "tests> running " << lVecQuestionAnswerCases.size() << " test case(s)\n";

		for (const QuestionAnswerCase& lQuestionAnswerCase : lVecQuestionAnswerCases)
		{
			const common::PromptRunResult lPromptRunResult = common::RunPromptWithMetrics(pPtrEngine, lQuestionAnswerCase.pSQuestionText);
			const std::string lSActual = common::NormalizeForMatch(lPromptRunResult.pSendMessageResponse.pSText);
			const std::string lSExpected = common::NormalizeForMatch(lQuestionAnswerCase.pSAnswerText);
			const bool lbPass = lPromptRunResult.pSendMessageResponse.pbOk && !lSExpected.empty() && (lSActual.find(lSExpected) != std::string::npos);

			std::cout << "tests> Question_" << lQuestionAnswerCase.piQuestionNumber << (lbPass ? " PASS" : " FAIL") << "\n";
			common::PrintMetrics(lPromptRunResult);

			if (!lbPass)
			{
				std::cout << "tests> expected~ " << common::Trim(lQuestionAnswerCase.pSAnswerText) << "\n";
				std::cout << "tests> actual~   " << common::Trim(lPromptRunResult.pSendMessageResponse.pSText) << "\n";
				++liFailCount;
			}
			else
			{
				++liPassCount;
			}
		}

		std::cout << "tests> summary: pass=" << liPassCount << " fail=" << liFailCount << "\n";
		return liFailCount == 0;
	}

	std::vector<BenchmarkProbe> BuildStandardOpenSourceBenchmarkProbes()
	{
		std::vector<BenchmarkProbe> lVecBenchmarkProbes;
		lVecBenchmarkProbes.push_back(BenchmarkProbe{"MMLU_style",
		                                             "Multiple choice: Which gas do plants absorb from the atmosphere? "
		                                             "A) Oxygen B) Nitrogen C) Carbon dioxide D) Helium. Answer with one letter.",
		                                             {"c", "carbon dioxide"}});
		lVecBenchmarkProbes.push_back(BenchmarkProbe{"GSM8K_style", "A store has 12 apples and sells 5, then receives 9 more. How many apples are in the store now?", {"16"}});
		lVecBenchmarkProbes.push_back(BenchmarkProbe{"ARC_Easy_style", "Which source of energy is renewable? A) Coal B) Solar C) Oil D) Natural gas. Answer with one letter.", {"b", "solar"}});
		lVecBenchmarkProbes.push_back(BenchmarkProbe{"TruthfulQA_style", "Can humans breathe in outer space without a spacesuit? Answer briefly and factually.", {"no", "cannot", "can't", "cannot breathe"}});
		return lVecBenchmarkProbes;
	}

	bool IsBenchmarkProbePass(const BenchmarkProbe& pBenchmarkProbe, const std::string& pSTextResponse)
	{
		if (pBenchmarkProbe.pVecSExpectedContainsAny.empty())
		{
			return !common::Trim(pSTextResponse).empty();
		}

		const std::string lSNormalizedActual = common::NormalizeForMatch(pSTextResponse);

		for (const std::string& lSExpectedHint : pBenchmarkProbe.pVecSExpectedContainsAny)
		{
			const std::string lSNormalizedExpected = common::NormalizeForMatch(lSExpectedHint);

			if (!lSNormalizedExpected.empty() && lSNormalizedActual.find(lSNormalizedExpected) != std::string::npos)
			{
				return true;
			}
		}

		return false;
	}

	void RunStandardOpenSourceBenchmarks(ollama_engine::EngineInterface* pPtrEngine)
	{
		const std::vector<BenchmarkProbe> lVecBenchmarkProbes = BuildStandardOpenSourceBenchmarkProbes();

		if (lVecBenchmarkProbes.empty())
		{
			return;
		}

		std::cout << "bench> running " << lVecBenchmarkProbes.size() << " standard open-source benchmark probes (mini, non-official)\n";
		double ldTotalMs = 0.0;
		double ldTotalTtftMs = 0.0;
		double ldTotalTokPerSec = 0.0;

		for (const BenchmarkProbe& lBenchmarkProbe : lVecBenchmarkProbes)
		{
			const common::PromptRunResult lPromptRunResult = common::RunPromptWithMetrics(pPtrEngine, lBenchmarkProbe.pSPrompt);
			std::cout << "bench> " << lBenchmarkProbe.pSName << "\n";

			if (!lPromptRunResult.pSendMessageResponse.pbOk)
			{
				std::cout << "bench> error: " << lPromptRunResult.pSendMessageResponse.pSError << "\n";
				continue;
			}

			common::PrintMetrics(lPromptRunResult);
			ldTotalMs += lPromptRunResult.pdTotalMilliseconds;
			ldTotalTtftMs += lPromptRunResult.pdTimeToFirstTokenMilliseconds;
			ldTotalTokPerSec += lPromptRunResult.pdTokensPerSecond;
		}

		const double ldCount = static_cast<double>(lVecBenchmarkProbes.size());

		if (ldCount > 0.0)
		{
			std::cout << "bench> avg_total_ms=" << (ldTotalMs / ldCount) << " avg_ttft_ms=" << (ldTotalTtftMs / ldCount) << " avg_tok/s=" << (ldTotalTokPerSec / ldCount) << "\n";
		}
	}

} // namespace ollama_engine_cli::tests
