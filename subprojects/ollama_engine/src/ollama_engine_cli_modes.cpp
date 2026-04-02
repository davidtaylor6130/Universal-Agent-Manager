#include "ollama_engine/cli_modes.h"

#include "ollama_engine_cli_autotune_internal.h"
#include "ollama_engine_cli_common_internal.h"
#include "ollama_engine_cli_tests_internal.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace
{

	void TryAutoApplyFinalProfile(ollama_engine_cli::common::CliSession* pCliSessionInOut, const bool pbPrintNoMatchMessage)
	{
		if (pCliSessionInOut == nullptr || !pCliSessionInOut->pPtrEngine)
		{
			return;
		}

		const std::filesystem::path lPathFinalProfileDirectory = ollama_engine_cli::autotune::GetAutoTuneFinalProfileDirectory(pCliSessionInOut->pPathModelFolder);
		const std::vector<ollama_engine_cli::autotune::AutoTuneTemplate> lVecAutoTuneFinalProfiles = ollama_engine_cli::autotune::LoadAutoTuneFinalProfiles(lPathFinalProfileDirectory);
		const std::string lSHardwareFingerprint = ollama_engine_cli::autotune::BuildHardwareFingerprint();
		const std::optional<ollama_engine_cli::autotune::AutoTuneTemplate> lOptAutoProfile = ollama_engine_cli::autotune::FindBestMatchingTemplate(lVecAutoTuneFinalProfiles, pCliSessionInOut->pSLoadedModelName, lSHardwareFingerprint);

		if (!lOptAutoProfile.has_value())
		{
			if (pbPrintNoMatchMessage)
			{
				std::cout << "autotune> no hardware/model-matching finalized profile found.\n";
			}

			return;
		}

		std::string lSError;

		if (ollama_engine_cli::autotune::ApplyAutoTuneTemplate(pCliSessionInOut->pPtrEngine.get(), *lOptAutoProfile, &pCliSessionInOut->pSLoadedModelName, &lSError))
		{
			std::cout << "autotune> auto-applied finalized profile '" << lOptAutoProfile->pSName << "'\n";
			std::cout << "autotune> " << ollama_engine_cli::autotune::DescribeGenerationSettings(pCliSessionInOut->pPtrEngine->GetGenerationSettings()) << "\n";
			return;
		}

		std::cout << "autotune> found matching finalized profile '" << lOptAutoProfile->pSName << "' but failed to apply: " << lSError << "\n";
	}

	std::optional<std::filesystem::path> ResolveCustomTestsDirectory(int argc, char** argv)
	{
		if (argc > 2 && argv[2] != nullptr && std::string(argv[2]).empty() == false)
		{
			return std::filesystem::path(argv[2]);
		}

		const std::string lSInput = ollama_engine_cli::common::Trim(ollama_engine_cli::common::ReadLine("tests> path to Question_N test files (/quit to exit): "));

		if (!std::cin.good() || lSInput.empty() || lSInput == "/quit")
		{
			return std::nullopt;
		}

		return std::filesystem::path(lSInput);
	}

} // namespace

namespace ollama_engine_cli
{

	int RunBasicLoadAndChatCli(int argc, char** argv)
	{
		common::CliSession lCliSession;
		int liExitCode = 0;

		if (!common::BootstrapCliSession(argc, argv, "chat", &lCliSession, &liExitCode))
		{
			return liExitCode;
		}

		TryAutoApplyFinalProfile(&lCliSession, true);
		std::cout << "chat> ready. Type /quit to exit.\n";

		while (true)
		{
			const std::string lSPrompt = common::ReadLine("you> ");

			if (!std::cin.good() || lSPrompt == "/quit")
			{
				break;
			}

			if (lSPrompt.empty())
			{
				continue;
			}

			const common::PromptRunResult lPromptRunResult = common::RunPromptWithMetrics(lCliSession.pPtrEngine.get(), lSPrompt);

			if (!lPromptRunResult.pSendMessageResponse.pbOk)
			{
				std::cout << "engine> error: " << lPromptRunResult.pSendMessageResponse.pSError << "\n";
				continue;
			}

			std::cout << "engine> " << lPromptRunResult.pSendMessageResponse.pSText << "\n";
			common::PrintMetrics(lPromptRunResult);
		}

		return 0;
	}

	int RunFinetuneWizardCli(int argc, char** argv)
	{
		common::CliSession lCliSession;
		int liExitCode = 0;

		if (!common::BootstrapCliSession(argc, argv, "autotune", &lCliSession, &liExitCode))
		{
			return liExitCode;
		}

		autotune::RunAutoTuneWizard(lCliSession.pPtrEngine.get(), lCliSession.pVecSModels, lCliSession.pPathModelFolder, &lCliSession.pSLoadedModelName);
		return 0;
	}

	int RunAutoTestCli(int argc, char** argv)
	{
		common::CliSession lCliSession;
		int liExitCode = 0;

		if (!common::BootstrapCliSession(argc, argv, "bench", &lCliSession, &liExitCode))
		{
			return liExitCode;
		}

		TryAutoApplyFinalProfile(&lCliSession, false);
		tests::RunStandardOpenSourceBenchmarks(lCliSession.pPtrEngine.get());
		return 0;
	}

	int RunCustomTestsCli(int argc, char** argv)
	{
		common::CliSession lCliSession;
		int liExitCode = 0;

		if (!common::BootstrapCliSession(argc, argv, "tests", &lCliSession, &liExitCode))
		{
			return liExitCode;
		}

		TryAutoApplyFinalProfile(&lCliSession, false);
		const std::optional<std::filesystem::path> lOptTestsDirectory = ResolveCustomTestsDirectory(argc, argv);

		if (!lOptTestsDirectory.has_value())
		{
			std::cout << "tests> usage: [model_folder] [question_answer_directory]\n";
			return 1;
		}

		const bool lbAllPassed = tests::RunQuestionAnswerTests(lCliSession.pPtrEngine.get(), *lOptTestsDirectory);
		std::cout << "tests> overall: " << (lbAllPassed ? "PASS" : "FAIL") << "\n";
		return lbAllPassed ? 0 : 2;
	}

} // namespace ollama_engine_cli
