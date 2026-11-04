#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uam::native_assets
{
	inline constexpr std::size_t kMaxOriginalBytes = 25U * 1024U * 1024U;
	inline constexpr std::size_t kMaxBatchBytes = 50U * 1024U * 1024U;
	inline constexpr std::size_t kMaxAssetCount = 256;
	inline constexpr std::string_view kStoreDirectory = "native-attachment-assets-v1";

	struct Limits
	{
		std::size_t original_bytes = kMaxOriginalBytes;
		std::size_t aggregate_bytes = kMaxBatchBytes;
		std::size_t count = kMaxAssetCount;
	};

	class InlinePayload;
	using Payload = std::shared_ptr<const InlinePayload>;

	/// Share one budget across all retained candidates in a task/scan. Copies of a
	/// payload share its reservation; the last payload owner releases it.
	class PayloadBudget final
	{
	  public:
		explicit PayloadBudget(Limits limits = {});
		std::size_t UsedBytes() const;
		std::size_t UsedCount() const;

	  private:
		friend class InlinePayload;
		friend Payload MakeInlinePayload(std::string&&, std::string, const std::shared_ptr<PayloadBudget>&, std::string&, std::stop_token);
		bool Acquire(std::size_t bytes, std::string& error);
		void Release(std::size_t bytes);
		Limits m_limits;
		mutable std::mutex m_mutex;
		std::size_t m_bytes = 0;
		std::size_t m_count = 0;
	};

	/// Runtime-only immutable encoded original. No model/JSON serializer is supplied.
	/// Do not persist or project this handle to React. No URI or source path input.
	class InlinePayload final
	{
	  public:
		~InlinePayload();
		InlinePayload(const InlinePayload&) = delete;
		InlinePayload& operator=(const InlinePayload&) = delete;
		const std::string& MimeType() const { return m_mime; }
		const std::string& Digest() const { return m_digest; }
		std::size_t DecodedSize() const { return m_size; }

	  private:
		friend Payload MakeInlinePayload(std::string&&, std::string, const std::shared_ptr<PayloadBudget>&, std::string&, std::stop_token);
		friend class PreparedBatch;
		InlinePayload(std::string&& encoded, std::string mime, std::string digest, std::size_t size, std::shared_ptr<PayloadBudget> budget);
		std::string m_encoded;
		std::string m_mime;
		std::string m_digest;
		std::size_t m_size;
		std::shared_ptr<PayloadBudget> m_budget;
	};

	/// Pure: strict padded/canonical base64 and size preflight, bounded cancellable
	/// digesting, then move ownership of the encoded string. Error is never payload text.
	Payload MakeInlinePayload(std::string&& encoded, std::string mime,
	    const std::shared_ptr<PayloadBudget>& budget, std::string& error, std::stop_token cancel = {});

	struct AssetReference
	{
		std::string key;
		std::string mime_type;
		std::string digest;
		std::size_t size_bytes = 0;
	};

	enum class PublicationState
	{
		PreparedUnpublished,
		AttemptedUncertain,
		PublishedAcknowledged
	};

	struct PublicationReceipt
	{
		std::string owner_hash;
		std::string generation;
		PublicationState state = PublicationState::PreparedUnpublished;
	};

	enum class PreparationPoint { GenerationCreated, BeforeOriginal, OriginalChunk, OriginalSynced, BatchSynced };
	/// Optional deterministic interruption/observation seam; false aborts preparation.
	using PreparationHook = std::function<bool(PreparationPoint, std::size_t)>;

	class PreparedBatch final
	{
	  public:
		/// Unpublished destruction performs scoped filesystem cleanup; dispose on a worker.
		~PreparedBatch();
		PreparedBatch(PreparedBatch&&) noexcept;
		PreparedBatch& operator=(PreparedBatch&&) noexcept;
		PreparedBatch(const PreparedBatch&) = delete;
		PreparedBatch& operator=(const PreparedBatch&) = delete;

		/// Worker only. Root must resolve/create the data root and admit the exact
		/// final UAM owner first. Never use this from pure parsing/discovery or GUI completion.
		/// All inputs succeed together or fail explicitly; even an empty list is rejected.
		static std::unique_ptr<PreparedBatch> Prepare(const std::filesystem::path& resolved_data_root,
		    std::string_view admitted_final_owner, const std::vector<Payload>& originals,
		    std::string& error, std::stop_token cancel = {}, const PreparationHook& hook = {});

		const std::vector<AssetReference>& Assets() const;
		PublicationReceipt Receipt() const;

		/// REQUIRED before invoking ANY primary publication that can reference Assets().
		/// Sets in-memory retention FIRST, then syncs an attempt receipt. On false do
		/// not publish; generation is conservatively retained even on error/exception.
		bool MarkPublicationAttempted(std::string& error);
		/// Call only after actual primary acknowledgement, never from a save bool=false.
		/// Failure to persist this acknowledgement still retains the generation.
		bool MarkPublished(std::string& error);

	  private:
		struct Impl;
		explicit PreparedBatch(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> m_impl;
	};

	struct ReadResult
	{
		bool ok = false;
		std::string bytes;
		std::string error;
	};

	/// Syntax/owner check only; a key is not renderer authority or proof of existence.
	bool IsKeyForOwner(std::string_view admitted_final_owner, std::string_view key);
	/// Pure reference consistency check; an optional admitted owner additionally
	/// checks ownership. This does not establish existence or publication authority.
	bool IsReferenceValid(const AssetReference& reference, std::string_view admitted_final_owner = {});
	/// Exact, bounded, regular-file read of a declared prepared original with a
	/// durable publication-attempt receipt. Verifies owner, size and SHA256; no URI,
	/// arbitrary relative path, fallback path or source-file reader is accepted.
	ReadResult ReadOwned(const std::filesystem::path& resolved_data_root,
	    std::string_view admitted_final_owner, std::string_view key,
	    std::size_t max_bytes = kMaxOriginalBytes, std::stop_token cancel = {});
}
