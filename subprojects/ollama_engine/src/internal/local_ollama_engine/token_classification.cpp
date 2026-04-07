#include "local_ollama_engine_private.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
#include "llama.h"
#endif

namespace ollama_engine::internal
{

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
	namespace
	{
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

		std::string NormalizeTokenMarkerText(const std::string& pSInput)
		{
			return TrimAsciiWhitespace(ToLowerAscii(pSInput));
		}

		bool IsKnownStopTokenText(const std::string& pSNormalizedTokenText)
		{
			return pSNormalizedTokenText == "</s>" || pSNormalizedTokenText == "<|eot_id|>" || pSNormalizedTokenText == "<|eom_id|>" || pSNormalizedTokenText == "<|end|>" || pSNormalizedTokenText == "<|endoftext|>" || pSNormalizedTokenText == "<|end_of_text|>" || pSNormalizedTokenText == "<end_of_turn>" || pSNormalizedTokenText == "<|im_end|>";
		}

		bool IsKnownThinkingMarkerTokenText(const std::string& pSNormalizedTokenText)
		{
			return pSNormalizedTokenText == "<think>" || pSNormalizedTokenText == "</think>" || pSNormalizedTokenText == "<thinking>" || pSNormalizedTokenText == "</thinking>" || pSNormalizedTokenText == "<|start_think|>" || pSNormalizedTokenText == "<|end_think|>" || pSNormalizedTokenText == "<|thinking|>" || pSNormalizedTokenText == "<reasoning>" || pSNormalizedTokenText == "</reasoning>";
		}

		bool IsThinkingStartTokenTextInternal(const std::string& pSNormalizedTokenText)
		{
			return pSNormalizedTokenText == "<think>" || pSNormalizedTokenText == "<thinking>" || pSNormalizedTokenText == "<|start_think|>" || pSNormalizedTokenText == "<|thinking|>" || pSNormalizedTokenText == "<reasoning>";
		}

		bool IsThinkingEndTokenTextInternal(const std::string& pSNormalizedTokenText)
		{
			return pSNormalizedTokenText == "</think>" || pSNormalizedTokenText == "</thinking>" || pSNormalizedTokenText == "<|end_think|>" || pSNormalizedTokenText == "</reasoning>";
		}

		std::string RemoveAllCaseInsensitive(std::string pSText, const std::string& pSMarkerLower)
		{
			if (pSMarkerLower.empty())
			{
				return pSText;
			}

			std::string lSLower = ToLowerAscii(pSText);
			std::size_t liPosition = 0;

			while (true)
			{
				liPosition = lSLower.find(pSMarkerLower, liPosition);

				if (liPosition == std::string::npos)
				{
					break;
				}

				pSText.erase(liPosition, pSMarkerLower.size());
				lSLower.erase(liPosition, pSMarkerLower.size());
			}

			return pSText;
		}

		std::string RemoveDelimitedSegmentsCaseInsensitive(std::string pSText, const std::string& pSStartLower, const std::string& pSEndLower)
		{
			if (pSStartLower.empty() || pSEndLower.empty())
			{
				return pSText;
			}

			std::string lSLower = ToLowerAscii(pSText);
			std::size_t liSearchPosition = 0;

			while (true)
			{
				const std::size_t liStart = lSLower.find(pSStartLower, liSearchPosition);

				if (liStart == std::string::npos)
				{
					break;
				}

				const std::size_t liEnd = lSLower.find(pSEndLower, liStart + pSStartLower.size());
				const std::size_t liEraseEnd = (liEnd == std::string::npos) ? pSText.size() : (liEnd + pSEndLower.size());
				pSText.erase(liStart, liEraseEnd - liStart);
				lSLower.erase(liStart, liEraseEnd - liStart);
				liSearchPosition = liStart;
			}

			return pSText;
		}
	} // namespace

	std::string GetNormalizedTokenMarkerText(const llama_vocab* pPtrVocab, const llama_token piToken)
	{
		if (pPtrVocab == nullptr)
		{
			return {};
		}

		const char* pSRawTokenText = llama_vocab_get_text(pPtrVocab, piToken);

		if (pSRawTokenText == nullptr)
		{
			return {};
		}

		return NormalizeTokenMarkerText(pSRawTokenText);
	}

	bool IsThinkingStartTokenText(const std::string& pSNormalizedTokenText)
	{
		return IsThinkingStartTokenTextInternal(pSNormalizedTokenText);
	}

	bool IsThinkingEndTokenText(const std::string& pSNormalizedTokenText)
	{
		return IsThinkingEndTokenTextInternal(pSNormalizedTokenText);
	}

	std::string StripReasoningSections(const std::string& pSText)
	{
		std::string lSOutput = pSText;
		lSOutput = RemoveDelimitedSegmentsCaseInsensitive(lSOutput, "<think>", "</think>");
		lSOutput = RemoveDelimitedSegmentsCaseInsensitive(lSOutput, "<thinking>", "</thinking>");
		lSOutput = RemoveDelimitedSegmentsCaseInsensitive(lSOutput, "<|start_think|>", "<|end_think|>");
		lSOutput = RemoveDelimitedSegmentsCaseInsensitive(lSOutput, "<reasoning>", "</reasoning>");

		lSOutput = RemoveAllCaseInsensitive(lSOutput, "<think>");
		lSOutput = RemoveAllCaseInsensitive(lSOutput, "</think>");
		lSOutput = RemoveAllCaseInsensitive(lSOutput, "<thinking>");
		lSOutput = RemoveAllCaseInsensitive(lSOutput, "</thinking>");
		lSOutput = RemoveAllCaseInsensitive(lSOutput, "<|start_think|>");
		lSOutput = RemoveAllCaseInsensitive(lSOutput, "<|end_think|>");
		lSOutput = RemoveAllCaseInsensitive(lSOutput, "<|thinking|>");
		lSOutput = RemoveAllCaseInsensitive(lSOutput, "<reasoning>");
		lSOutput = RemoveAllCaseInsensitive(lSOutput, "</reasoning>");
		return lSOutput;
	}

	std::optional<std::string> TokenToPieceText(const llama_vocab* pPtrVocab, const llama_token piToken)
	{
		if (pPtrVocab == nullptr)
		{
			return std::nullopt;
		}

		std::array<char, 256> lArrCBuffer{};
		int32_t liPieceLength = llama_token_to_piece(pPtrVocab, piToken, lArrCBuffer.data(), lArrCBuffer.size(), 0, false);

		if (liPieceLength < 0)
		{
			const int32_t liRequiredBufferLength = -liPieceLength;

			if (liRequiredBufferLength <= 0)
			{
				return std::nullopt;
			}

			std::vector<char> lVecCBuffer(static_cast<std::size_t>(liRequiredBufferLength));
			liPieceLength = llama_token_to_piece(pPtrVocab, piToken, lVecCBuffer.data(), lVecCBuffer.size(), 0, false);

			if (liPieceLength < 0)
			{
				return std::nullopt;
			}

			return std::string(lVecCBuffer.data(), static_cast<std::size_t>(liPieceLength));
		}

		return std::string(lArrCBuffer.data(), static_cast<std::size_t>(liPieceLength));
	}

	SampledTokenBehavior ClassifySampledToken(const llama_vocab* pPtrVocab, const llama_token piToken)
	{
		if (pPtrVocab == nullptr)
		{
			return SampledTokenBehavior::Stop;
		}

		if (llama_vocab_is_eog(pPtrVocab, piToken))
		{
			return SampledTokenBehavior::Stop;
		}

		const std::string lSNormalizedTokenText = GetNormalizedTokenMarkerText(pPtrVocab, piToken);

		if (IsKnownStopTokenText(lSNormalizedTokenText))
		{
			return SampledTokenBehavior::Stop;
		}

		if (IsKnownThinkingMarkerTokenText(lSNormalizedTokenText))
		{
			return SampledTokenBehavior::Thinking;
		}

		if (llama_vocab_is_control(pPtrVocab, piToken))
		{
			return SampledTokenBehavior::Hidden;
		}

		return SampledTokenBehavior::Emit;
	}

	std::optional<llama_token> SelectBestFallbackToken(const llama_vocab* pPtrVocab, llama_context* pPtrContext)
	{
		if (pPtrVocab == nullptr || pPtrContext == nullptr)
		{
			return std::nullopt;
		}

		float* lPtrfLogits = llama_get_logits_ith(pPtrContext, -1);

		if (lPtrfLogits == nullptr)
		{
			return std::nullopt;
		}

		const int32_t liVocabSize = llama_vocab_n_tokens(pPtrVocab);

		if (liVocabSize <= 0)
		{
			return std::nullopt;
		}

		auto lFindBestByBehavior = [&](const SampledTokenBehavior pDesiredBehavior) -> std::optional<llama_token>
		{
			bool lbFound = false;
			llama_token liBestToken = LLAMA_TOKEN_NULL;
			float lfBestLogit = -std::numeric_limits<float>::infinity();

			for (int32_t liTokenIndex = 0; liTokenIndex < liVocabSize; ++liTokenIndex)
			{
				const llama_token liToken = static_cast<llama_token>(liTokenIndex);
				const SampledTokenBehavior lTokenBehavior = ClassifySampledToken(pPtrVocab, liToken);

				if (lTokenBehavior != pDesiredBehavior)
				{
					continue;
				}

				const float lfLogit = lPtrfLogits[liTokenIndex];

				if (!lbFound || lfLogit > lfBestLogit)
				{
					lbFound = true;
					liBestToken = liToken;
					lfBestLogit = lfLogit;
				}
			}

			if (!lbFound)
			{
				return std::nullopt;
			}

			return liBestToken;
		};

		const std::optional<llama_token> lOptVisibleToken = lFindBestByBehavior(SampledTokenBehavior::Emit);

		if (lOptVisibleToken.has_value())
		{
			return lOptVisibleToken;
		}

		const std::optional<llama_token> lOptThinkingToken = lFindBestByBehavior(SampledTokenBehavior::Thinking);

		if (lOptThinkingToken.has_value())
		{
			return lOptThinkingToken;
		}

		return lFindBestByBehavior(SampledTokenBehavior::Hidden);
	}
#endif

} // namespace ollama_engine::internal
