#pragma once

#include "vectorised_rag_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
#include "llama.h"
#endif

namespace ollama_engine::internal::vectorised_rag
{
	namespace fs = std::filesystem;

	inline std::string EscapeJson(const std::string& pSValue)
	{
		std::string lSOut;
		lSOut.reserve(pSValue.size() + 8);

		for (const unsigned char lCChar : pSValue)
		{
			switch (lCChar)
			{
			case '\"':
				lSOut += "\\\"";
				break;
			case '\\':
				lSOut += "\\\\";
				break;
			case '\b':
				lSOut += "\\b";
				break;
			case '\f':
				lSOut += "\\f";
				break;
			case '\n':
				lSOut += "\\n";
				break;
			case '\r':
				lSOut += "\\r";
				break;
			case '\t':
				lSOut += "\\t";
				break;
			default:

				if (lCChar < 0x20)
				{
					const char lArrHex[] = "0123456789abcdef";
					lSOut += "\\u00";
					lSOut.push_back(lArrHex[(lCChar >> 4) & 0x0F]);
					lSOut.push_back(lArrHex[lCChar & 0x0F]);
				}
				else
				{
					lSOut.push_back(static_cast<char>(lCChar));
				}

				break;
			}
		}

		return lSOut;
	}

	inline std::optional<std::vector<float>> ParseEmbeddingFromJson(const std::string& pSJson)
	{
		const std::size_t liEmbeddingKey = pSJson.find("\"embedding\"");

		if (liEmbeddingKey == std::string::npos)
		{
			return std::nullopt;
		}

		const std::size_t liArrayStart = pSJson.find('[', liEmbeddingKey);

		if (liArrayStart == std::string::npos)
		{
			return std::nullopt;
		}

		std::size_t liArrayEnd = std::string::npos;
		int liDepth = 0;

		for (std::size_t liIndex = liArrayStart; liIndex < pSJson.size(); ++liIndex)
		{
			if (pSJson[liIndex] == '[')
			{
				++liDepth;
			}
			else if (pSJson[liIndex] == ']')
			{
				--liDepth;

				if (liDepth == 0)
				{
					liArrayEnd = liIndex;
					break;
				}
			}
		}

		if (liArrayEnd == std::string::npos || liArrayEnd <= liArrayStart + 1)
		{
			return std::nullopt;
		}

		const std::string lSArray = pSJson.substr(liArrayStart + 1, liArrayEnd - liArrayStart - 1);
		std::vector<float> lVecfEmbedding;
		lVecfEmbedding.reserve(512);
		std::string lSToken;

		for (const char lCChar : lSArray)
		{
			const bool lbNumeric = (std::isdigit(static_cast<unsigned char>(lCChar)) != 0) || lCChar == '-' || lCChar == '+' || lCChar == '.' || lCChar == 'e' || lCChar == 'E';

			if (lbNumeric)
			{
				lSToken.push_back(lCChar);
				continue;
			}

			if (!lSToken.empty())
			{
				lVecfEmbedding.push_back(static_cast<float>(std::strtod(lSToken.c_str(), nullptr)));
				lSToken.clear();
			}
		}

		if (!lSToken.empty())
		{
			lVecfEmbedding.push_back(static_cast<float>(std::strtod(lSToken.c_str(), nullptr)));
		}

		if (lVecfEmbedding.empty())
		{
			return std::nullopt;
		}

		return lVecfEmbedding;
	}

	inline std::vector<float> NormalizeEmbedding(std::vector<float> pVecfEmbedding)
	{
		double ldNorm = 0.0;

		for (const float lfValue : pVecfEmbedding)
		{
			ldNorm += static_cast<double>(lfValue) * static_cast<double>(lfValue);
		}

		ldNorm = std::sqrt(ldNorm);

		if (ldNorm > 0.0)
		{
			for (float& lfValue : pVecfEmbedding)
			{
				lfValue = static_cast<float>(static_cast<double>(lfValue) / ldNorm);
			}
		}

		return pVecfEmbedding;
	}

	inline bool IsServerEmbeddingsAvailable(Context& pContext, const std::string& pSServerUrl)
	{
		{
			std::lock_guard<std::mutex> lGuard(pContext.pMutex);

			if (pContext.pbLlamaServerChecked && pContext.pSCheckedLlamaServerUrl == pSServerUrl)
			{
				return pContext.pbLlamaServerAvailable;
			}
		}

		std::string lSOutput;
		const std::string lSHealthCommand = "curl -sS --fail --max-time 2 " + ShellQuote(pSServerUrl + "/health");
		const bool lbAvailable = (RunShellCommand(lSHealthCommand, &lSOutput) == 0);

		std::lock_guard<std::mutex> lGuard(pContext.pMutex);
		pContext.pbLlamaServerChecked = true;
		pContext.pSCheckedLlamaServerUrl = pSServerUrl;
		pContext.pbLlamaServerAvailable = lbAvailable;
		return lbAvailable;
	}

	inline std::optional<std::vector<float>> TryEmbedWithServer(Context& pContext, const RuntimeOptions& pRuntimeOptions, const fs::path& pPathModel, const std::string& pSText)
	{
		const std::string lSServerUrl = pRuntimeOptions.pSLlamaServerUrl.empty() ? "http://127.0.0.1:8080" : pRuntimeOptions.pSLlamaServerUrl;

		if (!IsServerEmbeddingsAvailable(pContext, lSServerUrl))
		{
			return std::nullopt;
		}

		const fs::path lPathPayloadFile = fs::temp_directory_path() / ("uam_rag_payload_" + determanistic_hash::HashTextHex(pSText) + ".json");
		{
			std::ofstream lPayloadOut(lPathPayloadFile, std::ios::binary | std::ios::trunc);

			if (!lPayloadOut.good())
			{
				return std::nullopt;
			}

			lPayloadOut << "{";

			if (!pPathModel.empty())
			{
				lPayloadOut << "\"model\":\"" << EscapeJson(pPathModel.string()) << "\",";
			}

			lPayloadOut << "\"input\":\"" << EscapeJson(pSText) << "\"}";
		}

		const std::array<std::string, 2> kArrSEndpoints = {"/v1/embeddings", "/embeddings"};
		std::optional<std::vector<float>> lOptVecfEmbedding;

		for (const std::string& lSEndpoint : kArrSEndpoints)
		{
			std::string lSCommandOutput;
			const std::string lSCommand = "curl -sS --fail --max-time 30 -H 'Content-Type: application/json' --data-binary @" + ShellQuote(lPathPayloadFile.string()) + " " + ShellQuote(lSServerUrl + lSEndpoint);

			if (RunShellCommand(lSCommand, &lSCommandOutput) != 0)
			{
				continue;
			}

			lOptVecfEmbedding = ParseEmbeddingFromJson(lSCommandOutput);

			if (lOptVecfEmbedding.has_value())
			{
				break;
			}
		}

		std::error_code lErrorCode;
		fs::remove(lPathPayloadFile, lErrorCode);

		if (!lOptVecfEmbedding.has_value())
		{
			return std::nullopt;
		}

		return NormalizeEmbedding(*lOptVecfEmbedding);
	}

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
	inline std::once_flag g_OnceLlamaBackendInit;

	inline void EnsureLlamaBackendInit()
	{
		const auto lFnInitializeLlamaBackend = []()
		{
			llama_backend_init();
			ggml_backend_load_all();
		};
		std::call_once(g_OnceLlamaBackendInit, lFnInitializeLlamaBackend);
	}

	inline void ReleaseEmbeddingRuntimeLocked(Context& pContext)
	{
		if (pContext.pPtrEmbeddingContext != nullptr)
		{
			llama_free(pContext.pPtrEmbeddingContext);
			pContext.pPtrEmbeddingContext = nullptr;
		}

		if (pContext.pPtrEmbeddingModel != nullptr)
		{
			llama_model_free(pContext.pPtrEmbeddingModel);
			pContext.pPtrEmbeddingModel = nullptr;
		}

		pContext.pPathLoadedEmbeddingModel.clear();
		pContext.piLoadedEmbeddingMaxTokens = 0;
	}

	inline std::vector<fs::path> DiscoverModelFiles(const fs::path& pPathModelFolder)
	{
		std::vector<fs::path> lVecPathModels;
		std::error_code lErrorCode;
		fs::recursive_directory_iterator lIterator(pPathModelFolder, fs::directory_options::skip_permission_denied, lErrorCode);
		const fs::recursive_directory_iterator lEndIterator;

		while (!lErrorCode && lIterator != lEndIterator)
		{
			const fs::directory_entry lEntry = *lIterator;
			lIterator.increment(lErrorCode);

			if (lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			if (lEntry.is_directory(lErrorCode))
			{
				if (!lErrorCode && ShouldSkipDirectory(lEntry.path()))
				{
					lIterator.disable_recursion_pending();
				}

				lErrorCode.clear();
				continue;
			}

			if (!lEntry.is_regular_file(lErrorCode) || lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			if (ToLowerAscii(lEntry.path().extension().string()) != ".gguf")
			{
				continue;
			}

			lVecPathModels.push_back(lEntry.path());
		}

		std::sort(lVecPathModels.begin(), lVecPathModels.end());
		return lVecPathModels;
	}

	inline fs::path ResolveEmbeddingModelPath(const RuntimeOptions& pRuntimeOptions)
	{
		std::error_code lErrorCode;

		if (!pRuntimeOptions.pPathEmbeddingModelFile.empty() && fs::exists(pRuntimeOptions.pPathEmbeddingModelFile, lErrorCode) && fs::is_regular_file(pRuntimeOptions.pPathEmbeddingModelFile, lErrorCode))
		{
			return pRuntimeOptions.pPathEmbeddingModelFile;
		}

		const char* lPtrEnvModel = std::getenv("UAM_EMBEDDING_MODEL_PATH");

		if (lPtrEnvModel != nullptr && *lPtrEnvModel != '\0')
		{
			fs::path lPathModel = lPtrEnvModel;

			if (fs::exists(lPathModel, lErrorCode) && fs::is_regular_file(lPathModel, lErrorCode))
			{
				return lPathModel;
			}
		}

		const std::vector<fs::path> lVecPathModels = DiscoverModelFiles(pRuntimeOptions.pPathModelFolder);

		for (const fs::path& lPathModel : lVecPathModels)
		{
			const std::string lSNameLower = ToLowerAscii(lPathModel.filename().string());

			if (lSNameLower.find("embed") != std::string::npos)
			{
				return lPathModel;
			}
		}

		return lVecPathModels.empty() ? fs::path{} : lVecPathModels.front();
	}

	inline bool EnsureDirectEmbeddingRuntime(Context& pContext, const fs::path& pPathModel, const RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut)
	{
		if (pPathModel.empty())
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "No embedding model could be resolved.";
			}

			return false;
		}

		const std::size_t liEmbeddingMaxTokens = pRuntimeOptions.piEmbeddingMaxTokens > 0 ? std::clamp<std::size_t>(pRuntimeOptions.piEmbeddingMaxTokens, 1, 32768) : 4096;

		EnsureLlamaBackendInit();

		if (pContext.pPtrEmbeddingModel != nullptr && pContext.pPtrEmbeddingContext != nullptr && pContext.pPathLoadedEmbeddingModel == pPathModel && pContext.piLoadedEmbeddingMaxTokens == liEmbeddingMaxTokens)
		{
			return true;
		}

		ReleaseEmbeddingRuntimeLocked(pContext);

		llama_model_params lModelParams = llama_model_default_params();
		lModelParams.n_gpu_layers = 99;
		llama_model* lPtrModel = llama_model_load_from_file(pPathModel.string().c_str(), lModelParams);

		if (lPtrModel == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to load embedding GGUF model: " + pPathModel.string();
			}

			return false;
		}

		llama_context_params lContextParams = llama_context_default_params();
		const auto liContextTokens = static_cast<decltype(lContextParams.n_ctx)>(liEmbeddingMaxTokens);
		lContextParams.n_ctx = liContextTokens;
		lContextParams.n_batch = liContextTokens;
		lContextParams.n_ubatch = liContextTokens;
		lContextParams.embeddings = true;
		lContextParams.pooling_type = LLAMA_POOLING_TYPE_MEAN;
		llama_context* lPtrContext = llama_init_from_model(lPtrModel, lContextParams);

		if (lPtrContext == nullptr)
		{
			llama_model_free(lPtrModel);

			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to initialize llama.cpp embedding context.";
			}

			return false;
		}

		pContext.pPtrEmbeddingModel = lPtrModel;
		pContext.pPtrEmbeddingContext = lPtrContext;
		pContext.pPathLoadedEmbeddingModel = pPathModel;
		pContext.piLoadedEmbeddingMaxTokens = liEmbeddingMaxTokens;
		return true;
	}

	inline std::optional<std::vector<float>> BuildEmbeddingDirect(Context& pContext, const RuntimeOptions& pRuntimeOptions, const std::string& pSText, std::string* pSErrorOut)
	{
		const fs::path lPathEmbeddingModel = ResolveEmbeddingModelPath(pRuntimeOptions);
		std::lock_guard<std::mutex> lGuard(pContext.pMutex);

		if (!EnsureDirectEmbeddingRuntime(pContext, lPathEmbeddingModel, pRuntimeOptions, pSErrorOut))
		{
			return std::nullopt;
		}

		const llama_vocab* lPtrVocab = llama_model_get_vocab(pContext.pPtrEmbeddingModel);

		if (lPtrVocab == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Embedding model has no vocabulary.";
			}

			return std::nullopt;
		}

		const int32_t liTokenCountWanted = -llama_tokenize(lPtrVocab, pSText.c_str(), static_cast<int32_t>(pSText.size()), nullptr, 0, true, true);

		if (liTokenCountWanted <= 0)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to tokenize text for embeddings.";
			}

			return std::nullopt;
		}

		std::vector<llama_token> lVeciTokens(static_cast<std::size_t>(liTokenCountWanted));
		const int32_t liTokenCountWritten = llama_tokenize(lPtrVocab, pSText.c_str(), static_cast<int32_t>(pSText.size()), lVeciTokens.data(), static_cast<int32_t>(lVeciTokens.size()), true, true);

		if (liTokenCountWritten <= 0)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to write tokenized text for embeddings.";
			}

			return std::nullopt;
		}

		lVeciTokens.resize(static_cast<std::size_t>(liTokenCountWritten));

		if (pContext.piLoadedEmbeddingMaxTokens > 0 && lVeciTokens.size() > pContext.piLoadedEmbeddingMaxTokens)
		{
			lVeciTokens.resize(pContext.piLoadedEmbeddingMaxTokens);
		}

		if (lVeciTokens.empty())
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "No tokens available after applying embedding token limit.";
			}

			return std::nullopt;
		}

		llama_memory_clear(llama_get_memory(pContext.pPtrEmbeddingContext), true);

		llama_batch lBatch = llama_batch_init(static_cast<int32_t>(lVeciTokens.size()), 0, 1);

		for (std::size_t liIndex = 0; liIndex < lVeciTokens.size(); ++liIndex)
		{
			lBatch.token[liIndex] = lVeciTokens[liIndex];
			lBatch.pos[liIndex] = static_cast<llama_pos>(liIndex);
			lBatch.n_seq_id[liIndex] = 1;
			lBatch.seq_id[liIndex][0] = 0;
			lBatch.logits[liIndex] = 1;
		}

		lBatch.n_tokens = static_cast<int32_t>(lVeciTokens.size());

		int32_t liStatus = 0;

		if (llama_model_has_encoder(pContext.pPtrEmbeddingModel) && !llama_model_has_decoder(pContext.pPtrEmbeddingModel))
		{
			liStatus = llama_encode(pContext.pPtrEmbeddingContext, lBatch);
		}
		else
		{
			liStatus = llama_decode(pContext.pPtrEmbeddingContext, lBatch);
		}

		if (liStatus != 0)
		{
			llama_batch_free(lBatch);

			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "llama.cpp failed while creating embeddings.";
			}

			return std::nullopt;
		}

		const int32_t liEmbeddingDimensionsRaw = std::max<int32_t>(llama_model_n_embd_out(pContext.pPtrEmbeddingModel), llama_model_n_embd(pContext.pPtrEmbeddingModel));

		if (liEmbeddingDimensionsRaw <= 0)
		{
			llama_batch_free(lBatch);

			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Embedding model reported invalid dimensions.";
			}

			return std::nullopt;
		}

		const float* lPtrfEmbedding = nullptr;

		if (llama_pooling_type(pContext.pPtrEmbeddingContext) != LLAMA_POOLING_TYPE_NONE)
		{
			lPtrfEmbedding = llama_get_embeddings_seq(pContext.pPtrEmbeddingContext, 0);
		}

		if (lPtrfEmbedding == nullptr)
		{
			lPtrfEmbedding = llama_get_embeddings_ith(pContext.pPtrEmbeddingContext, -1);
		}

		if (lPtrfEmbedding == nullptr)
		{
			llama_batch_free(lBatch);

			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "llama.cpp returned null embedding tensor.";
			}

			return std::nullopt;
		}

		std::vector<float> lVecfEmbedding(static_cast<std::size_t>(liEmbeddingDimensionsRaw));
		std::copy(lPtrfEmbedding, lPtrfEmbedding + liEmbeddingDimensionsRaw, lVecfEmbedding.begin());
		llama_batch_free(lBatch);
		return NormalizeEmbedding(std::move(lVecfEmbedding));
	}
#endif

	inline std::optional<std::vector<float>> BuildEmbedding(Context& pContext, const RuntimeOptions& pRuntimeOptions, const std::string& pSText, std::string* pSErrorOut)
	{
		if (pRuntimeOptions.pbUseDeterministicEmbeddings)
		{
			const std::size_t liDimensions = std::clamp<std::size_t>(pRuntimeOptions.piDeterministicEmbeddingDimensions, 32, 4096);
			return NormalizeEmbedding(ollama_engine::internal::BuildEmbedding(pSText, liDimensions));
		}

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
		const fs::path lPathModelForServer = ResolveEmbeddingModelPath(pRuntimeOptions);
		const std::optional<std::vector<float>> lOptServerEmbedding = TryEmbedWithServer(pContext, pRuntimeOptions, lPathModelForServer, pSText);

		if (lOptServerEmbedding.has_value())
		{
			return lOptServerEmbedding;
		}

		return BuildEmbeddingDirect(pContext, pRuntimeOptions, pSText, pSErrorOut);
#else
		(void)pContext;
		(void)pRuntimeOptions;
		(void)pSText;

		if (pSErrorOut != nullptr)
		{
			*pSErrorOut = "llama.cpp is not linked, embeddings are unavailable.";
		}

		return std::nullopt;
#endif
	}

} // namespace ollama_engine::internal::vectorised_rag
