#pragma once
#include <array>

// Public fixed file-format key, NOT an account secret or credential.
// Pinned primary extraction/provenance: mjacobs/agy-reader
// 4c5f5e7a75819c6308ced8e346740ab87ec09019, internal/completeness/legacy_key.bin.
// agy 1.4.4 binary SHA256 337dcde668441a2828968644d7982829c2d2a5e0c4144f346e1f3fc6abb3cdc7.
namespace uam::antigravity::history::detail
{
	inline constexpr std::array<unsigned char, 32> kLegacyFormatKey{0x73, 0x61, 0x66, 0x65, 0x43, 0x6f, 0x64, 0x65, 0x69, 0x75, 0x6d, 0x77, 0x6f, 0x72, 0x6c, 0x64, 0x4b, 0x65, 0x59, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x42, 0x61, 0x6c, 0x6c, 0x6f, 0x6f, 0x6e};
}
