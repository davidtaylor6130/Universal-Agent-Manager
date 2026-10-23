#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
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
		struct ReceiptOwner final {};
	  public:
		enum class WriteCompletion { Pending, Complete, Incomplete };

		/// Sink acceptance of this exact queued entry; never provider processing.
		class EnqueueReceipt final
		{
		  public:
			EnqueueReceipt(const EnqueueReceipt&) = delete;
			EnqueueReceipt& operator=(const EnqueueReceipt&) = delete;
			WriteCompletion Completion() const
			{
				return m_state.load(std::memory_order_acquire);
			}
		  private:
			friend class AsyncByteWriter;
			explicit EnqueueReceipt(std::weak_ptr<const ReceiptOwner> owner) : m_owner(std::move(owner)) {}
			const std::weak_ptr<const ReceiptOwner> m_owner;
			std::atomic<WriteCompletion> m_state{WriteCompletion::Pending};
		};
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
			return EnqueueImpl(bytes, len, nullptr, error_out);
		}

		/// Empty/null input cannot mint an observed-write receipt; legacy Enqueue is unchanged.
		bool EnqueueObserved(const char* bytes, std::size_t len,
		    std::shared_ptr<const EnqueueReceipt>& receipt_out, std::string* error_out = nullptr)
		{
			receipt_out.reset();
			if (bytes == nullptr || len == 0)
			{
				if (error_out) *error_out = "An observed input write requires nonempty bytes.";
				return false;
			}
			return EnqueueImpl(bytes, len, &receipt_out, error_out);
		}

		bool OwnsReceipt(const EnqueueReceipt& receipt) const
		{
			return receipt.m_owner.lock() == m_receipt_owner;
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
				FinalizePendingReceiptsLocked();
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
		struct QueuedBytes
		{
			std::string bytes;
			std::shared_ptr<EnqueueReceipt> receipt;
		};

		bool EnqueueImpl(const char* bytes, std::size_t len,
		    std::shared_ptr<const EnqueueReceipt>* receipt_out, std::string* error_out)
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
			std::shared_ptr<EnqueueReceipt> receipt;
			if (receipt_out) receipt = std::shared_ptr<EnqueueReceipt>(new EnqueueReceipt(m_receipt_owner));
			m_queue.push_back(QueuedBytes{std::string(bytes, len), receipt});
			if (receipt_out) *receipt_out = std::move(receipt);
			m_pending_bytes += len;
			m_condition.notify_one();
			return true;
		}

		/// Called only while the existing writer mutex excludes completion publication.
		void FinalizePendingReceiptsLocked()
		{
			for (const QueuedBytes& entry : m_queue)
			{
				if (!entry.receipt) continue;
				WriteCompletion pending = WriteCompletion::Pending;
				(void)entry.receipt->m_state.compare_exchange_strong(pending, WriteCompletion::Incomplete,
				    std::memory_order_release, std::memory_order_relaxed);
			}
		}

		void FailLocked(std::string message)
		{
			m_failed = true;
			FinalizePendingReceiptsLocked();
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

				const QueuedBytes& entry = m_queue.front();
				const std::string& front = entry.bytes;
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
					if (entry.receipt && !m_failed && !m_stopping)
					{
						WriteCompletion pending = WriteCompletion::Pending;
						(void)entry.receipt->m_state.compare_exchange_strong(pending, WriteCompletion::Complete,
						    std::memory_order_release, std::memory_order_relaxed);
					}
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
		const std::shared_ptr<ReceiptOwner> m_receipt_owner = std::make_shared<ReceiptOwner>();
		std::deque<QueuedBytes> m_queue;
		std::size_t m_front_offset = 0;
		std::size_t m_pending_bytes = 0;
		std::chrono::steady_clock::time_point m_last_progress_at = std::chrono::steady_clock::now();
		bool m_failed = false;
		bool m_stopping = false;
		std::string m_error;
		std::jthread m_worker;
	};
} // namespace uam::platform
