#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

// Read-once secret handoff: an agent receives only a path. POSIX serves the value through a FIFO that is
// written once to the first reader and never stored on disk; Windows writes an owner-only file removed after its TTL.
namespace uam::secrets
{
	inline constexpr std::chrono::seconds kDropTtl{600};
	inline constexpr std::chrono::seconds kWindowsDropTtl{60};
	inline constexpr std::string_view kDropPrefix = "uam-secret-";

	struct DropResult
	{
		bool ok = false;
		std::string path;
		std::string error; // Never contains the value.
	};

	/// This computer's drop kind, chosen by the platform file that is built: POSIX pipes are read once and live
	/// kDropTtl; Windows files cannot see a read and live kWindowsDropTtl. platform is what the card names.
	struct LocalDropKind
	{
		std::chrono::seconds ttl;
		bool read_once;
		std::string_view platform;
	};
	LocalDropKind LocalDrop();

	/// Creates an owner-only drop in directory (created owner-only when missing) and serves value until it is read or ttl passes.
	DropResult Drop(const std::filesystem::path& directory, std::string_view value, std::chrono::seconds ttl);
	/// Removes drop-prefixed FIFOs and files left by earlier processes; nothing else. Returns how many were removed.
	std::size_t SweepStale(const std::filesystem::path& directory);
	/// Stops and joins every pending drop writer and removes its drop (shutdown and tests).
	void StopAllDrops();
}
