#pragma once

#include "ollama_engine/engine_structures.h"

#include "../vectorised_rag/vectorised_rag.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
#include "llama.h"
#endif

namespace ollama_engine::internal
{

	std::string ToLowerAscii(std::string pSInput);

	GenerationSettings ClampGenerationSettings(const GenerationSettings& pGenerationSettings);

	RagRuntimeMode ResolveRagRuntimeMode(RagRuntimeMode pRagRuntimeMode);

	vectorised_rag::RuntimeOptions BuildVectorisedRagRuntimeOptions(const EngineOptions& pEngineOptions, const std::filesystem::path& pPathLoadedModelFile, const std::string& pSRagOutputDatabaseName);

	std::vector<std::filesystem::path> DiscoverModelFilesRecursively(const std::filesystem::path& pPathRootDirectory);

	struct RagRuntimeInterface
	{
		virtual ~RagRuntimeInterface() = default;
		virtual bool Scan(const std::optional<std::string>& pOptSVectorFile, const vectorised_rag::RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut) = 0;
		virtual bool LoadRagDatabases(const std::vector<std::string>& pVecSDatabaseInputs, const vectorised_rag::RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut) = 0;
		virtual std::vector<std::string> Fetch_Relevant_Info(const std::string& pSPrompt, std::size_t piMax, std::size_t piMin, const vectorised_rag::RuntimeOptions& pRuntimeOptions) = 0;
		virtual VectorisationStateResponse Fetch_state() = 0;
	};

	std::unique_ptr<RagRuntimeInterface> CreateRagRuntime(RagRuntimeMode pRagRuntimeMode);

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
	void EnsureLlamaRuntimeInitialized();

	bool LlamaLoadProgressCallback(float pfProgress, void* pPtrUserData);

	std::optional<std::string> BuildPromptWithChatTemplate(const llama_model* pPtrModel, const std::string& pSPrompt);

	llama_sampler* BuildSamplerFromGenerationSettings(const GenerationSettings& pGenerationSettings);

	enum class SampledTokenBehavior
	{
		Stop,
		Thinking,
		Hidden,
		Emit
	};

	std::optional<std::string> TokenToPieceText(const llama_vocab* pPtrVocab, llama_token piToken);

	SampledTokenBehavior ClassifySampledToken(const llama_vocab* pPtrVocab, llama_token piToken);

	std::string GetNormalizedTokenMarkerText(const llama_vocab* pPtrVocab, llama_token piToken);

	bool IsThinkingStartTokenText(const std::string& pSNormalizedTokenText);

	bool IsThinkingEndTokenText(const std::string& pSNormalizedTokenText);

	std::optional<llama_token> SelectBestFallbackToken(const llama_vocab* pPtrVocab, llama_context* pPtrContext);

	std::string StripReasoningSections(const std::string& pSText);
#endif

} // namespace ollama_engine::internal
