#include "determanistic_hash.h"

#include <array>

namespace ollama_engine::internal::determanistic_hash {

std::uint64_t Fnv1a64(const std::string_view pSContent) {
  constexpr std::uint64_t kiFnv64OffsetBasis = 1469598103934665603ULL;
  constexpr std::uint64_t kiFnv64Prime = 1099511628211ULL;
  std::uint64_t liHash = kiFnv64OffsetBasis;
  for (const unsigned char lCChar : pSContent) {
    liHash ^= static_cast<std::uint64_t>(lCChar);
    liHash *= kiFnv64Prime;
  }
  return liHash;
}

std::string ToHex(const std::uint64_t piHash) {
  constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string lSOut(16, '0');
  for (int liIndex = 15; liIndex >= 0; --liIndex) {
    const std::size_t liShift = static_cast<std::size_t>(15 - liIndex) * 4;
    lSOut[static_cast<std::size_t>(liIndex)] = kHex[static_cast<std::size_t>((piHash >> liShift) & 0xFULL)];
  }
  return lSOut;
}

std::string HashTextHex(const std::string_view pSContent) {
  return ToHex(Fnv1a64(pSContent));
}

}  // namespace ollama_engine::internal::determanistic_hash
