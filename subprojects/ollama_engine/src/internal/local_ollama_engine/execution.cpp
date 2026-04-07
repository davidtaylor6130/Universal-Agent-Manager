#include "../local_ollama_engine.h"

#include "local_ollama_engine_private.h"

#include "../embedding_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
#include "llama.h"
#endif

namespace ollama_engine::internal
{

	namespace
	{
		namespace fs = std::filesystem;

		constexpr std::size_t ki_DefaultContextTokens = 2048;
		constexpr std::size_t ki_DefaultMaxGeneratedTokens = 256;
		constexpr int32_t ki_DefaultGpuLayers = 99;

		bool IsHiddenFileName(const std::string& pSFileName)
		{
			return !pSFileName.empty() && pSFileName.front() == '.';
		}

		bool IsSupportedModelFile(const fs::path& pPathFile)
		{
			return ToLowerAscii(pPathFile.extension().string()) == ".gguf";
		}

		std::string TrimAsciiWhitespace(const std::string& pSInput)
		{
			std::size_t liBegin = 0;

			while (liBegin < pSInput.size() && std::isspace(static_cast<unsigned char>(pSInput[liBegin])) != 0)
			{
				++liBegin;
			}

			std::size_t liEnd = pSInput.size();

			while (liEnd > liBegin && std::isspace(static_cast<unsigned char>(pSInput[liEnd - 1])) != 0)
			{
				--liEnd;
			}

			return pSInput.substr(liBegin, liEnd - liBegin);
		}
	} // namespace

	std::vector<fs::path> DiscoverModelFilesRecursively(const fs::path& pPathRootDirectory)
	{
		std::vector<fs::path> lVecPathModelFiles;
		std::error_code lErrorCode;

		if (!fs::exists(pPathRootDirectory, lErrorCode) || !fs::is_directory(pPathRootDirectory, lErrorCode))
		{
			return lVecPathModelFiles;
		}

		const fs::directory_options lDirectoryOptions = fs::directory_options::skip_permission_denied;
		fs::recursive_directory_iterator lIterator(pPathRootDirectory, lDirectoryOptions, lErrorCode);
		const fs::recursive_directory_iterator lEndIterator;

		for (; lIterator != lEndIterator; lIterator.increment(lErrorCode))
		{
			if (lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			const fs::directory_entry& lDirectoryEntry = *lIterator;
			const std::string lSName = lDirectoryEntry.path().filename().string();

			if (IsHiddenFileName(lSName))
			{
				if (lDirectoryEntry.is_directory(lErrorCode) && !lErrorCode)
				{
					lIterator.disable_recursion_pending();
				}

				continue;
			}

			if (!lDirectoryEntry.is_regular_file(lErrorCode) || lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			if (!IsSupportedModelFile(lDirectoryEntry.path()))
			{
				continue;
			}

			lVecPathModelFiles.push_back(lDirectoryEntry.path());
		}

		std::sort(lVecPathModelFiles.begin(), lVecPathModelFiles.end());
		return lVecPathModelFiles;
	}

	/// <summary>Loads a model file and updates progress state.</summary>
	/// <param name="pSModelName">Model file name in the configured model folder.</param>
	/// <param name="pSErrorOut">Optional output pointer for error details.</param>
	/// <returns>True when the model load succeeds.</returns>
	bool LocalOllamaCppEngine::Load(const std::string& pSModelName, std::string* pSErrorOut)
	{
		if (pSModelName.empty())
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Model name is empty.";
			}

			return false;
		}

		const fs::path lPathModelCandidate = mEngineOptions.pPathModelFolder / pSModelName;
		fs::path lPathModel = lPathModelCandidate;
		std::string lSModelDisplayName = pSModelName;
		std::error_code lErrorCode;

		if (!fs::exists(lPathModelCandidate, lErrorCode))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Model not found in model folder: " + pSModelName;
			}

			return false;
		}

		lErrorCode.clear();

		if (fs::is_directory(lPathModelCandidate, lErrorCode) && !lErrorCode)
		{
			const std::vector<fs::path> lVecPathModelFiles = DiscoverModelFilesRecursively(lPathModelCandidate);

			if (lVecPathModelFiles.empty())
			{
				if (pSErrorOut != nullptr)
				{
					*pSErrorOut = "No .gguf model file found inside directory: " + pSModelName;
				}

				return false;
			}

			lPathModel = lVecPathModelFiles.front();
		}
		else if (!fs::is_regular_file(lPathModelCandidate, lErrorCode) || lErrorCode)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Model selection is not a regular file: " + pSModelName;
			}

			return false;
		}

		if (!IsSupportedModelFile(lPathModel))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Unsupported model extension. Expected .gguf";
			}

			return false;
		}

		const fs::path lPathRelativeModel = fs::relative(lPathModel, mEngineOptions.pPathModelFolder, lErrorCode);

		if (!lErrorCode && !lPathRelativeModel.empty())
		{
			lSModelDisplayName = lPathRelativeModel.generic_string();
		}

		const std::size_t liFileUnits = static_cast<std::size_t>(fs::file_size(lPathModel, lErrorCode));
		const std::size_t liTotalUnits = (lErrorCode || liFileUnits == 0) ? static_cast<std::size_t>(1) : liFileUnits;

		std::lock_guard<std::mutex> lRuntimeGuard(mMutexEngineRuntime);
		ReleaseRuntimeLocked();

		{
			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loading;
			mCurrentStateResponse.pSLoadedModelName.clear();
			mCurrentStateResponse.pOptLoadingStructure = LoadingStructure{lSModelDisplayName, 0, liTotalUnits, "Loading model from disk"};
			mCurrentStateResponse.pOptRunningStructure.reset();
		}

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
		EnsureLlamaRuntimeInitialized();

		llama_model_params lModelParams = llama_model_default_params();
		lModelParams.n_gpu_layers = ki_DefaultGpuLayers;
		lModelParams.progress_callback = LlamaLoadProgressCallback;
		lModelParams.progress_callback_user_data = nullptr;

		llama_model* lPtrModel = llama_model_load_from_file(lPathModel.string().c_str(), lModelParams);

		if (lPtrModel == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to load GGUF model via llama.cpp: " + lPathModel.string();
			}

			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Idle;
			mCurrentStateResponse.pOptLoadingStructure.reset();
			mCurrentStateResponse.pOptRunningStructure.reset();
			return false;
		}

		llama_context_params lContextParams = llama_context_default_params();
		lContextParams.n_ctx = ki_DefaultContextTokens;
		lContextParams.n_batch = std::min<std::size_t>(ki_DefaultContextTokens, 512);
		lContextParams.n_ubatch = lContextParams.n_batch;

		llama_context* lPtrContext = llama_init_from_model(lPtrModel, lContextParams);

		if (lPtrContext == nullptr)
		{
			llama_model_free(lPtrModel);

			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to create llama.cpp context.";
			}

			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Idle;
			mCurrentStateResponse.pOptLoadingStructure.reset();
			mCurrentStateResponse.pOptRunningStructure.reset();
			return false;
		}

		llama_sampler* lPtrSampler = BuildSamplerFromGenerationSettings(mGenerationSettings);

		if (lPtrSampler == nullptr)
		{
			llama_free(lPtrContext);
			llama_model_free(lPtrModel);

			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to create llama.cpp sampler chain.";
			}

			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Idle;
			mCurrentStateResponse.pOptLoadingStructure.reset();
			mCurrentStateResponse.pOptRunningStructure.reset();
			return false;
		}

		mPtrModel = lPtrModel;
		mPtrContext = lPtrContext;
		mPtrSampler = lPtrSampler;
		mPathLoadedModelFile = lPathModel;

		{
			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			ms_LoadedModelName = lSModelDisplayName;
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
			mCurrentStateResponse.pSLoadedModelName = lSModelDisplayName;
			mCurrentStateResponse.pOptLoadingStructure.reset();
			mCurrentStateResponse.pOptRunningStructure.reset();
		}

		return true;
#else
		(void)lPathModel;
		(void)liTotalUnits;

		if (pSErrorOut != nullptr)
		{
			*pSErrorOut = "llama.cpp is not linked. Reconfigure with UAM_FETCH_LLAMA_CPP=ON.";
		}

		std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
		mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Idle;
		mCurrentStateResponse.pOptLoadingStructure.reset();
		mCurrentStateResponse.pOptRunningStructure.reset();
		return false;
#endif
	}

	/// <summary>Applies generation settings and rebuilds the sampler chain when needed.</summary>
	/// <param name="pGenerationSettings">Requested generation settings.</param>
	/// <param name="pSErrorOut">Optional output pointer for error details.</param>
	/// <returns>True when settings were applied.</returns>
	bool LocalOllamaCppEngine::SetGenerationSettings(const GenerationSettings& pGenerationSettings, std::string* pSErrorOut)
	{
		const GenerationSettings lGenerationSettings = ClampGenerationSettings(pGenerationSettings);
		std::lock_guard<std::mutex> lRuntimeGuard(mMutexEngineRuntime);

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
		llama_sampler* lPtrNewSampler = nullptr;

		if (mPtrModel != nullptr && mPtrContext != nullptr)
		{
			lPtrNewSampler = BuildSamplerFromGenerationSettings(lGenerationSettings);

			if (lPtrNewSampler == nullptr)
			{
				if (pSErrorOut != nullptr)
				{
					*pSErrorOut = "Failed to build sampler chain from generation settings.";
				}

				return false;
			}
		}

		if (lPtrNewSampler != nullptr)
		{
			if (mPtrSampler != nullptr)
			{
				llama_sampler_free(mPtrSampler);
			}

			mPtrSampler = lPtrNewSampler;
		}
#else
		(void)pSErrorOut;
#endif

	mGenerationSettings = lGenerationSettings;
	mEngineOptions.pGenerationSettings = lGenerationSettings;
	return true;
}

GenerationSettings LocalOllamaCppEngine::GetGenerationSettings() const
{
	std::lock_guard<std::mutex> lRuntimeGuard(mMutexEngineRuntime);
	return mGenerationSettings;
}

	/// <summary>Processes a prompt and returns a model response and optional embedding.</summary>
	/// <param name="pSPrompt">Prompt text to process.</param>
	/// <returns>Response payload with status, text, embedding, and error fields.</returns>
	SendMessageResponse LocalOllamaCppEngine::SendMessage(const std::string& pSPrompt)
	{
		SendMessageResponse lSendMessageResponse;

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
		std::lock_guard<std::mutex> lRuntimeGuard(mMutexEngineRuntime);

		if (mPtrModel == nullptr || mPtrContext == nullptr || mPtrSampler == nullptr || ms_LoadedModelName.empty())
		{
			lSendMessageResponse.pSError = "No model is loaded.";
			return lSendMessageResponse;
		}

		const llama_vocab* lPtrVocab = llama_model_get_vocab(mPtrModel);

		if (lPtrVocab == nullptr)
		{
			lSendMessageResponse.pSError = "Loaded model has no vocabulary.";
			return lSendMessageResponse;
		}

		std::string lSPromptForModel = pSPrompt;
		const std::optional<std::string> lOptFormattedPrompt = BuildPromptWithChatTemplate(mPtrModel, pSPrompt);

		if (lOptFormattedPrompt.has_value() && !lOptFormattedPrompt->empty())
		{
			lSPromptForModel = *lOptFormattedPrompt;
		}

		const int32_t liPromptTokenCountWanted = -llama_tokenize(lPtrVocab, lSPromptForModel.c_str(), static_cast<int32_t>(lSPromptForModel.size()), nullptr, 0, true, true);

		if (liPromptTokenCountWanted <= 0)
		{
			lSendMessageResponse.pSError = "Prompt tokenization failed.";
			return lSendMessageResponse;
		}

		std::vector<llama_token> lVeciPromptTokens(static_cast<std::size_t>(liPromptTokenCountWanted));
		const int32_t liPromptTokenCountWritten = llama_tokenize(lPtrVocab, lSPromptForModel.c_str(), static_cast<int32_t>(lSPromptForModel.size()), lVeciPromptTokens.data(), static_cast<int32_t>(lVeciPromptTokens.size()), true, true);

		if (liPromptTokenCountWritten < 0)
		{
			lSendMessageResponse.pSError = "Prompt tokenization write pass failed.";
			return lSendMessageResponse;
		}

		lVeciPromptTokens.resize(static_cast<std::size_t>(liPromptTokenCountWritten));

		const int32_t liContextSize = static_cast<int32_t>(llama_n_ctx(mPtrContext));

		if (liContextSize <= 2)
		{
			lSendMessageResponse.pSError = "Context size is too small for generation.";
			return lSendMessageResponse;
		}

		if (static_cast<int32_t>(lVeciPromptTokens.size()) >= liContextSize)
		{
			const std::size_t liKeepTokenCount = static_cast<std::size_t>(liContextSize - 1);
			const std::size_t liDropTokenCount = lVeciPromptTokens.size() - liKeepTokenCount;
			lVeciPromptTokens.erase(lVeciPromptTokens.begin(), lVeciPromptTokens.begin() + liDropTokenCount);
		}

		llama_memory_clear(llama_get_memory(mPtrContext), true);
		llama_sampler_reset(mPtrSampler);

		const int32_t liDecodeBatchSize = std::max<int32_t>(1, static_cast<int32_t>(llama_n_batch(mPtrContext)));

		const std::size_t liTotalUnits = lVeciPromptTokens.size() + ki_DefaultMaxGeneratedTokens;
		{
			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Running;
			mCurrentStateResponse.pOptRunningStructure = RunningStructure{ms_LoadedModelName, 0, liTotalUnits, "Tokenizing and evaluating prompt"};
			mCurrentStateResponse.pOptLoadingStructure.reset();
		}

		std::size_t liPromptOffset = 0;

		while (liPromptOffset < lVeciPromptTokens.size())
		{
			const std::size_t liRemainingPromptTokens = lVeciPromptTokens.size() - liPromptOffset;
			const std::size_t liChunkSize = std::min<std::size_t>(liRemainingPromptTokens, static_cast<std::size_t>(liDecodeBatchSize));
			llama_batch lPromptBatch = llama_batch_get_one(lVeciPromptTokens.data() + liPromptOffset, static_cast<int32_t>(liChunkSize));

			if (llama_decode(mPtrContext, lPromptBatch) != 0)
			{
				lSendMessageResponse.pSError = "llama_decode failed while evaluating prompt.";
				std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
				mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
				mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
				mCurrentStateResponse.pOptRunningStructure.reset();
				return lSendMessageResponse;
			}

			liPromptOffset += liChunkSize;
			{
				std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);

				if (mCurrentStateResponse.pOptRunningStructure.has_value())
				{
					mCurrentStateResponse.pOptRunningStructure->piProcessedUnits = liPromptOffset;
				}
			}
		}

		std::size_t liProcessedUnits = lVeciPromptTokens.size();
		std::string lSResponseText;
		lSResponseText.reserve(1024);
		bool lbThinkingMode = false;

		auto UpdateGenerationState = [&](const EngineLifecycleState pEngineLifecycleState, const std::string& pSDetail, const std::size_t piProcessedUnits)
		{
			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = pEngineLifecycleState;

			if (mCurrentStateResponse.pOptRunningStructure.has_value())
			{
				mCurrentStateResponse.pOptRunningStructure->piProcessedUnits = piProcessedUnits;
				mCurrentStateResponse.pOptRunningStructure->pSDetail = pSDetail;
			}
		};

		UpdateGenerationState(EngineLifecycleState::Running, "Generating response", liProcessedUnits);

		for (std::size_t liStep = 0; liStep < ki_DefaultMaxGeneratedTokens; ++liStep)
		{
			const llama_pos liSeqPosMax = llama_memory_seq_pos_max(llama_get_memory(mPtrContext), 0);

			if ((liSeqPosMax + 1) >= liContextSize)
			{
				break;
			}

			llama_token liNextToken = llama_sampler_sample(mPtrSampler, mPtrContext, -1);
			SampledTokenBehavior lTokenBehavior = ClassifySampledToken(lPtrVocab, liNextToken);

			if (lTokenBehavior == SampledTokenBehavior::Stop)
			{
				if (liStep == 0 && lSResponseText.empty())
				{
					bool lbRecoveredWithResample = false;
					constexpr int kiMaxResampleAttempts = 4;

					for (int liAttempt = 0; liAttempt < kiMaxResampleAttempts; ++liAttempt)
					{
						const llama_token liRetryToken = llama_sampler_sample(mPtrSampler, mPtrContext, -1);
						const SampledTokenBehavior lRetryBehavior = ClassifySampledToken(lPtrVocab, liRetryToken);

						if (lRetryBehavior != SampledTokenBehavior::Stop)
						{
							liNextToken = liRetryToken;
							lTokenBehavior = lRetryBehavior;
							lbRecoveredWithResample = true;
							break;
						}
					}

					if (!lbRecoveredWithResample)
					{
						const std::optional<llama_token> lOptFallbackToken = SelectBestFallbackToken(lPtrVocab, mPtrContext);

						if (!lOptFallbackToken.has_value())
						{
							break;
						}

						liNextToken = *lOptFallbackToken;
						lTokenBehavior = ClassifySampledToken(lPtrVocab, liNextToken);

						if (lTokenBehavior == SampledTokenBehavior::Stop)
						{
							break;
						}
					}
				}
				else
				{
					break;
				}
			}

			if (lTokenBehavior == SampledTokenBehavior::Thinking)
			{
				const std::string lSThinkingMarker = GetNormalizedTokenMarkerText(lPtrVocab, liNextToken);

				if (IsThinkingStartTokenText(lSThinkingMarker))
				{
					lbThinkingMode = true;
				}
				else if (IsThinkingEndTokenText(lSThinkingMarker))
				{
					lbThinkingMode = false;
				}
				else
				{
					lbThinkingMode = true;
				}
			}

			if (lTokenBehavior == SampledTokenBehavior::Emit && !lbThinkingMode)
			{
				const std::optional<std::string> lOptPiece = TokenToPieceText(lPtrVocab, liNextToken);

				if (!lOptPiece.has_value())
				{
					lSendMessageResponse.pSError = "Failed to decode sampled token bytes.";
					std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
					mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
					mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
					mCurrentStateResponse.pOptRunningStructure.reset();
					return lSendMessageResponse;
				}

				lSResponseText += *lOptPiece;
			}

			++liProcessedUnits;
			UpdateGenerationState(lbThinkingMode ? EngineLifecycleState::Thinking : EngineLifecycleState::Running, lbThinkingMode ? "Reasoning about response" : "Generating response", liProcessedUnits);

			llama_token liTokenForDecode = liNextToken;
			llama_batch lNextTokenBatch = llama_batch_get_one(&liTokenForDecode, 1);

			if (llama_decode(mPtrContext, lNextTokenBatch) != 0)
			{
				lSendMessageResponse.pSError = "llama_decode failed during generation.";
				std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
				mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
				mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
				mCurrentStateResponse.pOptRunningStructure.reset();
				return lSendMessageResponse;
			}
		}

		const std::string lSVisibleResponse = TrimAsciiWhitespace(StripReasoningSections(lSResponseText));
		const std::string lSRawTrimmedResponse = TrimAsciiWhitespace(lSResponseText);

		lSendMessageResponse.pbOk = true;

		if (!lSVisibleResponse.empty())
		{
			lSendMessageResponse.pSText = lSVisibleResponse;
		}
		else if (!lSRawTrimmedResponse.empty())
		{
			lSendMessageResponse.pSText = lSRawTrimmedResponse;
		}
		else
		{
			lSendMessageResponse.pSText = "(no textual response)";
		}

		lSendMessageResponse.pVecfEmbedding = BuildEmbedding(pSPrompt, mEngineOptions.piEmbeddingDimensions);

		{
			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Finished;
			mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;

			if (mCurrentStateResponse.pOptRunningStructure.has_value())
			{
				mCurrentStateResponse.pOptRunningStructure->piProcessedUnits = liProcessedUnits;
				mCurrentStateResponse.pOptRunningStructure->piTotalUnits = liProcessedUnits;
				mCurrentStateResponse.pOptRunningStructure->pSDetail = "Response ready; awaiting fetch";
			}

			mCurrentStateResponse.pOptLoadingStructure.reset();
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(6));

		{
			std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
			mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
			mCurrentStateResponse.pOptRunningStructure.reset();
			mCurrentStateResponse.pOptLoadingStructure.reset();
		}

		return lSendMessageResponse;
#else
		std::string lSModelNameSnapshot;
		{
			std::lock_guard<std::mutex> lGuard(mMutexCurrentState);
			lSModelNameSnapshot = ms_LoadedModelName;

			if (lSModelNameSnapshot.empty())
			{
				lSendMessageResponse.pSError = "No model is loaded.";
				return lSendMessageResponse;
			}

			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Running;
			mCurrentStateResponse.pOptRunningStructure = RunningStructure{lSModelNameSnapshot, 0, pSPrompt.size(), "Running inference"};
			mCurrentStateResponse.pOptLoadingStructure.reset();
		}

		lSendMessageResponse.pbOk = true;
		lSendMessageResponse.pSText = "llama.cpp runtime is not linked in this build.";
		lSendMessageResponse.pVecfEmbedding = BuildEmbedding(pSPrompt, mEngineOptions.piEmbeddingDimensions);

		{
			std::lock_guard<std::mutex> lGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Finished;
			mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;

			if (mCurrentStateResponse.pOptRunningStructure.has_value())
			{
				mCurrentStateResponse.pOptRunningStructure->piProcessedUnits = std::max<std::size_t>(pSPrompt.size(), static_cast<std::size_t>(1));
				mCurrentStateResponse.pOptRunningStructure->piTotalUnits = std::max<std::size_t>(pSPrompt.size(), static_cast<std::size_t>(1));
				mCurrentStateResponse.pOptRunningStructure->pSDetail = "Response ready; awaiting fetch";
			}

			mCurrentStateResponse.pOptLoadingStructure.reset();
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(6));

		{
			std::lock_guard<std::mutex> lGuard(mMutexCurrentState);
			mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
			mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
			mCurrentStateResponse.pOptRunningStructure.reset();
			mCurrentStateResponse.pOptLoadingStructure.reset();
		}

		return lSendMessageResponse;
#endif
	}

} // namespace ollama_engine::internal
