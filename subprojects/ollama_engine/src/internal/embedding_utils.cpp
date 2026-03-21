#include "embedding_utils.h"
#include "determanistic_hash/determanistic_hash.h"

#include <cctype>
#include <cmath>

namespace ollama_engine {
namespace {

/// <summary>Tokenizes text into lowercase alphanumeric tokens.</summary>
/// <param name="pSText">Input text.</param>
/// <returns>Token list extracted from the text.</returns>
/// <remarks>
/// This function normalizes text before hashing so similar prompts produce
/// stable embeddings. It lowercases letters and keeps only alphanumeric
/// characters plus underscore (`_`), which means punctuation and whitespace
/// become token boundaries. Example:
/// "Error-Code_42!" -> ["error", "code_42"].
/// </remarks>
std::vector<std::string> Tokenize(const std::string& pSText) {
  std::vector<std::string> lVecSTokens;
  std::string lSToken;
  lSToken.reserve(32);
  for (const unsigned char lCChar : pSText) {
    if (std::isalnum(lCChar) != 0 || lCChar == '_') {
      lSToken.push_back(static_cast<char>(std::tolower(lCChar)));
      continue;
    }
    if (!lSToken.empty()) {
      lVecSTokens.push_back(lSToken);
      lSToken.clear();
    }
  }
  if (!lSToken.empty()) {
    lVecSTokens.push_back(lSToken);
  }
  return lVecSTokens;
}

}  // namespace

namespace internal {

/// <summary>Builds a normalized embedding vector from prompt text.</summary>
/// <param name="pSText">Prompt or seed text.</param>
/// <param name="piDimensions">Embedding dimensions.</param>
/// <returns>Dense normalized float embedding.</returns>
std::vector<float> BuildEmbedding(const std::string& pSText, const std::size_t piDimensions) {
  std::vector<float> lVecfEmbedding(piDimensions, 0.0f);
  if (piDimensions == 0) {
    return lVecfEmbedding;
  }

  std::vector<std::string> lVecSTokens = Tokenize(pSText);
  if (lVecSTokens.empty()) {
    lVecSTokens.push_back(pSText);
  }
  for (const std::string& lSToken : lVecSTokens) {
    const std::uint64_t liHash = determanistic_hash::Fnv1a64(lSToken);
    // Map the full 64-bit hash into a valid embedding index [0, piDimensions-1].
    // '%' is used as a wrap-around bucket operation so any hash value fits the vector size.
    const std::size_t liFirst = static_cast<std::size_t>(liHash % piDimensions);
    // Use the *upper* 32 bits as a second, mostly independent bucket source.
    // '>> 32' shifts away the lower half; '%' then maps that upper-half value into index range.
    // This spreads each token across two dimensions instead of one, reducing single-bucket collisions.
    const std::size_t liSecond = static_cast<std::size_t>((liHash >> 32) % piDimensions);
    // Extract lowest 8 bits with 0xFFULL (range 0..255), normalize by /255.0f to [0,1],
    // then shift to [1,2] via +1.0f. This gives deterministic per-token weight variation
    // without large magnitude swings.
    const float lfWeight = 1.0f + static_cast<float>(liHash & 0xFFULL) / 255.0f;
    lVecfEmbedding[liFirst] += lfWeight;
    // Add a smaller negative contribution on the second index to keep signal balanced.
    lVecfEmbedding[liSecond] -= (lfWeight * 0.5f);
  }

  double ldNorm = 0.0;
  for (const float lfValue : lVecfEmbedding) {
    ldNorm += static_cast<double>(lfValue) * static_cast<double>(lfValue);
  }
  ldNorm = std::sqrt(ldNorm);
  if (ldNorm > 0.0) {
    for (float& lfValue : lVecfEmbedding) {
      lfValue = static_cast<float>(static_cast<double>(lfValue) / ldNorm);
    }
  }
  return lVecfEmbedding;
}

}  // namespace internal
}  // namespace ollama_engine
