#pragma once
#include <string_view>
#include <nlohmann/json_fwd.hpp>

#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace uam::gemini
{
	struct HistoryProject;
}

namespace uam::native_assets
{
	class PayloadBudget;
	class InlinePayload;
}

enum class GeminiHistoryParseStatus
{
	Content,
	ConfirmedEmpty,
	UnsupportedOrInvalid,
	Cancelled
};

struct GeminiHistoryParseResult
{
	GeminiHistoryParseStatus status = GeminiHistoryParseStatus::UnsupportedOrInvalid;
	std::optional<ChatSession> chat;
	std::string error;
};

// Share across an operation/scan. Payload reservations release with their last
// handle. Metadata admissions are cumulative attempted work, including failed or
// discarded candidates; create a new context for a new operation/retry. A failed
// admission never authorizes a partial snapshot. No I/O or generic allocator.
struct GeminiHistoryParseContext
{
	static constexpr std::size_t kMaxAttachments = 256;
	static constexpr std::size_t kMaxMetadataBytes = 1024U * 1024U;
	static constexpr std::size_t kMaxUriBytes = 16U * 1024U;
	GeminiHistoryParseContext();
	std::shared_ptr<uam::native_assets::PayloadBudget> payload_budget;
	// Optional owner armed by the producer before parsing. Returned payloads
	// alias this owner so copies outside the discovery result retain its lease.
	// Only fresh originals enter its fixed slots; aliases cannot be fed back to
	// form a cycle, and no retention allocation can fail after payload creation.
	struct RetainedPayloads
	{
		RetainedPayloads() = default;
		RetainedPayloads(const RetainedPayloads&) = delete;
		RetainedPayloads& operator=(const RetainedPayloads&) = delete;
	  private:
		friend struct GeminiHistoryParseContext;
		std::array<std::shared_ptr<const uam::native_assets::InlinePayload>, kMaxAttachments> originals_;
		std::size_t count_ = 0;
	};
	std::shared_ptr<RetainedPayloads> payload_retirement;
	std::shared_ptr<const uam::native_assets::InlinePayload> MakePayload(std::string&& encoded, std::string mime, std::string& error, std::stop_token stop);
	std::string RetirementError();
	std::stop_token cancel;
	bool AdmitMetadata(std::size_t bytes, std::string& error);

  private:
	std::mutex mutex_;
	std::size_t attachments_ = 0;
	std::size_t metadata_bytes_ = 0;
	std::string retirement_error_;
};

struct GeminiJsonHistoryStoreOptions
{
	std::uintmax_t max_file_bytes = 0;
	std::size_t max_messages = 0;
	std::string execution_host_id = "local";
	std::shared_ptr<GeminiHistoryParseContext> context;
};

class GeminiJsonHistoryStore
{
  public:
	// Rich results are required at mutation boundaries: nullopt cannot establish
	// emptiness. Parsing moves inline strings into shared handles and performs no I/O.
	static GeminiHistoryParseResult ParseDocumentResult(nlohmann::json document, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {});
	static GeminiHistoryParseResult ParseTextResult(std::string_view text, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {});
	// File reading retains the existing native history/journal ownership boundary.
	static GeminiHistoryParseResult ParseFileResult(const std::filesystem::path& file_path, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {}, std::stop_token stop_token = {});
	static std::optional<ChatSession> ParseDocument(nlohmann::json document, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {});
	static std::optional<ChatSession> ParseText(std::string_view text, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {});
	static std::optional<ChatSession> ParseFile(const std::filesystem::path& file_path, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {}, std::stop_token stop_token = {});

	// An explicit project fences provider-owned enumeration; standalone callers retain their supplied directory.
	static std::vector<ChatSession> Load(const std::filesystem::path& chats_dir, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {}, std::stop_token stop_token = {}, const uam::gemini::HistoryProject* selected_project = nullptr);

	static bool SaveFile(const std::filesystem::path& file_path, const ChatSession& chat);
};

std::vector<ChatSession> LoadGeminiJsonHistoryForRuntime(const std::filesystem::path& chats_dir, const ProviderProfile& profile, const ProviderRuntimeHistoryLoadOptions& options, std::stop_token stop_token = {}, const std::shared_ptr<GeminiHistoryParseContext>& context = {});

std::vector<ProviderChatSource> DiscoverGeminiTmpChatSources(std::string* error_out = nullptr, std::stop_token stop_token = {});
