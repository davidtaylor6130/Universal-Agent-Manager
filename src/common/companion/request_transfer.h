#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace uam::companion
{
	inline constexpr std::size_t kMaxRequestBytes = 32 * 1024 * 1024;
	inline constexpr std::size_t kRequestChunkBytes = 32 * 1024;
	inline constexpr std::size_t kMaxRequestTransfers = 4;
	inline constexpr std::chrono::seconds kRequestTransferTtl{120};

	// Canonical RFC 4648 base64: padding required, no whitespace or unused bits.
	inline std::optional<std::size_t> Base64DecodedSize(std::string_view encoded)
	{
		if (encoded.size() % 4 != 0)
			return std::nullopt;
		if (encoded.empty())
			return 0;
		const auto digit = [](char value) -> int
		{
			if (value >= 'A' && value <= 'Z')
				return value - 'A';
			if (value >= 'a' && value <= 'z')
				return value - 'a' + 26;
			if (value >= '0' && value <= '9')
				return value - '0' + 52;
			return value == '+' ? 62 : value == '/' ? 63 : -1;
		};
		const std::size_t padding = encoded.back() == '=' ? (encoded[encoded.size() - 2] == '=' ? 2 : 1) : 0;
		for (std::size_t i = 0; i < encoded.size() - padding; ++i)
			if (digit(encoded[i]) < 0)
				return std::nullopt;
		const int last = digit(encoded[encoded.size() - padding - 1]);
		if ((padding == 2 && (last & 15) != 0) || (padding == 1 && (last & 3) != 0))
			return std::nullopt;
		return encoded.size() / 4 * 3 - padding;
	}

	inline std::string DecodeBase64(std::string_view encoded, std::size_t decoded_size)
	{
		std::string decoded;
		decoded.reserve(decoded_size);
		std::uint32_t value = 0;
		int bits = 0;
		for (char c : encoded)
		{
			if (c == '=')
				break;
			const unsigned int digit = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : 63;
			value = (value << 6) | digit;
			bits += 6;
			if (bits >= 8)
			{
				bits -= 8;
				decoded.push_back(static_cast<char>((value >> bits) & 255));
			}
		}
		return decoded;
	}

	struct TransferResult
	{
		int status = 200;
		std::string error;
		std::string id;
		std::size_t next_offset = 0;
		std::string body;
		bool Ok() const
		{
			return status == 200;
		}
	};

	inline constexpr std::size_t kMaxInFlightRequests = 8;
	inline constexpr std::size_t kMaxResponseBytes = 32 * 1024 * 1024;
	inline constexpr std::size_t kMaxResponseTransfers = 16;
	inline constexpr std::size_t kResponseChunkBytes = 128 * 1024;
	inline constexpr std::chrono::seconds kResponseTransferTtl{120};

	/// The callback lease survives queued UI dispatch and asynchronous completion without owning AppState.
	class RequestAdmission
	{
		struct State
		{
			std::mutex mutex;
			std::size_t bytes = 0;
			std::size_t count = 0;
		};
	  public:
		class Lease
		{
		  public:
			~Lease()
			{
				if (!m_admitted) return;
				std::lock_guard<std::mutex> lock(m_state->mutex);
				m_state->bytes -= m_bytes;
				--m_state->count;
			}
			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;
		  private:
			friend class RequestAdmission;
			Lease(std::shared_ptr<State> state, std::size_t bytes) : m_state(std::move(state)), m_bytes(bytes) {}
			std::shared_ptr<State> m_state;
			std::size_t m_bytes;
			bool m_admitted = false;
		};
		std::shared_ptr<Lease> Acquire(std::size_t bytes)
		{
			std::shared_ptr<Lease> lease(new Lease(m_state, bytes));
			std::lock_guard<std::mutex> lock(m_state->mutex);
			if (bytes == 0 || bytes > kMaxRequestBytes || m_state->count >= kMaxInFlightRequests || bytes > kMaxRequestBytes - m_state->bytes) return {};
			m_state->bytes += bytes;
			++m_state->count;
			lease->m_admitted = true;
			return lease;
		}
	  private:
		std::shared_ptr<State> m_state = std::make_shared<State>();
	};

	struct ResponseDescriptor
	{
		std::string id;
		std::size_t total_bytes = 0;
	};
	struct ResponseChunk
	{
		std::string bytes;
		std::size_t next_offset = 0;
		bool done = false;
	};

	/// Thread-safe because serialization callbacks store before posting only bounded wire replies.
	class ResponseTransfers
	{
	  public:
		using Clock = std::chrono::steady_clock;
		using IdFactory = std::function<std::string()>;
		explicit ResponseTransfers(IdFactory factory) : m_id_factory(std::move(factory)) {}
		std::optional<ResponseDescriptor> Store(std::string body, Clock::time_point now = Clock::now())
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Prune(now);
			if (body.empty() || body.size() > kMaxResponseBytes || m_transfers.size() >= kMaxResponseTransfers || body.size() > kMaxResponseBytes - m_bytes) return std::nullopt;
			const std::string id = m_id_factory();
			if (id.empty() || m_transfers.contains(id)) return std::nullopt;
			const std::size_t bytes = body.size();
			m_transfers.emplace(id, Transfer{std::move(body), now});
			m_bytes += bytes;
			return ResponseDescriptor{id, bytes};
		}
		std::optional<ResponseChunk> Read(const std::string& id, std::size_t offset, Clock::time_point now = Clock::now())
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Prune(now);
			const auto found = m_transfers.find(id);
			if (found == m_transfers.end() || offset >= found->second.body.size()) return std::nullopt;
			auto& transfer = found->second;
			transfer.touched = now;
			const std::size_t count = std::min(kResponseChunkBytes, transfer.body.size() - offset);
			ResponseChunk result{transfer.body.substr(offset, count), offset + count, offset + count == transfer.body.size()};
			if (result.done)
			{
				m_bytes -= transfer.body.size();
				m_transfers.erase(found);
			}
			return result;
		}
		void Expire(Clock::time_point now = Clock::now())
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Prune(now);
		}
		void Clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_transfers.clear();
			m_bytes = 0;
		}
	  private:
		struct Transfer { std::string body; Clock::time_point touched; };
		void Prune(Clock::time_point now)
		{
			for (auto it = m_transfers.begin(); it != m_transfers.end();)
			{
				if (now - it->second.touched >= kResponseTransferTtl)
				{
					m_bytes -= it->second.body.size();
					it = m_transfers.erase(it);
				}
				else ++it;
			}
		}
		std::mutex m_mutex;
		IdFactory m_id_factory;
		std::unordered_map<std::string, Transfer> m_transfers;
		std::size_t m_bytes = 0;
	};

	// Owned by the transport thread; injected UUID and clock keep this CEF/OS free.
	class RequestTransfers
	{
	  public:
		using Clock = std::chrono::steady_clock;
		using IdFactory = std::function<std::string()>;
		explicit RequestTransfers(IdFactory id_factory) : m_id_factory(std::move(id_factory))
		{
		}

		TransferResult Begin(std::size_t total_bytes, Clock::time_point now = Clock::now())
		{
			Prune(now);
			if (total_bytes == 0 || total_bytes > kMaxRequestBytes)
				return Failure(413, "Request transfer must contain 1 to 33554432 bytes.");
			if (m_transfers.size() >= kMaxRequestTransfers || total_bytes > kMaxRequestBytes - m_reserved_bytes)
				return Failure(503, "Request transfer capacity exhausted.");
			const std::string id = m_id_factory();
			if (id.empty() || m_transfers.contains(id))
				return Failure(503, "Request transfer identity unavailable.");
			m_transfers.emplace(id, Transfer{total_bytes, {}, now});
			m_reserved_bytes += total_bytes;
			TransferResult result;
			result.id = id;
			return result;
		}

		TransferResult Append(const std::string& id, std::size_t offset, std::string_view encoded, Clock::time_point now = Clock::now())
		{
			Prune(now);
			const auto found = m_transfers.find(id);
			if (found == m_transfers.end())
				return Failure(404, "Request transfer not found or expired.");
			Transfer& transfer = found->second;
			if (offset != transfer.body.size())
				return Failure(409, "Request chunks must have contiguous offsets.");
			if (encoded.size() > (kRequestChunkBytes + 2) / 3 * 4)
				return Failure(413, "Request chunk exceeds 32768 bytes.");
			const std::optional<std::size_t> size = Base64DecodedSize(encoded);
			if (!size || *size == 0)
				return Failure(400, "Request chunk must be non-empty canonical base64.");
			if (*size > kRequestChunkBytes || *size > transfer.total_bytes - offset)
				return Failure(413, "Request chunk exceeds its declared bounds.");
			transfer.body += DecodeBase64(encoded, *size);
			TransferResult result;
			result.next_offset = transfer.body.size();
			return result;
		}

		TransferResult Commit(const std::string& id, Clock::time_point now = Clock::now())
		{
			Prune(now);
			const auto found = m_transfers.find(id);
			if (found == m_transfers.end())
				return Failure(404, "Request transfer not found or expired.");
			// Consume before parsing or dispatch, including incomplete/invalid commits.
			Transfer transfer = std::move(found->second);
			m_reserved_bytes -= transfer.total_bytes;
			m_transfers.erase(found);
			if (transfer.body.size() != transfer.total_bytes)
				return Failure(409, "Request transfer is incomplete and was discarded.");
			TransferResult result;
			result.body = std::move(transfer.body);
			return result;
		}

		void Clear()
		{
			m_transfers.clear();
			m_reserved_bytes = 0;
		}

		void Expire(Clock::time_point now = Clock::now())
		{
			Prune(now);
		}

	  private:
		struct Transfer
		{
			std::size_t total_bytes;
			std::string body;
			Clock::time_point created;
		};
		static TransferResult Failure(int status, std::string error)
		{
			TransferResult result;
			result.status = status;
			result.error = std::move(error);
			return result;
		}
		void Prune(Clock::time_point now)
		{
			for (auto it = m_transfers.begin(); it != m_transfers.end();)
			{
				if (now - it->second.created >= kRequestTransferTtl)
				{
					m_reserved_bytes -= it->second.total_bytes;
					it = m_transfers.erase(it);
				}
				else
					++it;
			}
		}
		IdFactory m_id_factory;
		std::unordered_map<std::string, Transfer> m_transfers;
		std::size_t m_reserved_bytes = 0;
	};
} // namespace uam::companion
