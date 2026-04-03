#include "local_ollama_engine.h"

#include "local_ollama_engine/local_ollama_engine_private.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ollama_engine::internal
{
	namespace fs = std::filesystem;

	namespace
	{
		bool IsAllowedDatabaseNameCharacter(const char pCChar)
		{
			const unsigned char lCUChar = static_cast<unsigned char>(pCChar);
			return std::isalnum(lCUChar) != 0 || pCChar == '_' || pCChar == '-' || pCChar == '.';
		}

		bool IsValidRagDatabaseName(const std::string& pSDatabaseName)
		{
			if (pSDatabaseName.empty())
			{
				return true;
			}

			return std::all_of(pSDatabaseName.begin(), pSDatabaseName.end(), [](const char pCChar) { return IsAllowedDatabaseNameCharacter(pCChar); });
		}
	} // namespace

	/// <summary>
	/// Initializes the local engine and normalizes runtime options.
	/// </summary>
	/// <param name="pEngineOptions">Runtime options used by this engine instance.</param>
	LocalOllamaCppEngine::LocalOllamaCppEngine(EngineOptions pEngineOptions) : mEngineOptions(std::move(pEngineOptions))
	{
		if (mEngineOptions.pPathModelFolder.empty())
		{
			mEngineOptions.pPathModelFolder = fs::current_path() / "models";
		}

		mEngineOptions.piEmbeddingDimensions = std::clamp<std::size_t>(mEngineOptions.piEmbeddingDimensions, 32, 4096);
		mEngineOptions.piEmbeddingMaxTokens = std::clamp<std::size_t>(mEngineOptions.piEmbeddingMaxTokens, 0, 32768);
		mGenerationSettings = ClampGenerationSettings(mEngineOptions.pGenerationSettings);
		mEngineOptions.pGenerationSettings = mGenerationSettings;
		mEngineOptions.pRagRuntimeMode = ResolveRagRuntimeMode(mEngineOptions.pRagRuntimeMode);
		mPtrRagRuntime = CreateRagRuntime(mEngineOptions.pRagRuntimeMode);
	}

	/// <summary>Releases loaded runtime resources.</summary>
	LocalOllamaCppEngine::~LocalOllamaCppEngine()
	{
		mPtrRagRuntime.reset();
		std::lock_guard<std::mutex> lGuard(mMutexEngineRuntime);
		ReleaseRuntimeLocked();
	}

	/// <summary>Enumerates model files from the configured model folder.</summary>
	/// <returns>Sorted model file names.</returns>
	std::vector<std::string> LocalOllamaCppEngine::ListModels()
	{
		std::vector<std::string> lVecSModels;
		const std::vector<fs::path> lVecPathModelFiles = DiscoverModelFilesRecursively(mEngineOptions.pPathModelFolder);
		std::error_code lErrorCode;

		for (const fs::path& lPathModelFile : lVecPathModelFiles)
		{
			const fs::path lPathRelative = fs::relative(lPathModelFile, mEngineOptions.pPathModelFolder, lErrorCode);

			if (lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			lVecSModels.push_back(lPathRelative.generic_string());
		}

		std::sort(lVecSModels.begin(), lVecSModels.end());
		return lVecSModels;
	}

	/// <summary>Frees active llama model/context/sampler pointers.</summary>
	void LocalOllamaCppEngine::ReleaseRuntimeLocked()
	{
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP

		if (mPtrSampler != nullptr)
		{
			llama_sampler_free(mPtrSampler);
			mPtrSampler = nullptr;
		}

		if (mPtrContext != nullptr)
		{
			llama_free(mPtrContext);
			mPtrContext = nullptr;
		}

		if (mPtrModel != nullptr)
		{
			llama_model_free(mPtrModel);
			mPtrModel = nullptr;
		}

		mPathLoadedModelFile.clear();
#endif
	}

	/// <summary>Returns a thread-safe snapshot of current engine state.</summary>
	/// <returns>Current lifecycle/progress state.</returns>
	CurrentStateResponse LocalOllamaCppEngine::QueryCurrentState() const
	{
		std::lock_guard<std::mutex> lGuard(mMutexCurrentState);
		return mCurrentStateResponse;
	}

	/// <summary>Starts asynchronous repository scan + vectorisation.</summary>
	/// <param name="pOptSVectorFile">Optional scan target path/URL.</param>
	/// <param name="pSErrorOut">Optional output pointer for error details.</param>
	/// <returns>True when the scan worker starts successfully.</returns>
	bool LocalOllamaCppEngine::Scan(const std::optional<std::string>& pOptSVectorFile, std::string* pSErrorOut)
	{
		if (mPtrRagRuntime == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "RAG runtime is not initialized.";
			}

			return false;
		}

		vectorised_rag::RuntimeOptions lRuntimeOptions;
		{
			std::lock_guard<std::mutex> lGuard(mMutexEngineRuntime);
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
			lRuntimeOptions = BuildVectorisedRagRuntimeOptions(mEngineOptions, mPathLoadedModelFile, ms_RagOutputDatabaseName);
#else
			lRuntimeOptions = BuildVectorisedRagRuntimeOptions(mEngineOptions, std::filesystem::path{}, ms_RagOutputDatabaseName);
#endif
		}

		return mPtrRagRuntime->Scan(pOptSVectorFile, lRuntimeOptions, pSErrorOut);
	}

	bool LocalOllamaCppEngine::SetRagOutputDatabase(const std::string& pSDatabaseName, std::string* pSErrorOut)
	{
		if (!IsValidRagDatabaseName(pSDatabaseName))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Invalid database name. Only [A-Za-z0-9._-] are allowed. Use empty string to reset default naming.";
			}

			return false;
		}

		std::lock_guard<std::mutex> lGuard(mMutexEngineRuntime);
		ms_RagOutputDatabaseName = pSDatabaseName;
		return true;
	}

	bool LocalOllamaCppEngine::LoadRagDatabases(const std::vector<std::string>& pVecSDatabaseInputs, std::string* pSErrorOut)
	{
		if (mPtrRagRuntime == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "RAG runtime is not initialized.";
			}

			return false;
		}

		vectorised_rag::RuntimeOptions lRuntimeOptions;
		{
			std::lock_guard<std::mutex> lGuard(mMutexEngineRuntime);
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
			lRuntimeOptions = BuildVectorisedRagRuntimeOptions(mEngineOptions, mPathLoadedModelFile, ms_RagOutputDatabaseName);
#else
			lRuntimeOptions = BuildVectorisedRagRuntimeOptions(mEngineOptions, std::filesystem::path{}, ms_RagOutputDatabaseName);
#endif
		}

		return mPtrRagRuntime->LoadRagDatabases(pVecSDatabaseInputs, lRuntimeOptions, pSErrorOut);
	}

	/// <summary>Fetches semantically relevant snippets from the vectorised index.</summary>
	/// <param name="pSPrompt">Query prompt.</param>
	/// <param name="piMax">Maximum material to return.</param>
	/// <param name="piMin">Minimum material to return.</param>
	/// <returns>Snippet list.</returns>
	std::vector<std::string> LocalOllamaCppEngine::Fetch_Relevant_Info(const std::string& pSPrompt, const std::size_t piMax, const std::size_t piMin)
	{
		if (mPtrRagRuntime == nullptr)
		{
			return {};
		}

		vectorised_rag::RuntimeOptions lRuntimeOptions;
		{
			std::lock_guard<std::mutex> lGuard(mMutexEngineRuntime);
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
			lRuntimeOptions = BuildVectorisedRagRuntimeOptions(mEngineOptions, mPathLoadedModelFile, ms_RagOutputDatabaseName);
#else
			lRuntimeOptions = BuildVectorisedRagRuntimeOptions(mEngineOptions, std::filesystem::path{}, ms_RagOutputDatabaseName);
#endif
		}

		return mPtrRagRuntime->Fetch_Relevant_Info(pSPrompt, piMax, piMin, lRuntimeOptions);
	}

	/// <summary>Returns current vectorisation state.</summary>
	/// <returns>Vectorisation state response snapshot.</returns>
	VectorisationStateResponse LocalOllamaCppEngine::Fetch_state()
	{
		return (mPtrRagRuntime != nullptr) ? mPtrRagRuntime->Fetch_state() : VectorisationStateResponse{};
	}

} // namespace ollama_engine::internal
