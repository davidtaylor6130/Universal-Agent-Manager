#include "local_ollama_engine_private.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ollama_engine::internal
{

	RagRuntimeMode ResolveRagRuntimeMode(const RagRuntimeMode pRagRuntimeMode)
	{
		const char* lPtrModeEnv = std::getenv("UAM_RAG_MODE");

		if (lPtrModeEnv == nullptr || *lPtrModeEnv == '\0')
		{
			return pRagRuntimeMode;
		}

		const std::string lSMode = ToLowerAscii(lPtrModeEnv);

		if (lSMode == "deterministic")
		{
			return RagRuntimeMode::Deterministic;
		}

		if (lSMode == "vectorised" || lSMode == "vectorized")
		{
			return RagRuntimeMode::Vectorised;
		}

		return pRagRuntimeMode;
	}

	vectorised_rag::RuntimeOptions BuildVectorisedRagRuntimeOptions(const EngineOptions& pEngineOptions, const std::filesystem::path& pPathLoadedModelFile, const std::string& pSRagOutputDatabaseName)
	{
		vectorised_rag::RuntimeOptions lRuntimeOptions;
		lRuntimeOptions.pPathModelFolder = pEngineOptions.pPathModelFolder;
		lRuntimeOptions.pPathEmbeddingModelFile = pPathLoadedModelFile;
		lRuntimeOptions.piDeterministicEmbeddingDimensions = pEngineOptions.piEmbeddingDimensions;
		lRuntimeOptions.piEmbeddingMaxTokens = pEngineOptions.piEmbeddingMaxTokens;
		lRuntimeOptions.pSDatabaseName = pSRagOutputDatabaseName;
		const char* lPtrServerUrl = std::getenv("UAM_LLAMA_SERVER_URL");

		if (lPtrServerUrl != nullptr && *lPtrServerUrl != '\0')
		{
			lRuntimeOptions.pSLlamaServerUrl = lPtrServerUrl;
		}

		return lRuntimeOptions;
	}

	namespace
	{
		VectorisationStateResponse ToVectorisationStateResponse(const vectorised_rag::ScanStateSnapshot& pScanStateSnapshot)
		{
			VectorisationStateResponse lVectorisationStateResponse;
			lVectorisationStateResponse.piVectorDatabaseSize = pScanStateSnapshot.piVectorDatabaseSize;
			lVectorisationStateResponse.piFilesProcessed = pScanStateSnapshot.piFilesProcessed;
			lVectorisationStateResponse.piTotalFiles = pScanStateSnapshot.piTotalFiles;

			switch (pScanStateSnapshot.pState)
			{
			case vectorised_rag::StateValue::Running:
				lVectorisationStateResponse.pVectorisationLifecycleState = VectorisationLifecycleState::Running;
				break;
			case vectorised_rag::StateValue::Finished:
				lVectorisationStateResponse.pVectorisationLifecycleState = VectorisationLifecycleState::Finished;
				break;
			case vectorised_rag::StateValue::Stopped:
			default:
				lVectorisationStateResponse.pVectorisationLifecycleState = VectorisationLifecycleState::Stopped;
				break;
			}

			return lVectorisationStateResponse;
		}

		class VectorisedRagRuntime final : public RagRuntimeInterface
		{
		  public:
			~VectorisedRagRuntime() override
			{
				vectorised_rag::Shutdown(mContext);
			}

			bool Scan(const std::optional<std::string>& pOptSVectorFile, const vectorised_rag::RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut) override
			{
				vectorised_rag::RuntimeOptions lRuntimeOptions = pRuntimeOptions;
				lRuntimeOptions.pbUseDeterministicEmbeddings = false;
				lRuntimeOptions.pSStorageFolderName = ".vectorised_rag";
				return vectorised_rag::Scan(mContext, pOptSVectorFile, lRuntimeOptions, pSErrorOut);
			}

			bool LoadRagDatabases(const std::vector<std::string>& pVecSDatabaseInputs, const vectorised_rag::RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut) override
			{
				vectorised_rag::RuntimeOptions lRuntimeOptions = pRuntimeOptions;
				lRuntimeOptions.pbUseDeterministicEmbeddings = false;
				lRuntimeOptions.pSStorageFolderName = ".vectorised_rag";
				return vectorised_rag::LoadRagDatabases(mContext, pVecSDatabaseInputs, lRuntimeOptions, pSErrorOut);
			}

			std::vector<std::string> Fetch_Relevant_Info(const std::string& pSPrompt, const std::size_t piMax, const std::size_t piMin, const vectorised_rag::RuntimeOptions& pRuntimeOptions) override
			{
				vectorised_rag::RuntimeOptions lRuntimeOptions = pRuntimeOptions;
				lRuntimeOptions.pbUseDeterministicEmbeddings = false;
				lRuntimeOptions.pSStorageFolderName = ".vectorised_rag";
				return vectorised_rag::Fetch_Relevant_Info(mContext, pSPrompt, piMax, piMin, lRuntimeOptions);
			}

			VectorisationStateResponse Fetch_state() override
			{
				return ToVectorisationStateResponse(vectorised_rag::Fetch_state(mContext));
			}

		  private:
			vectorised_rag::Context mContext;
		};

		class DeterministicRagRuntime final : public RagRuntimeInterface
		{
		  public:
			~DeterministicRagRuntime() override
			{
				vectorised_rag::Shutdown(mContext);
			}

			bool Scan(const std::optional<std::string>& pOptSVectorFile, const vectorised_rag::RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut) override
			{
				vectorised_rag::RuntimeOptions lRuntimeOptions = pRuntimeOptions;
				lRuntimeOptions.pbUseDeterministicEmbeddings = true;
				lRuntimeOptions.pSStorageFolderName = ".deterministic_rag";
				return vectorised_rag::Scan(mContext, pOptSVectorFile, lRuntimeOptions, pSErrorOut);
			}

			bool LoadRagDatabases(const std::vector<std::string>& pVecSDatabaseInputs, const vectorised_rag::RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut) override
			{
				vectorised_rag::RuntimeOptions lRuntimeOptions = pRuntimeOptions;
				lRuntimeOptions.pbUseDeterministicEmbeddings = true;
				lRuntimeOptions.pSStorageFolderName = ".deterministic_rag";
				return vectorised_rag::LoadRagDatabases(mContext, pVecSDatabaseInputs, lRuntimeOptions, pSErrorOut);
			}

			std::vector<std::string> Fetch_Relevant_Info(const std::string& pSPrompt, const std::size_t piMax, const std::size_t piMin, const vectorised_rag::RuntimeOptions& pRuntimeOptions) override
			{
				vectorised_rag::RuntimeOptions lRuntimeOptions = pRuntimeOptions;
				lRuntimeOptions.pbUseDeterministicEmbeddings = true;
				lRuntimeOptions.pSStorageFolderName = ".deterministic_rag";
				return vectorised_rag::Fetch_Relevant_Info(mContext, pSPrompt, piMax, piMin, lRuntimeOptions);
			}

			VectorisationStateResponse Fetch_state() override
			{
				return ToVectorisationStateResponse(vectorised_rag::Fetch_state(mContext));
			}

		  private:
			vectorised_rag::Context mContext;
		};
	} // namespace

	std::unique_ptr<RagRuntimeInterface> CreateRagRuntime(const RagRuntimeMode pRagRuntimeMode)
	{
		if (pRagRuntimeMode == RagRuntimeMode::Deterministic)
		{
			return std::make_unique<DeterministicRagRuntime>();
		}

		return std::make_unique<VectorisedRagRuntime>();
	}

} // namespace ollama_engine::internal
