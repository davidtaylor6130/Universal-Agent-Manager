#include "ollama_engine_cli_common_internal.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace
{

	std::size_t CountTokens(const std::string& pSText)
	{
		std::istringstream lStream(pSText);
		std::size_t liCount = 0;
		std::string lSToken;

		while (lStream >> lSToken)
		{
			++liCount;
		}

		return liCount;
	}

} // namespace

namespace ollama_engine_cli::common
{

	void PrintModels(const std::vector<std::string>& pVecSModels)
	{
		std::cout << "Available models:\n";

		for (std::size_t liIndex = 0; liIndex < pVecSModels.size(); ++liIndex)
		{
			std::cout << "  [" << (liIndex + 1) << "] " << pVecSModels[liIndex] << "\n";
		}
	}

	std::string ReadLine(const std::string& pSPrompt)
	{
		std::cout << pSPrompt;
		std::cout.flush();
		std::string lSLine;
		std::getline(std::cin, lSLine);
		return lSLine;
	}

	std::optional<std::size_t> ParseModelIndex(const std::string& pSInput)
	{
		if (pSInput.empty())
		{
			return std::nullopt;
		}

		char* lPtrEnd = nullptr;
		const unsigned long lliValue = std::strtoul(pSInput.c_str(), &lPtrEnd, 10);

		if (lPtrEnd == nullptr || *lPtrEnd != '\0' || lliValue == 0UL)
		{
			return std::nullopt;
		}

		return static_cast<std::size_t>(lliValue - 1UL);
	}

	std::optional<std::string> ResolveSelectedModel(const std::string& pSInput, const std::vector<std::string>& pVecSModels)
	{
		const std::optional<std::size_t> lOptModelIndex = ParseModelIndex(pSInput);

		if (lOptModelIndex.has_value() && *lOptModelIndex < pVecSModels.size())
		{
			return pVecSModels[*lOptModelIndex];
		}

		for (const std::string& lSModelName : pVecSModels)
		{
			if (lSModelName == pSInput)
			{
				return lSModelName;
			}
		}

		return std::nullopt;
	}

	std::optional<std::string> ReadTextFile(const std::filesystem::path& pPathFile)
	{
		std::ifstream lFileIn(pPathFile, std::ios::in | std::ios::binary);

		if (!lFileIn.good())
		{
			return std::nullopt;
		}

		std::ostringstream lBuffer;
		lBuffer << lFileIn.rdbuf();
		return lBuffer.str();
	}

	std::string Trim(const std::string& pSValue)
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

	std::string NormalizeForMatch(const std::string& pSText)
	{
		std::string lSOutput;
		lSOutput.reserve(pSText.size());
		bool lbPrevSpace = true;

		for (const unsigned char lCChar : pSText)
		{
			if (std::isalnum(lCChar) != 0)
			{
				lSOutput.push_back(static_cast<char>(std::tolower(lCChar)));
				lbPrevSpace = false;
				continue;
			}

			if (!lbPrevSpace)
			{
				lSOutput.push_back(' ');
				lbPrevSpace = true;
			}
		}

		return Trim(lSOutput);
	}

	PromptRunResult RunPromptWithMetrics(ollama_engine::EngineInterface* pPtrEngine, const std::string& pSPrompt)
	{
		PromptRunResult lPromptRunResult;
		std::atomic<bool> lbDone{false};
		std::optional<std::chrono::steady_clock::time_point> lOptFirstTokenPoint;
		const std::chrono::steady_clock::time_point lStart = std::chrono::steady_clock::now();
		const auto lFnRunPrompt = [&]()
		{
			lPromptRunResult.pSendMessageResponse = pPtrEngine->SendMessage(pSPrompt);
			lbDone.store(true);
		};
		std::thread lWorker(lFnRunPrompt);

		while (!lbDone.load())
		{
			const ollama_engine::CurrentStateResponse lCurrentStateResponse = pPtrEngine->QueryCurrentState();

			if (!lOptFirstTokenPoint.has_value() && (lCurrentStateResponse.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Thinking || lCurrentStateResponse.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Running || lCurrentStateResponse.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Finished))
			{
				lOptFirstTokenPoint = std::chrono::steady_clock::now();
			}

			std::this_thread::sleep_for(std::chrono::microseconds(250));
		}

		lWorker.join();

		const std::chrono::steady_clock::time_point lEnd = std::chrono::steady_clock::now();

		if (!lOptFirstTokenPoint.has_value())
		{
			lOptFirstTokenPoint = lEnd;
		}

		const std::chrono::duration<double, std::milli> lTotalDuration = lEnd - lStart;
		const std::chrono::duration<double, std::milli> lFirstTokenDuration = *lOptFirstTokenPoint - lStart;
		lPromptRunResult.pdTotalMilliseconds = lTotalDuration.count();
		lPromptRunResult.pdTimeToFirstTokenMilliseconds = lFirstTokenDuration.count();
		lPromptRunResult.piOutputTokenCount = CountTokens(lPromptRunResult.pSendMessageResponse.pSText);
		const double ldTotalSeconds = lPromptRunResult.pdTotalMilliseconds / 1000.0;
		lPromptRunResult.pdTokensPerSecond = (ldTotalSeconds > 0.0) ? static_cast<double>(lPromptRunResult.piOutputTokenCount) / ldTotalSeconds : 0.0;
		return lPromptRunResult;
	}

	void PrintMetrics(const PromptRunResult& pPromptRunResult)
	{
		std::cout << "metrics> ttft_ms=" << pPromptRunResult.pdTimeToFirstTokenMilliseconds << " total_ms=" << pPromptRunResult.pdTotalMilliseconds << " output_tokens=" << pPromptRunResult.piOutputTokenCount << " tok/s=" << pPromptRunResult.pdTokensPerSecond << "\n";
	}

	bool BootstrapCliSession(int argc, char** argv, const std::string& pSPromptPrefix, CliSession* pCliSessionOut, int* piExitCodeOut)
	{
		if (pCliSessionOut == nullptr || piExitCodeOut == nullptr)
		{
			return false;
		}

		CliSession lCliSession;
		lCliSession.pPathModelFolder = std::filesystem::current_path() / "models";

		if (argc > 1 && argv[1] != nullptr && std::string(argv[1]).empty() == false)
		{
			lCliSession.pPathModelFolder = std::filesystem::path(argv[1]);
		}

		ollama_engine::EngineOptions lEngineOptions;
		lEngineOptions.pPathModelFolder = lCliSession.pPathModelFolder;
		lEngineOptions.piEmbeddingDimensions = 256;
		lCliSession.pPtrEngine = ollama_engine::CreateEngine(lEngineOptions);

		if (!lCliSession.pPtrEngine)
		{
			std::cerr << pSPromptPrefix << "> failed to create engine.\n";
			*piExitCodeOut = 1;
			return false;
		}

		lCliSession.pVecSModels = lCliSession.pPtrEngine->ListModels();

		if (lCliSession.pVecSModels.empty())
		{
			std::cerr << pSPromptPrefix << "> no models found in folder: " << lCliSession.pPathModelFolder << "\n";
			*piExitCodeOut = 1;
			return false;
		}

		std::cout << pSPromptPrefix << "> model folder: " << lCliSession.pPathModelFolder << "\n";
		PrintModels(lCliSession.pVecSModels);

		while (true)
		{
			const std::string lSInput = ReadLine(pSPromptPrefix + "> select model (number/name, /quit to exit): ");

			if (!std::cin.good() || lSInput == "/quit")
			{
				*piExitCodeOut = 0;
				return false;
			}

			const std::optional<std::string> lOptSelectedModel = ResolveSelectedModel(Trim(lSInput), lCliSession.pVecSModels);

			if (!lOptSelectedModel.has_value())
			{
				std::cout << pSPromptPrefix << "> invalid model selection.\n";
				continue;
			}

			std::string lSError;

			if (!lCliSession.pPtrEngine->Load(*lOptSelectedModel, &lSError))
			{
				std::cout << pSPromptPrefix << "> failed to load model '" << *lOptSelectedModel << "': " << lSError << "\n";
				continue;
			}

			lCliSession.pSLoadedModelName = *lOptSelectedModel;
			break;
		}

		std::cout << pSPromptPrefix << "> loaded model: " << lCliSession.pSLoadedModelName << "\n";
		*pCliSessionOut = std::move(lCliSession);
		return true;
	}

} // namespace ollama_engine_cli::common
