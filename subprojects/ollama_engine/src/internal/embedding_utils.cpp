#include "embedding_utils.h"

#include <cctype>
#include <cmath>
#include <cstdint>

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

/// <summary>Computes a deterministic 64-bit FNV-1a hash.</summary>
/// <param name="pSText">Input text.</param>
/// <returns>FNV-1a hash value.</returns>
/// <remarks>
/// We use FNV-1a because it is fast, deterministic, and good enough for
/// mapping tokens into embedding dimensions.
/// Magic numbers used here are standard FNV-1a constants:
/// - 1469598103934665603ULL: 64-bit FNV offset basis (initial hash)
/// - 1099511628211ULL: 64-bit FNV prime (mixing multiplier)
/// These are defined by the FNV hash specification and are not arbitrary.
/// </remarks>
std::uint64_t Fnv1a64(const std::string& pSText) {
  constexpr std::uint64_t kiFnv64OffsetBasis = 1469598103934665603ULL;
  constexpr std::uint64_t kiFnv64Prime = 1099511628211ULL;
  std::uint64_t liHash = kiFnv64OffsetBasis;
  for (const unsigned char lCChar : pSText) {
    liHash ^= static_cast<std::uint64_t>(lCChar);
    liHash *= kiFnv64Prime;
  }
  return liHash;
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
    const std::uint64_t liHash = Fnv1a64(lSToken);
    const std::size_t liFirst = static_cast<std::size_t>(liHash % piDimensions);
    const std::size_t liSecond = static_cast<std::size_t>((liHash >> 32) % piDimensions);
    const float lfWeight = 1.0f + static_cast<float>(liHash & 0xFFULL) / 255.0f;
    lVecfEmbedding[liFirst] += lfWeight;
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
