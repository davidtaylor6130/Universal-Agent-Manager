#include "local_ollama_engine_private.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
#include "llama.h"
#endif

namespace ollama_engine::internal
{

	std::string ToLowerAscii(std::string pSInput)
	{
		std::transform(pSInput.begin(), pSInput.end(), pSInput.begin(), [](const unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });
		return pSInput;
	}

	GenerationSettings ClampGenerationSettings(const GenerationSettings& pGenerationSettings)
	{
		GenerationSettings lGenerationSettings = pGenerationSettings;
		lGenerationSettings.pfTemperature = std::clamp(lGenerationSettings.pfTemperature, 0.0f, 2.0f);
		lGenerationSettings.pfTopP = std::clamp(lGenerationSettings.pfTopP, 0.05f, 1.0f);
		lGenerationSettings.pfMinP = std::clamp(lGenerationSettings.pfMinP, 0.0f, 1.0f);

		if (lGenerationSettings.pfMinP > lGenerationSettings.pfTopP)
		{
			lGenerationSettings.pfMinP = lGenerationSettings.pfTopP;
		}

		lGenerationSettings.piTopK = std::clamp(lGenerationSettings.piTopK, 1, 200);
		lGenerationSettings.pfRepeatPenalty = std::clamp(lGenerationSettings.pfRepeatPenalty, 0.8f, 2.0f);
		return lGenerationSettings;
	}

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
	namespace
	{
		std::once_flag g_OnceLlamaRuntimeInit;

		void LlamaLogCallback(enum ggml_log_level pLogLevel, const char* pSMessage, void* pPtrUserData)
		{
			(void)pPtrUserData;

			if (pLogLevel >= GGML_LOG_LEVEL_ERROR && pSMessage != nullptr)
			{
				std::fputs(pSMessage, stderr);
			}
		}
	} // namespace

	bool LlamaLoadProgressCallback(float pfProgress, void* pPtrUserData)
	{
		(void)pfProgress;
		(void)pPtrUserData;
		return true;
	}

	void EnsureLlamaRuntimeInitialized()
	{
		const auto lFnInitializeLlamaRuntime = []()
		{
			llama_log_set(LlamaLogCallback, nullptr);
			llama_backend_init();
			ggml_backend_load_all();
		};
		std::call_once(g_OnceLlamaRuntimeInit, lFnInitializeLlamaRuntime);
	}

	std::optional<std::string> BuildPromptWithChatTemplate(const llama_model* pPtrModel, const std::string& pSPrompt)
	{
		if (pPtrModel == nullptr)
		{
			return std::nullopt;
		}

		const char* lPtrTemplate = llama_model_chat_template(pPtrModel, nullptr);

		if (lPtrTemplate == nullptr)
		{
			return pSPrompt;
		}

		llama_chat_message lChatMessage{};
		lChatMessage.role = "user";
		lChatMessage.content = pSPrompt.c_str();

		std::vector<char> lVecCBuffer(std::max<std::size_t>(1024, (pSPrompt.size() * 2) + 512));
		int32_t liWrittenBytes = llama_chat_apply_template(lPtrTemplate, &lChatMessage, 1, true, lVecCBuffer.data(), lVecCBuffer.size());

		if (liWrittenBytes > static_cast<int32_t>(lVecCBuffer.size()))
		{
			lVecCBuffer.resize(static_cast<std::size_t>(liWrittenBytes));
			liWrittenBytes = llama_chat_apply_template(lPtrTemplate, &lChatMessage, 1, true, lVecCBuffer.data(), lVecCBuffer.size());
		}

		if (liWrittenBytes < 0)
		{
			return std::nullopt;
		}

		return std::string(lVecCBuffer.data(), static_cast<std::size_t>(liWrittenBytes));
	}

	llama_sampler* BuildSamplerFromGenerationSettings(const GenerationSettings& pGenerationSettings)
	{
		llama_sampler_chain_params lSamplerChainParams = llama_sampler_chain_default_params();
		llama_sampler* lPtrSamplerChain = llama_sampler_chain_init(lSamplerChainParams);

		if (lPtrSamplerChain == nullptr)
		{
			return nullptr;
		}

		auto AddOwnedSamplerOrFail = [&](llama_sampler* pPtrSampler) -> bool
		{
			if (pPtrSampler == nullptr)
			{
				llama_sampler_free(lPtrSamplerChain);
				return false;
			}

			llama_sampler_chain_add(lPtrSamplerChain, pPtrSampler);
			return true;
		};

		if (!AddOwnedSamplerOrFail(llama_sampler_init_top_k(pGenerationSettings.piTopK)) || !AddOwnedSamplerOrFail(llama_sampler_init_top_p(pGenerationSettings.pfTopP, 1)) || !AddOwnedSamplerOrFail(llama_sampler_init_min_p(pGenerationSettings.pfMinP, 1)) || !AddOwnedSamplerOrFail(llama_sampler_init_penalties(64, pGenerationSettings.pfRepeatPenalty, 0.0f, 0.0f)))
		{
			return nullptr;
		}

		if (pGenerationSettings.pfTemperature <= 0.0f)
		{
			if (!AddOwnedSamplerOrFail(llama_sampler_init_greedy()))
			{
				return nullptr;
			}
		}
		else
		{
			if (!AddOwnedSamplerOrFail(llama_sampler_init_temp(pGenerationSettings.pfTemperature)) || !AddOwnedSamplerOrFail(llama_sampler_init_dist(pGenerationSettings.piSeed)))
			{
				return nullptr;
			}
		}

		return lPtrSamplerChain;
	}
#endif

} // namespace ollama_engine::internal
