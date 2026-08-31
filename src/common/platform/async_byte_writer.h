#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace uam::platform
{
	inline constexpr std::size_t kAsyncInputMaxQueuedBytes = 4 * 1024 * 1024;
	inline constexpr std::size_t kAsyncInputWriteChunkBytes = 16 * 1024;
	inline constexpr auto kAsyncInputNoProgressTimeout = std::chrono::seconds(5);
	inline constexpr auto kAsyncInputCloseDrainTimeout = std::chrono::seconds(1);

	class AsyncByteWriter
	{
	  public:
		using WriteChunk = std::function<std::ptrdiff_t(const char*, std::size_t, std::string&)>;
		using Close = std::function<void()>;

		AsyncByteWriter(WriteChunk write_chunk, Close close, std::chrono::milliseconds no_progress_timeout = kAsyncInputNoProgressTimeout)
		    : m_write_chunk(std::move(write_chunk)), m_close(std::move(close)), m_no_progress_timeout(no_progress_timeout), m_worker([this](std::stop_token stop_token) { Run(stop_token); })
		{
		}

		~AsyncByteWriter()
		{
			Stop();
		}

		AsyncByteWriter(const AsyncByteWriter&) = delete;
		AsyncByteWriter& operator=(const AsyncByteWriter&) = delete;

		bool Enqueue(const char* bytes, std::size_t len, std::string* error_out = nullptr)
		{
			if (bytes == nullptr || len == 0)
			{
				return true;
			}

			std::lock_guard lock(m_mutex);
			if (m_failed || m_stopping)
			{
				if (error_out != nullptr)
				{
					*error_out = m_error.empty() ? "Provider input writer is stopped." : m_error;
				}
				return false;
			}
			if (len > kAsyncInputMaxQueuedBytes - std::min(m_pending_bytes, kAsyncInputMaxQueuedBytes))
			{
				FailLocked("Provider input exceeded the 4 MiB queue safety limit.");
				if (error_out != nullptr)
				{
					*error_out = m_error;
				}
				m_condition.notify_all();
				return false;
			}

			if (m_pending_bytes == 0)
			{
				m_last_progress_at = std::chrono::steady_clock::now();
			}
			m_queue.emplace_back(bytes, len);
			m_pending_bytes += len;
			m_condition.notify_one();
			return true;
		}

		bool FailureOrStall(std::string* error_out = nullptr)
		{
			std::lock_guard lock(m_mutex);
			if (!m_failed && m_pending_bytes > 0 && std::chrono::steady_clock::now() - m_last_progress_at >= m_no_progress_timeout)
			{
				FailLocked("Provider input made no progress before the safety deadline.");
				m_condition.notify_all();
			}
			if (!m_failed)
			{
				return false;
			}
			if (error_out != nullptr)
			{
				*error_out = m_error;
			}
			return true;
		}

		std::size_t PendingBytes() const
		{
			std::lock_guard lock(m_mutex);
			return m_pending_bytes;
		}

		bool Flush(std::chrono::milliseconds timeout, std::string* error_out = nullptr)
		{
			std::unique_lock lock(m_mutex);
			if (!m_condition.wait_for(lock, timeout, [this] { return m_pending_bytes == 0 || m_failed; }))
			{
				if (error_out != nullptr)
				{
					*error_out = "Provider input did not drain before the close deadline.";
				}
				return false;
			}
			if (m_failed)
			{
				if (error_out != nullptr)
				{
					*error_out = m_error;
				}
				return false;
			}
			return true;
		}

		void Stop()
		{
			{
				std::lock_guard lock(m_mutex);
				if (m_stopping)
				{
					return;
				}
				m_stopping = true;
			}
			m_worker.request_stop();
			m_condition.notify_all();
#if defined(_WIN32)
		#if defined(__MINGW32__)
			(void)CancelSynchronousIo(reinterpret_cast<HANDLE>(m_worker.native_handle()));
		#else
			(void)CancelSynchronousIo(m_worker.native_handle());
		#endif
#endif
			if (m_worker.joinable())
			{
				m_worker.join();
			}
			if (m_close)
			{
				m_close();
				m_close = {};
			}
		}

	  private:
		void FailLocked(std::string message)
		{
			m_failed = true;
			m_error = std::move(message);
		}

		void Run(std::stop_token stop_token)
		{
			std::unique_lock lock(m_mutex);
			while (!stop_token.stop_requested())
			{
				m_condition.wait(lock, stop_token, [this] { return m_failed || !m_queue.empty(); });
				if (stop_token.stop_requested() || m_failed)
				{
					return;
				}

				const std::string& front = m_queue.front();
				const std::size_t chunk_size = std::min(kAsyncInputWriteChunkBytes, front.size() - m_front_offset);
				std::string chunk(front.data() + m_front_offset, chunk_size);
				lock.unlock();
				std::string write_error;
				const std::ptrdiff_t written = m_write_chunk(chunk.data(), chunk.size(), write_error);
				lock.lock();

				if (written < 0 || static_cast<std::size_t>(written) > chunk.size())
				{
					FailLocked(write_error.empty() ? "Provider input write failed." : std::move(write_error));
					m_condition.notify_all();
					return;
				}
				if (written == 0)
				{
					m_condition.wait_for(lock, stop_token, std::chrono::milliseconds(10), [] { return false; });
					continue;
				}

				const std::size_t written_size = static_cast<std::size_t>(written);
				m_front_offset += written_size;
				m_pending_bytes -= written_size;
				m_last_progress_at = std::chrono::steady_clock::now();
				m_condition.notify_all();
				if (m_front_offset == front.size())
				{
					m_queue.pop_front();
					m_front_offset = 0;
				}
			}
		}

		WriteChunk m_write_chunk;
		Close m_close;
		std::chrono::milliseconds m_no_progress_timeout;
		mutable std::mutex m_mutex;
		std::condition_variable_any m_condition;
		std::deque<std::string> m_queue;
		std::size_t m_front_offset = 0;
		std::size_t m_pending_bytes = 0;
		std::chrono::steady_clock::time_point m_last_progress_at = std::chrono::steady_clock::now();
		bool m_failed = false;
		bool m_stopping = false;
		std::string m_error;
		std::jthread m_worker;
	};
} // namespace uam::platform
