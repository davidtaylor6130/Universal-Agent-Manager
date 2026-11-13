#pragma once

// Platform-neutral pieces shared by secret_drop.cpp and secret_drop_windows.cpp.
#include "common/paths/path_utils.h"
#include "common/secrets/secret_drop.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>

namespace uam::secrets::drop_internal
{
	// Volatile writes so the compiler cannot drop the wipe as a dead store.
	inline void Wipe(std::string& value)
	{
		volatile char* bytes = value.data();
		for (std::size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
		value.clear();
	}

	inline std::string RandomDropName()
	{
		constexpr char kHex[] = "0123456789abcdef";
		std::random_device random;
		std::string name(kDropPrefix);
		for (int word = 0; word < 4; ++word)
		{
			const std::uint32_t bits = random();
			for (int shift = 28; shift >= 0; shift -= 4) name.push_back(kHex[(bits >> shift) & 0x0FU]);
		}
		return name;
	}

	struct Writer
	{
		std::jthread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};

	inline std::mutex& WritersMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	// Process exit destroys this list, which stops and joins every pending writer so it removes its drop.
	inline std::list<Writer>& Writers()
	{
		static std::list<Writer> writers;
		return writers;
	}

	/// Runs body(stop_token) on a tracked thread; throws std::system_error when no thread can start.
	template <class Body>
	void StartWriter(Body body)
	{
		auto done = std::make_shared<std::atomic<bool>>(false);
		std::lock_guard lock(WritersMutex());
		std::erase_if(Writers(), [](const Writer& writer) { return writer.done->load(std::memory_order_acquire); });
		Writers().push_back({std::jthread([body = std::move(body), done](std::stop_token stop) mutable
		{
			body(stop);
			done->store(true, std::memory_order_release);
		}), done});
	}

	inline void StopAllWriters()
	{
		std::list<Writer> writers;
		{
			std::lock_guard lock(WritersMutex());
			writers.swap(Writers());
		}
		for (Writer& writer : writers) writer.thread.request_stop();
		// Destroying the list joins each writer after it removed its drop.
	}

	inline std::size_t SweepDropDirectory(const std::filesystem::path& directory)
	{
		std::error_code error;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error)) || error) return 0;
		std::size_t removed = 0;
		for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error))
		{
			if (!uam::paths::Utf8PathString(entry->path().filename()).starts_with(kDropPrefix)) continue;
			std::error_code entry_error;
			const std::filesystem::file_status status = entry->symlink_status(entry_error);
			if (entry_error || !(std::filesystem::is_fifo(status) || std::filesystem::is_regular_file(status))) continue;
			if (std::filesystem::remove(entry->path(), entry_error)) ++removed;
		}
		return removed;
	}
}
