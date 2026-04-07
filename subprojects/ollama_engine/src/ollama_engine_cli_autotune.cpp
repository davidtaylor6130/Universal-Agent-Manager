#include "ollama_engine_cli_autotune_internal.h"

#include "ollama_engine_cli_common_internal.h"
#include "ollama_engine_cli_tests_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{

	enum class AutoTuneEvaluationMode
	{
		BuiltInBenchmarks,
		QuestionAnswerFolder
	};

	struct AutoTuneWizardConfig
	{
		std::string pSModelName;
		AutoTuneEvaluationMode pAutoTuneEvaluationMode = AutoTuneEvaluationMode::BuiltInBenchmarks;
		std::filesystem::path pPathQuestionAnswerFolder;
		int piRunsPerSetting = 3;
		double pdTargetTokensPerSecond = 0.0;
		double pdTargetLatencyMilliseconds = 0.0;
	};

	struct AutoTuneEvaluationSummary
	{
		bool pbOk = false;
		std::size_t piPassCount = 0;
		std::size_t piCaseCount = 0;
		double pdAverageScore = 0.0;
		double pdAverageTokensPerSecond = 0.0;
		double pdAverageTotalMilliseconds = 0.0;
		double pdAverageTtftMilliseconds = 0.0;
		std::string pSError;
	};

	int ParseIntOrDefault(const std::string& pSInput, const int piDefaultValue)
	{
		const std::string lSTrimmed = ollama_engine_cli::common::Trim(pSInput);

		if (lSTrimmed.empty())
		{
			return piDefaultValue;
		}

		char* lPtrEnd = nullptr;
		const long liValue = std::strtol(lSTrimmed.c_str(), &lPtrEnd, 10);

		if (lPtrEnd == nullptr || *lPtrEnd != '\0')
		{
			return piDefaultValue;
		}

		return static_cast<int>(liValue);
	}

	std::uint32_t ParseUnsigned32OrDefault(const std::string& pSInput, const std::uint32_t piDefaultValue)
	{
		const std::string lSTrimmed = ollama_engine_cli::common::Trim(pSInput);

		if (lSTrimmed.empty())
		{
			return piDefaultValue;
		}

		char* lPtrEnd = nullptr;
		const unsigned long long lliValue = std::strtoull(lSTrimmed.c_str(), &lPtrEnd, 10);

		if (lPtrEnd == nullptr || *lPtrEnd != '\0')
		{
			return piDefaultValue;
		}

		if (lliValue > static_cast<unsigned long long>(std::numeric_limits<std::uint32_t>::max()))
		{
			return piDefaultValue;
		}

		return static_cast<std::uint32_t>(lliValue);
	}

	double ParseDoubleOrDefault(const std::string& pSInput, const double pdDefaultValue)
	{
		const std::string lSTrimmed = ollama_engine_cli::common::Trim(pSInput);

		if (lSTrimmed.empty())
		{
			return pdDefaultValue;
		}

		char* lPtrEnd = nullptr;
		const double ldValue = std::strtod(lSTrimmed.c_str(), &lPtrEnd);

		if (lPtrEnd == nullptr || *lPtrEnd != '\0')
		{
			return pdDefaultValue;
		}

		return ldValue;
	}

	bool ReadYesNo(const std::string& pSPrompt, const bool pbDefault)
	{
		const std::string lSInput = ollama_engine_cli::common::Trim(ollama_engine_cli::common::ReadLine(pSPrompt));

		if (lSInput.empty())
		{
			return pbDefault;
		}

		const std::string lSNormalized = ollama_engine_cli::common::NormalizeForMatch(lSInput);

		if (lSNormalized == "y" || lSNormalized == "yes" || lSNormalized == "true")
		{
			return true;
		}

		if (lSNormalized == "n" || lSNormalized == "no" || lSNormalized == "false")
		{
			return false;
		}

		return pbDefault;
	}

	std::string GetOsLabel()
	{
#if defined(_WIN32)
		return "Windows";
#elif defined(__APPLE__)
		return "macOS";
#elif defined(__linux__)
		return "Linux";
#else
		return "UnknownOS";
#endif
	}

	std::string GetArchLabel()
	{
#if defined(__aarch64__) || defined(_M_ARM64)
		return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
		return "x64";
#elif defined(__i386__) || defined(_M_IX86)
		return "x86";
#else
		return "unknown-arch";
#endif
	}

	std::string BuildHardwareSummary()
	{
		std::ostringstream lStream;
		lStream << "OS=" << GetOsLabel() << ", Arch=" << GetArchLabel() << ", Threads=" << std::thread::hardware_concurrency();
		return lStream.str();
	}

	std::string BuildUtcIsoTimestamp()
	{
		const std::time_t liNow = std::time(nullptr);
		std::tm lUtcTm{};
#if defined(_WIN32)
		gmtime_s(&lUtcTm, &liNow);
#else
		gmtime_r(&liNow, &lUtcTm);
#endif
		std::ostringstream lStream;
		lStream << std::put_time(&lUtcTm, "%Y-%m-%dT%H:%M:%SZ");
		return lStream.str();
	}

	std::string SanitizeTemplateName(const std::string& pSInput)
	{
		std::string lSName;
		lSName.reserve(pSInput.size());

		for (const unsigned char lCChar : pSInput)
		{
			if (std::isalnum(lCChar) != 0 || lCChar == '-' || lCChar == '_')
			{
				lSName.push_back(static_cast<char>(lCChar));
			}
			else if (std::isspace(lCChar) != 0)
			{
				lSName.push_back('_');
			}
		}

		if (lSName.empty())
		{
			lSName = "autotune_template";
		}

		return lSName;
	}

	std::string EscapeXmlText(const std::string& pSValue)
	{
		std::string lSOutput;
		lSOutput.reserve(pSValue.size());

		for (const char lCChar : pSValue)
		{
			switch (lCChar)
			{
			case '&':
				lSOutput += "&amp;";
				break;
			case '<':
				lSOutput += "&lt;";
				break;
			case '>':
				lSOutput += "&gt;";
				break;
			case '"':
				lSOutput += "&quot;";
				break;
			case '\'':
				lSOutput += "&apos;";
				break;
			default:
				lSOutput.push_back(lCChar);
				break;
			}
		}

		return lSOutput;
	}

	std::string UnescapeXmlText(std::string pSValue)
	{
		auto ReplaceAll = [](std::string* pSInOut, const std::string& pSFrom, const std::string& pSTo)
		{
			if (pSInOut == nullptr || pSFrom.empty())
			{
				return;
			}

			std::size_t liPosition = 0;

			while (true)
			{
				liPosition = pSInOut->find(pSFrom, liPosition);

				if (liPosition == std::string::npos)
				{
					break;
				}

				pSInOut->replace(liPosition, pSFrom.size(), pSTo);
				liPosition += pSTo.size();
			}
		};

		ReplaceAll(&pSValue, "&lt;", "<");
		ReplaceAll(&pSValue, "&gt;", ">");
		ReplaceAll(&pSValue, "&quot;", "\"");
		ReplaceAll(&pSValue, "&apos;", "'");
		ReplaceAll(&pSValue, "&amp;", "&");
		return pSValue;
	}

	std::optional<std::string> ExtractXmlTagValue(const std::string& pSXmlText, const std::string& pSTagName)
	{
		const std::string lSStartTag = "<" + pSTagName + ">";
		const std::string lSEndTag = "</" + pSTagName + ">";
		const std::size_t liStart = pSXmlText.find(lSStartTag);

		if (liStart == std::string::npos)
		{
			return std::nullopt;
		}

		const std::size_t liContentStart = liStart + lSStartTag.size();
		const std::size_t liEnd = pSXmlText.find(lSEndTag, liContentStart);

		if (liEnd == std::string::npos)
		{
			return std::nullopt;
		}

		return UnescapeXmlText(pSXmlText.substr(liContentStart, liEnd - liContentStart));
	}

	bool SaveAutoTuneTemplateXmlFile(const ollama_engine_cli::autotune::AutoTuneTemplate& pAutoTuneTemplate, const std::filesystem::path& pPathDirectory, std::string* pSErrorOut)
	{
		std::error_code lErrorCode;
		std::filesystem::create_directories(pPathDirectory, lErrorCode);

		if (lErrorCode)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to create template directory: " + pPathDirectory.string();
			}

			return false;
		}

		const std::filesystem::path lPathTemplateFile = pPathDirectory / (pAutoTuneTemplate.pSName + ".xml");
		std::ofstream lFileOut(lPathTemplateFile, std::ios::out | std::ios::trunc);

		if (!lFileOut.good())
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to open template file for writing: " + lPathTemplateFile.string();
			}

			return false;
		}

		lFileOut << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		lFileOut << "<AutoTuneTemplate version=\"1\">\n";
		lFileOut << "  <TemplateName>" << EscapeXmlText(pAutoTuneTemplate.pSName) << "</TemplateName>\n";
		lFileOut << "  <CreatedUtc>" << EscapeXmlText(pAutoTuneTemplate.pSCreatedUtc) << "</CreatedUtc>\n";
		lFileOut << "  <ModelName>" << EscapeXmlText(pAutoTuneTemplate.pSModelName) << "</ModelName>\n";
		lFileOut << "  <HardwareFingerprint>" << EscapeXmlText(pAutoTuneTemplate.pSHardwareFingerprint) << "</HardwareFingerprint>\n";
		lFileOut << "  <HardwareSummary>" << EscapeXmlText(pAutoTuneTemplate.pSHardwareSummary) << "</HardwareSummary>\n";
		lFileOut << "  <Temperature>" << pAutoTuneTemplate.pGenerationSettings.pfTemperature << "</Temperature>\n";
		lFileOut << "  <TopP>" << pAutoTuneTemplate.pGenerationSettings.pfTopP << "</TopP>\n";
		lFileOut << "  <MinP>" << pAutoTuneTemplate.pGenerationSettings.pfMinP << "</MinP>\n";
		lFileOut << "  <TopK>" << pAutoTuneTemplate.pGenerationSettings.piTopK << "</TopK>\n";
		lFileOut << "  <RepeatPenalty>" << pAutoTuneTemplate.pGenerationSettings.pfRepeatPenalty << "</RepeatPenalty>\n";
		lFileOut << "  <Seed>" << pAutoTuneTemplate.pGenerationSettings.piSeed << "</Seed>\n";
		lFileOut << "  <RunsPerSetting>" << pAutoTuneTemplate.piRunsPerSetting << "</RunsPerSetting>\n";
		lFileOut << "  <EvaluationMode>" << EscapeXmlText(pAutoTuneTemplate.pSEvaluationMode) << "</EvaluationMode>\n";
		lFileOut << "  <EvaluationSource>" << EscapeXmlText(pAutoTuneTemplate.pSEvaluationSource) << "</EvaluationSource>\n";
		lFileOut << "  <AverageScore>" << pAutoTuneTemplate.pdAverageScore << "</AverageScore>\n";
		lFileOut << "  <AverageTokPerSec>" << pAutoTuneTemplate.pdAverageTokensPerSecond << "</AverageTokPerSec>\n";
		lFileOut << "  <AverageTotalMs>" << pAutoTuneTemplate.pdAverageTotalMilliseconds << "</AverageTotalMs>\n";
		lFileOut << "  <AverageTtftMs>" << pAutoTuneTemplate.pdAverageTtftMilliseconds << "</AverageTtftMs>\n";
		lFileOut << "</AutoTuneTemplate>\n";
		return true;
	}

	bool SaveAutoTuneFinalProfileOleFile(const ollama_engine_cli::autotune::AutoTuneTemplate& pAutoTuneTemplate, const std::filesystem::path& pPathDirectory, std::string* pSErrorOut)
	{
		std::error_code lErrorCode;
		std::filesystem::create_directories(pPathDirectory, lErrorCode);

		if (lErrorCode)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to create final profile directory: " + pPathDirectory.string();
			}

			return false;
		}

		const std::filesystem::path lPathTemplateFile = pPathDirectory / (pAutoTuneTemplate.pSName + ".ole");
		std::ofstream lFileOut(lPathTemplateFile, std::ios::out | std::ios::trunc);

		if (!lFileOut.good())
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to open final profile file for writing: " + lPathTemplateFile.string();
			}

			return false;
		}

		lFileOut << "OLE_VERSION=1\n";
		lFileOut << "TemplateName=" << pAutoTuneTemplate.pSName << "\n";
		lFileOut << "CreatedUtc=" << pAutoTuneTemplate.pSCreatedUtc << "\n";
		lFileOut << "ModelName=" << pAutoTuneTemplate.pSModelName << "\n";
		lFileOut << "HardwareFingerprint=" << pAutoTuneTemplate.pSHardwareFingerprint << "\n";
		lFileOut << "HardwareSummary=" << pAutoTuneTemplate.pSHardwareSummary << "\n";
		lFileOut << "Temperature=" << pAutoTuneTemplate.pGenerationSettings.pfTemperature << "\n";
		lFileOut << "TopP=" << pAutoTuneTemplate.pGenerationSettings.pfTopP << "\n";
		lFileOut << "MinP=" << pAutoTuneTemplate.pGenerationSettings.pfMinP << "\n";
		lFileOut << "TopK=" << pAutoTuneTemplate.pGenerationSettings.piTopK << "\n";
		lFileOut << "RepeatPenalty=" << pAutoTuneTemplate.pGenerationSettings.pfRepeatPenalty << "\n";
		lFileOut << "Seed=" << pAutoTuneTemplate.pGenerationSettings.piSeed << "\n";
		lFileOut << "RunsPerSetting=" << pAutoTuneTemplate.piRunsPerSetting << "\n";
		lFileOut << "EvaluationMode=" << pAutoTuneTemplate.pSEvaluationMode << "\n";
		lFileOut << "EvaluationSource=" << pAutoTuneTemplate.pSEvaluationSource << "\n";
		lFileOut << "AverageScore=" << pAutoTuneTemplate.pdAverageScore << "\n";
		lFileOut << "AverageTokPerSec=" << pAutoTuneTemplate.pdAverageTokensPerSecond << "\n";
		lFileOut << "AverageTotalMs=" << pAutoTuneTemplate.pdAverageTotalMilliseconds << "\n";
		lFileOut << "AverageTtftMs=" << pAutoTuneTemplate.pdAverageTtftMilliseconds << "\n";
		return true;
	}

	std::optional<ollama_engine_cli::autotune::AutoTuneTemplate> LoadAutoTuneTemplateXmlFile(const std::filesystem::path& pPathTemplateFile)
	{
		std::ifstream lFileIn(pPathTemplateFile, std::ios::in);

		if (!lFileIn.good())
		{
			return std::nullopt;
		}

		std::ostringstream lBuffer;
		lBuffer << lFileIn.rdbuf();
		const std::string lSXmlText = lBuffer.str();

		if (lSXmlText.empty())
		{
			return std::nullopt;
		}

		ollama_engine_cli::autotune::AutoTuneTemplate lAutoTuneTemplate;
		lAutoTuneTemplate.pSName = ExtractXmlTagValue(lSXmlText, "TemplateName").value_or(pPathTemplateFile.stem().string());
		lAutoTuneTemplate.pSCreatedUtc = ExtractXmlTagValue(lSXmlText, "CreatedUtc").value_or("");
		lAutoTuneTemplate.pSModelName = ExtractXmlTagValue(lSXmlText, "ModelName").value_or("");
		lAutoTuneTemplate.pSHardwareFingerprint = ExtractXmlTagValue(lSXmlText, "HardwareFingerprint").value_or("");
		lAutoTuneTemplate.pSHardwareSummary = ExtractXmlTagValue(lSXmlText, "HardwareSummary").value_or("");
		lAutoTuneTemplate.pGenerationSettings.pfTemperature = static_cast<float>(ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "Temperature").value_or(""), 0.8));
		lAutoTuneTemplate.pGenerationSettings.pfTopP = static_cast<float>(ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "TopP").value_or(""), 0.95));
		lAutoTuneTemplate.pGenerationSettings.pfMinP = static_cast<float>(ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "MinP").value_or(""), 0.05));
		lAutoTuneTemplate.pGenerationSettings.piTopK = ParseIntOrDefault(ExtractXmlTagValue(lSXmlText, "TopK").value_or(""), 40);
		lAutoTuneTemplate.pGenerationSettings.pfRepeatPenalty = static_cast<float>(ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "RepeatPenalty").value_or(""), 1.0));
		lAutoTuneTemplate.pGenerationSettings.piSeed = ParseUnsigned32OrDefault(ExtractXmlTagValue(lSXmlText, "Seed").value_or(""), 4294967295U);
		lAutoTuneTemplate.piRunsPerSetting = ParseIntOrDefault(ExtractXmlTagValue(lSXmlText, "RunsPerSetting").value_or(""), 3);
		lAutoTuneTemplate.pSEvaluationMode = ExtractXmlTagValue(lSXmlText, "EvaluationMode").value_or("");
		lAutoTuneTemplate.pSEvaluationSource = ExtractXmlTagValue(lSXmlText, "EvaluationSource").value_or("");
		lAutoTuneTemplate.pdAverageScore = ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "AverageScore").value_or(""), 0.0);
		lAutoTuneTemplate.pdAverageTokensPerSecond = ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "AverageTokPerSec").value_or(""), 0.0);
		lAutoTuneTemplate.pdAverageTotalMilliseconds = ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "AverageTotalMs").value_or(""), 0.0);
		lAutoTuneTemplate.pdAverageTtftMilliseconds = ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "AverageTtftMs").value_or(""), 0.0);
		return lAutoTuneTemplate;
	}

	std::optional<ollama_engine_cli::autotune::AutoTuneTemplate> LoadAutoTuneFinalProfileOleFile(const std::filesystem::path& pPathTemplateFile)
	{
		std::ifstream lFileIn(pPathTemplateFile, std::ios::in);

		if (!lFileIn.good())
		{
			return std::nullopt;
		}

		std::unordered_map<std::string, std::string> lMapValues;
		std::string lSLine;

		while (std::getline(lFileIn, lSLine))
		{
			const std::size_t liSeparator = lSLine.find('=');

			if (liSeparator == std::string::npos)
			{
				continue;
			}

			const std::string lSKey = ollama_engine_cli::common::Trim(lSLine.substr(0, liSeparator));
			const std::string lSValue = ollama_engine_cli::common::Trim(lSLine.substr(liSeparator + 1));

			if (!lSKey.empty())
			{
				lMapValues[lSKey] = lSValue;
			}
		}

		if (lMapValues.empty())
		{
			return std::nullopt;
		}

		ollama_engine_cli::autotune::AutoTuneTemplate lAutoTuneTemplate;
		lAutoTuneTemplate.pSName = lMapValues.count("TemplateName") > 0 ? lMapValues["TemplateName"] : pPathTemplateFile.stem().string();
		lAutoTuneTemplate.pSCreatedUtc = lMapValues["CreatedUtc"];
		lAutoTuneTemplate.pSModelName = lMapValues["ModelName"];
		lAutoTuneTemplate.pSHardwareFingerprint = lMapValues["HardwareFingerprint"];
		lAutoTuneTemplate.pSHardwareSummary = lMapValues["HardwareSummary"];
		lAutoTuneTemplate.pGenerationSettings.pfTemperature = static_cast<float>(ParseDoubleOrDefault(lMapValues["Temperature"], 0.8));
		lAutoTuneTemplate.pGenerationSettings.pfTopP = static_cast<float>(ParseDoubleOrDefault(lMapValues["TopP"], 0.95));
		lAutoTuneTemplate.pGenerationSettings.pfMinP = static_cast<float>(ParseDoubleOrDefault(lMapValues["MinP"], 0.05));
		lAutoTuneTemplate.pGenerationSettings.piTopK = ParseIntOrDefault(lMapValues["TopK"], 40);
		lAutoTuneTemplate.pGenerationSettings.pfRepeatPenalty = static_cast<float>(ParseDoubleOrDefault(lMapValues["RepeatPenalty"], 1.0));
		lAutoTuneTemplate.pGenerationSettings.piSeed = ParseUnsigned32OrDefault(lMapValues["Seed"], 4294967295U);
		lAutoTuneTemplate.piRunsPerSetting = ParseIntOrDefault(lMapValues["RunsPerSetting"], 3);
		lAutoTuneTemplate.pSEvaluationMode = lMapValues["EvaluationMode"];
		lAutoTuneTemplate.pSEvaluationSource = lMapValues["EvaluationSource"];
		lAutoTuneTemplate.pdAverageScore = ParseDoubleOrDefault(lMapValues["AverageScore"], 0.0);
		lAutoTuneTemplate.pdAverageTokensPerSecond = ParseDoubleOrDefault(lMapValues["AverageTokPerSec"], 0.0);
		lAutoTuneTemplate.pdAverageTotalMilliseconds = ParseDoubleOrDefault(lMapValues["AverageTotalMs"], 0.0);
		lAutoTuneTemplate.pdAverageTtftMilliseconds = ParseDoubleOrDefault(lMapValues["AverageTtftMs"], 0.0);
		return lAutoTuneTemplate;
	}

	std::vector<ollama_engine::GenerationSettings> BuildAutoTuneCandidates()
	{
		std::vector<ollama_engine::GenerationSettings> lVecGenerationSettings;
		const std::vector<float> lVecfTemperature = {0.3f, 0.6f, 0.9f, 1.1f};
		const std::vector<float> lVecfTopP = {0.90f, 0.95f};
		const std::vector<float> lVecfMinP = {0.02f, 0.05f};
		const std::vector<float> lVecfRepeatPenalty = {1.0f, 1.1f};

		for (const float lfTemperature : lVecfTemperature)
		{
			for (const float lfTopP : lVecfTopP)
			{
				for (const float lfMinP : lVecfMinP)
				{
					for (const float lfRepeatPenalty : lVecfRepeatPenalty)
					{
						ollama_engine::GenerationSettings lGenerationSettings;
						lGenerationSettings.pfTemperature = lfTemperature;
						lGenerationSettings.pfTopP = lfTopP;
						lGenerationSettings.pfMinP = lfMinP;
						lGenerationSettings.piTopK = 40;
						lGenerationSettings.pfRepeatPenalty = lfRepeatPenalty;
						lGenerationSettings.piSeed = 4294967295U;
						lVecGenerationSettings.push_back(lGenerationSettings);
					}
				}
			}
		}

		return lVecGenerationSettings;
	}

	AutoTuneEvaluationSummary EvaluateAutoTuneSettings(ollama_engine::EngineInterface* pPtrEngine, const AutoTuneWizardConfig& pAutoTuneWizardConfig)
	{
		AutoTuneEvaluationSummary lAutoTuneEvaluationSummary;

		if (pPtrEngine == nullptr)
		{
			lAutoTuneEvaluationSummary.pSError = "Engine pointer is null.";
			return lAutoTuneEvaluationSummary;
		}

		std::size_t liPassCount = 0;
		std::size_t liCaseCount = 0;
		double ldTotalMs = 0.0;
		double ldTotalTtftMs = 0.0;
		double ldTotalTokPerSec = 0.0;

		if (pAutoTuneWizardConfig.pAutoTuneEvaluationMode == AutoTuneEvaluationMode::BuiltInBenchmarks)
		{
			const std::vector<ollama_engine_cli::tests::BenchmarkProbe> lVecBenchmarkProbes = ollama_engine_cli::tests::BuildStandardOpenSourceBenchmarkProbes();

			if (lVecBenchmarkProbes.empty())
			{
				lAutoTuneEvaluationSummary.pSError = "No built-in benchmark probes are available.";
				return lAutoTuneEvaluationSummary;
			}

			for (int liRun = 0; liRun < pAutoTuneWizardConfig.piRunsPerSetting; ++liRun)
			{
				for (const ollama_engine_cli::tests::BenchmarkProbe& lBenchmarkProbe : lVecBenchmarkProbes)
				{
					const ollama_engine_cli::common::PromptRunResult lPromptRunResult = ollama_engine_cli::common::RunPromptWithMetrics(pPtrEngine, lBenchmarkProbe.pSPrompt);
					++liCaseCount;
					ldTotalMs += lPromptRunResult.pdTotalMilliseconds;
					ldTotalTtftMs += lPromptRunResult.pdTimeToFirstTokenMilliseconds;
					ldTotalTokPerSec += lPromptRunResult.pdTokensPerSecond;

					if (lPromptRunResult.pSendMessageResponse.pbOk && ollama_engine_cli::tests::IsBenchmarkProbePass(lBenchmarkProbe, lPromptRunResult.pSendMessageResponse.pSText))
					{
						++liPassCount;
					}
				}
			}
		}
		else
		{
			const std::vector<ollama_engine_cli::tests::QuestionAnswerCase> lVecQuestionAnswerCases = ollama_engine_cli::tests::LoadQuestionAnswerCases(pAutoTuneWizardConfig.pPathQuestionAnswerFolder);

			if (lVecQuestionAnswerCases.empty())
			{
				lAutoTuneEvaluationSummary.pSError = "No Question_N test pairs found in " + pAutoTuneWizardConfig.pPathQuestionAnswerFolder.string();
				return lAutoTuneEvaluationSummary;
			}

			for (int liRun = 0; liRun < pAutoTuneWizardConfig.piRunsPerSetting; ++liRun)
			{
				for (const ollama_engine_cli::tests::QuestionAnswerCase& lQuestionAnswerCase : lVecQuestionAnswerCases)
				{
					const ollama_engine_cli::common::PromptRunResult lPromptRunResult = ollama_engine_cli::common::RunPromptWithMetrics(pPtrEngine, lQuestionAnswerCase.pSQuestionText);
					++liCaseCount;
					ldTotalMs += lPromptRunResult.pdTotalMilliseconds;
					ldTotalTtftMs += lPromptRunResult.pdTimeToFirstTokenMilliseconds;
					ldTotalTokPerSec += lPromptRunResult.pdTokensPerSecond;

					const std::string lSActual = ollama_engine_cli::common::NormalizeForMatch(lPromptRunResult.pSendMessageResponse.pSText);
					const std::string lSExpected = ollama_engine_cli::common::NormalizeForMatch(lQuestionAnswerCase.pSAnswerText);
					const bool lbPass = lPromptRunResult.pSendMessageResponse.pbOk && !lSExpected.empty() && (lSActual.find(lSExpected) != std::string::npos);

					if (lbPass)
					{
						++liPassCount;
					}
				}
			}
		}

		if (liCaseCount == 0)
		{
			lAutoTuneEvaluationSummary.pSError = "No autotune evaluation cases were executed.";
			return lAutoTuneEvaluationSummary;
		}

		lAutoTuneEvaluationSummary.pbOk = true;
		lAutoTuneEvaluationSummary.piPassCount = liPassCount;
		lAutoTuneEvaluationSummary.piCaseCount = liCaseCount;
		lAutoTuneEvaluationSummary.pdAverageScore = static_cast<double>(liPassCount) / static_cast<double>(liCaseCount);
		lAutoTuneEvaluationSummary.pdAverageTotalMilliseconds = ldTotalMs / static_cast<double>(liCaseCount);
		lAutoTuneEvaluationSummary.pdAverageTtftMilliseconds = ldTotalTtftMs / static_cast<double>(liCaseCount);
		lAutoTuneEvaluationSummary.pdAverageTokensPerSecond = ldTotalTokPerSec / static_cast<double>(liCaseCount);
		return lAutoTuneEvaluationSummary;
	}

	double ComputeAutoTuneObjectiveScore(const AutoTuneEvaluationSummary& pAutoTuneEvaluationSummary, const AutoTuneWizardConfig& pAutoTuneWizardConfig)
	{
		if (!pAutoTuneEvaluationSummary.pbOk)
		{
			return 0.0;
		}

		const double ldQualityScore = pAutoTuneEvaluationSummary.pdAverageScore;
		const double ldSpeedScore = (pAutoTuneWizardConfig.pdTargetTokensPerSecond > 0.0) ? std::clamp(pAutoTuneEvaluationSummary.pdAverageTokensPerSecond / pAutoTuneWizardConfig.pdTargetTokensPerSecond, 0.0, 1.0) : 0.5;
		const double ldLatencyScore = (pAutoTuneWizardConfig.pdTargetLatencyMilliseconds > 0.0) ? std::clamp(pAutoTuneWizardConfig.pdTargetLatencyMilliseconds / std::max(1.0, pAutoTuneEvaluationSummary.pdAverageTotalMilliseconds), 0.0, 1.0) : 0.5;
		return (ldQualityScore * 0.70) + (ldSpeedScore * 0.15) + (ldLatencyScore * 0.15);
	}

	void PrintAutoTunePrimer()
	{
		std::cout << "autotune> What this means in plain language:\n";
		std::cout << "autotune> - Quality score: how often answers pass your checks.\n";
		std::cout << "autotune> - tok/s target: response speed target (higher is faster).\n";
		std::cout << "autotune> - latency target: full-response time target in milliseconds (lower is better).\n";
		std::cout << "autotune> - runs per setting: how many repeats to average so results are stable.\n";
		std::cout << "autotune> The tuner varies randomness/repetition controls and keeps the best average setup.\n";
	}

} // namespace

namespace ollama_engine_cli::autotune
{

	std::string DescribeGenerationSettings(const ollama_engine::GenerationSettings& pGenerationSettings)
	{
		std::ostringstream lStream;
		lStream << "temperature=" << pGenerationSettings.pfTemperature << ", top_p=" << pGenerationSettings.pfTopP << ", min_p=" << pGenerationSettings.pfMinP << ", top_k=" << pGenerationSettings.piTopK << ", repeat_penalty=" << pGenerationSettings.pfRepeatPenalty << ", seed=" << pGenerationSettings.piSeed;
		return lStream.str();
	}

	std::string BuildHardwareFingerprint()
	{
		std::ostringstream lStream;
		lStream << GetOsLabel() << "-" << GetArchLabel() << "-threads" << std::thread::hardware_concurrency();
		return lStream.str();
	}

	std::filesystem::path GetAutoTuneTemplateDirectory(const std::filesystem::path& pPathModelFolder)
	{
		return pPathModelFolder / "autotune_templates";
	}

	std::filesystem::path GetAutoTuneFinalProfileDirectory(const std::filesystem::path& pPathModelFolder)
	{
		return pPathModelFolder / "autotune_final_profiles";
	}

	std::vector<AutoTuneTemplate> LoadAutoTuneTemplates(const std::filesystem::path& pPathDirectory)
	{
		std::vector<AutoTuneTemplate> lVecAutoTuneTemplates;
		std::error_code lErrorCode;

		if (!std::filesystem::exists(pPathDirectory, lErrorCode) || !std::filesystem::is_directory(pPathDirectory, lErrorCode))
		{
			return lVecAutoTuneTemplates;
		}

		for (const std::filesystem::directory_entry& lDirectoryEntry : std::filesystem::directory_iterator(pPathDirectory, lErrorCode))
		{
			if (lErrorCode || !lDirectoryEntry.is_regular_file(lErrorCode))
			{
				continue;
			}

			std::string lSExtension = lDirectoryEntry.path().extension().string();
			std::transform(lSExtension.begin(), lSExtension.end(), lSExtension.begin(), [](unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });

			if (lSExtension != ".xml")
			{
				continue;
			}

			const std::optional<AutoTuneTemplate> lOptTemplate = LoadAutoTuneTemplateXmlFile(lDirectoryEntry.path());

			if (lOptTemplate.has_value())
			{
				lVecAutoTuneTemplates.push_back(*lOptTemplate);
			}
		}

		const auto lFnSortByNameThenCreated = [](const AutoTuneTemplate& pLhs, const AutoTuneTemplate& pRhs)
		{
			if (pLhs.pSName != pRhs.pSName)
			{
				return pLhs.pSName < pRhs.pSName;
			}

			return pLhs.pSCreatedUtc < pRhs.pSCreatedUtc;
		};
		std::sort(lVecAutoTuneTemplates.begin(), lVecAutoTuneTemplates.end(), lFnSortByNameThenCreated);
		return lVecAutoTuneTemplates;
	}

	std::vector<AutoTuneTemplate> LoadAutoTuneFinalProfiles(const std::filesystem::path& pPathDirectory)
	{
		std::vector<AutoTuneTemplate> lVecAutoTuneProfiles;
		std::error_code lErrorCode;

		if (!std::filesystem::exists(pPathDirectory, lErrorCode) || !std::filesystem::is_directory(pPathDirectory, lErrorCode))
		{
			return lVecAutoTuneProfiles;
		}

		for (const std::filesystem::directory_entry& lDirectoryEntry : std::filesystem::directory_iterator(pPathDirectory, lErrorCode))
		{
			if (lErrorCode || !lDirectoryEntry.is_regular_file(lErrorCode))
			{
				continue;
			}

			std::string lSExtension = lDirectoryEntry.path().extension().string();
			std::transform(lSExtension.begin(), lSExtension.end(), lSExtension.begin(), [](unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });

			if (lSExtension != ".ole")
			{
				continue;
			}

			const std::optional<AutoTuneTemplate> lOptProfile = LoadAutoTuneFinalProfileOleFile(lDirectoryEntry.path());

			if (lOptProfile.has_value())
			{
				lVecAutoTuneProfiles.push_back(*lOptProfile);
			}
		}

		const auto lFnSortByNameThenCreated = [](const AutoTuneTemplate& pLhs, const AutoTuneTemplate& pRhs)
		{
			if (pLhs.pSName != pRhs.pSName)
			{
				return pLhs.pSName < pRhs.pSName;
			}

			return pLhs.pSCreatedUtc < pRhs.pSCreatedUtc;
		};
		std::sort(lVecAutoTuneProfiles.begin(), lVecAutoTuneProfiles.end(), lFnSortByNameThenCreated);
		return lVecAutoTuneProfiles;
	}

	std::optional<AutoTuneTemplate> FindBestMatchingTemplate(const std::vector<AutoTuneTemplate>& pVecAutoTuneTemplates, const std::string& pSModelName, const std::string& pSHardwareFingerprint)
	{
		std::optional<AutoTuneTemplate> lOptBestTemplate;

		for (const AutoTuneTemplate& lAutoTuneTemplate : pVecAutoTuneTemplates)
		{
			if (lAutoTuneTemplate.pSModelName != pSModelName)
			{
				continue;
			}

			if (lAutoTuneTemplate.pSHardwareFingerprint != pSHardwareFingerprint)
			{
				continue;
			}

			if (!lOptBestTemplate.has_value() || lAutoTuneTemplate.pSCreatedUtc > lOptBestTemplate->pSCreatedUtc)
			{
				lOptBestTemplate = lAutoTuneTemplate;
			}
		}

		return lOptBestTemplate;
	}

	bool ApplyAutoTuneTemplate(ollama_engine::EngineInterface* pPtrEngine, const AutoTuneTemplate& pAutoTuneTemplate, std::string* pSLoadedModelNameInOut, std::string* pSErrorOut)
	{
		if (pPtrEngine == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Engine pointer is null.";
			}

			return false;
		}

		if (pSLoadedModelNameInOut == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Loaded model pointer is null.";
			}

			return false;
		}

		if (*pSLoadedModelNameInOut != pAutoTuneTemplate.pSModelName)
		{
			std::string lSLoadError;

			if (!pPtrEngine->Load(pAutoTuneTemplate.pSModelName, &lSLoadError))
			{
				if (pSErrorOut != nullptr)
				{
					*pSErrorOut = "Failed to load template model '" + pAutoTuneTemplate.pSModelName + "': " + lSLoadError;
				}

				return false;
			}

			*pSLoadedModelNameInOut = pAutoTuneTemplate.pSModelName;
		}

		std::string lSSettingsError;

		if (!pPtrEngine->SetGenerationSettings(pAutoTuneTemplate.pGenerationSettings, &lSSettingsError))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to apply template generation settings: " + lSSettingsError;
			}

			return false;
		}

		return true;
	}

	void RunAutoTuneWizard(ollama_engine::EngineInterface* pPtrEngine, const std::vector<std::string>& pVecSModels, const std::filesystem::path& pPathModelFolder, std::string* pSLoadedModelNameInOut)
	{
		if (pPtrEngine == nullptr)
		{
			std::cout << "autotune> engine is not available.\n";
			return;
		}

		if (pVecSModels.empty())
		{
			std::cout << "autotune> no models found.\n";
			return;
		}

		AutoTuneWizardConfig lAutoTuneWizardConfig;
		lAutoTuneWizardConfig.pSModelName = (pSLoadedModelNameInOut != nullptr) ? *pSLoadedModelNameInOut : pVecSModels.front();

		std::cout << "autotune> Wizard (plain language)\n";
		std::cout << "autotune> We will test multiple generation settings and pick the best average result.\n";
		PrintAutoTunePrimer();
		ollama_engine_cli::common::PrintModels(pVecSModels);
		const std::string lSModelSelectionInput = ollama_engine_cli::common::ReadLine("autotune> pick model (number/name, Enter=current): ");

		if (!ollama_engine_cli::common::Trim(lSModelSelectionInput).empty())
		{
			const std::optional<std::string> lOptSelectedModel = ollama_engine_cli::common::ResolveSelectedModel(ollama_engine_cli::common::Trim(lSModelSelectionInput), pVecSModels);

			if (!lOptSelectedModel.has_value())
			{
				std::cout << "autotune> invalid model selection.\n";
				return;
			}

			lAutoTuneWizardConfig.pSModelName = *lOptSelectedModel;
		}

		if (pSLoadedModelNameInOut == nullptr || *pSLoadedModelNameInOut != lAutoTuneWizardConfig.pSModelName)
		{
			std::string lSError;

			if (!pPtrEngine->Load(lAutoTuneWizardConfig.pSModelName, &lSError))
			{
				std::cout << "autotune> failed to load model '" << lAutoTuneWizardConfig.pSModelName << "': " << lSError << "\n";
				return;
			}

			if (pSLoadedModelNameInOut != nullptr)
			{
				*pSLoadedModelNameInOut = lAutoTuneWizardConfig.pSModelName;
			}
		}

		std::cout << "autotune> Evaluation target:\n";
		std::cout << "  1) Built-in open-source mini probes (fast start)\n";
		std::cout << "  2) Custom Question_N test files (best for your domain)\n";
		const int liEvaluationChoice = std::clamp(ParseIntOrDefault(ollama_engine_cli::common::ReadLine("autotune> choose 1 or 2 [1]: "), 1), 1, 2);

		if (liEvaluationChoice == 2)
		{
			lAutoTuneWizardConfig.pAutoTuneEvaluationMode = AutoTuneEvaluationMode::QuestionAnswerFolder;
			const std::string lSQuestionFolderPath = ollama_engine_cli::common::Trim(ollama_engine_cli::common::ReadLine("autotune> path to Question_N files: "));

			if (lSQuestionFolderPath.empty())
			{
				std::cout << "autotune> custom mode requires a folder path.\n";
				return;
			}

			lAutoTuneWizardConfig.pPathQuestionAnswerFolder = std::filesystem::path(lSQuestionFolderPath);
		}
		else
		{
			lAutoTuneWizardConfig.pAutoTuneEvaluationMode = AutoTuneEvaluationMode::BuiltInBenchmarks;
		}

		lAutoTuneWizardConfig.pdTargetTokensPerSecond = std::max(0.0, ParseDoubleOrDefault(ollama_engine_cli::common::ReadLine("autotune> target speed tok/s (0=ignore) [0]: "), 0.0));
		lAutoTuneWizardConfig.pdTargetLatencyMilliseconds = std::max(0.0, ParseDoubleOrDefault(ollama_engine_cli::common::ReadLine("autotune> target latency ms (0=ignore) [0]: "), 0.0));
		lAutoTuneWizardConfig.piRunsPerSetting = std::clamp(ParseIntOrDefault(ollama_engine_cli::common::ReadLine("autotune> runs per setting for average [3]: "), 3), 1, 10);

		const std::vector<ollama_engine::GenerationSettings> lVecGenerationSettingsCandidates = BuildAutoTuneCandidates();
		std::cout << "autotune> candidate setting count: " << lVecGenerationSettingsCandidates.size() << "\n";

		if (!ReadYesNo("autotune> start tuning now? [Y/n]: ", true))
		{
			std::cout << "autotune> cancelled.\n";
			return;
		}

		double ldBestObjectiveScore = -1.0;
		AutoTuneEvaluationSummary lBestAutoTuneEvaluationSummary;
		ollama_engine::GenerationSettings lBestGenerationSettings;

		for (std::size_t liCandidateIndex = 0; liCandidateIndex < lVecGenerationSettingsCandidates.size(); ++liCandidateIndex)
		{
			const ollama_engine::GenerationSettings& lGenerationSettings = lVecGenerationSettingsCandidates[liCandidateIndex];
			std::string lSError;

			if (!pPtrEngine->SetGenerationSettings(lGenerationSettings, &lSError))
			{
				std::cout << "autotune> candidate " << (liCandidateIndex + 1) << " skipped: " << lSError << "\n";
				continue;
			}

			const AutoTuneEvaluationSummary lAutoTuneEvaluationSummary = EvaluateAutoTuneSettings(pPtrEngine, lAutoTuneWizardConfig);

			if (!lAutoTuneEvaluationSummary.pbOk)
			{
				std::cout << "autotune> candidate " << (liCandidateIndex + 1) << " evaluation failed: " << lAutoTuneEvaluationSummary.pSError << "\n";
				continue;
			}

			const double ldObjectiveScore = ComputeAutoTuneObjectiveScore(lAutoTuneEvaluationSummary, lAutoTuneWizardConfig);
			std::cout << "autotune> candidate " << (liCandidateIndex + 1) << "/" << lVecGenerationSettingsCandidates.size() << " score=" << ldObjectiveScore << " pass=" << lAutoTuneEvaluationSummary.piPassCount << "/" << lAutoTuneEvaluationSummary.piCaseCount << " avg_ms=" << lAutoTuneEvaluationSummary.pdAverageTotalMilliseconds << " tok/s=" << lAutoTuneEvaluationSummary.pdAverageTokensPerSecond << " settings: " << DescribeGenerationSettings(lGenerationSettings) << "\n";

			if (ldObjectiveScore > ldBestObjectiveScore)
			{
				ldBestObjectiveScore = ldObjectiveScore;
				lBestAutoTuneEvaluationSummary = lAutoTuneEvaluationSummary;
				lBestGenerationSettings = lGenerationSettings;
			}
		}

		if (ldBestObjectiveScore < 0.0)
		{
			std::cout << "autotune> no successful candidate evaluations.\n";
			return;
		}

		std::string lSError;

		if (!pPtrEngine->SetGenerationSettings(lBestGenerationSettings, &lSError))
		{
			std::cout << "autotune> failed to apply best settings: " << lSError << "\n";
			return;
		}

		std::cout << "autotune> best settings applied.\n";
		std::cout << "autotune> score=" << ldBestObjectiveScore << " pass=" << lBestAutoTuneEvaluationSummary.piPassCount << "/" << lBestAutoTuneEvaluationSummary.piCaseCount << " avg_ms=" << lBestAutoTuneEvaluationSummary.pdAverageTotalMilliseconds << " avg_ttft_ms=" << lBestAutoTuneEvaluationSummary.pdAverageTtftMilliseconds << " avg_tok/s=" << lBestAutoTuneEvaluationSummary.pdAverageTokensPerSecond << "\n";
		std::cout << "autotune> " << DescribeGenerationSettings(lBestGenerationSettings) << "\n";

		const std::string lSDefaultTemplateName = SanitizeTemplateName(lAutoTuneWizardConfig.pSModelName + "_autotune");
		std::string lSTemplateName = ollama_engine_cli::common::Trim(ollama_engine_cli::common::ReadLine("autotune> profile/template base name [" + lSDefaultTemplateName + "]: "));

		if (lSTemplateName.empty())
		{
			lSTemplateName = lSDefaultTemplateName;
		}

		lSTemplateName = SanitizeTemplateName(lSTemplateName);

		AutoTuneTemplate lAutoTuneTemplate;
		lAutoTuneTemplate.pSName = lSTemplateName;
		lAutoTuneTemplate.pSModelName = lAutoTuneWizardConfig.pSModelName;
		lAutoTuneTemplate.pSCreatedUtc = BuildUtcIsoTimestamp();
		lAutoTuneTemplate.pSHardwareFingerprint = BuildHardwareFingerprint();
		lAutoTuneTemplate.pSHardwareSummary = BuildHardwareSummary();
		lAutoTuneTemplate.pGenerationSettings = lBestGenerationSettings;
		lAutoTuneTemplate.piRunsPerSetting = lAutoTuneWizardConfig.piRunsPerSetting;
		lAutoTuneTemplate.pSEvaluationMode = (lAutoTuneWizardConfig.pAutoTuneEvaluationMode == AutoTuneEvaluationMode::BuiltInBenchmarks) ? "builtin" : "question_folder";
		lAutoTuneTemplate.pSEvaluationSource = lAutoTuneWizardConfig.pPathQuestionAnswerFolder.string();
		lAutoTuneTemplate.pdAverageScore = lBestAutoTuneEvaluationSummary.pdAverageScore;
		lAutoTuneTemplate.pdAverageTokensPerSecond = lBestAutoTuneEvaluationSummary.pdAverageTokensPerSecond;
		lAutoTuneTemplate.pdAverageTotalMilliseconds = lBestAutoTuneEvaluationSummary.pdAverageTotalMilliseconds;
		lAutoTuneTemplate.pdAverageTtftMilliseconds = lBestAutoTuneEvaluationSummary.pdAverageTtftMilliseconds;

		bool lbSavedSomething = false;

		if (ReadYesNo("autotune> save reusable template (.xml)? [Y/n]: ", true))
		{
			std::string lSSaveTemplateError;
			const std::filesystem::path lPathTemplateDirectory = GetAutoTuneTemplateDirectory(pPathModelFolder);

			if (!SaveAutoTuneTemplateXmlFile(lAutoTuneTemplate, lPathTemplateDirectory, &lSSaveTemplateError))
			{
				std::cout << "autotune> failed to save template: " << lSSaveTemplateError << "\n";
			}
			else
			{
				std::cout << "autotune> saved template: " << (lPathTemplateDirectory / (lAutoTuneTemplate.pSName + ".xml")) << "\n";
				lbSavedSomething = true;
			}
		}

		if (ReadYesNo("autotune> save finalized finetune profile (.ole) for hardware auto-apply? [Y/n]: ", true))
		{
			std::string lSSaveFinalError;
			const std::filesystem::path lPathFinalProfileDirectory = GetAutoTuneFinalProfileDirectory(pPathModelFolder);

			if (!SaveAutoTuneFinalProfileOleFile(lAutoTuneTemplate, lPathFinalProfileDirectory, &lSSaveFinalError))
			{
				std::cout << "autotune> failed to save finalized profile: " << lSSaveFinalError << "\n";
			}
			else
			{
				std::cout << "autotune> saved finalized profile: " << (lPathFinalProfileDirectory / (lAutoTuneTemplate.pSName + ".ole")) << "\n";
				lbSavedSomething = true;
			}
		}

		if (!lbSavedSomething)
		{
			std::cout << "autotune> no file saved.\n";
		}
	}

} // namespace ollama_engine_cli::autotune
