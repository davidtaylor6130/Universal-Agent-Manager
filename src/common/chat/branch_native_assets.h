#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace uam
{
	namespace native_assets
	{
		class PreparedBatch;
	}

	/// Metadata-only admission predicate; no original reads or path/URI interpretation.
	bool BranchPrefixHasOwnedOriginals(const ChatSession& source, int message_index);
	/// Same branch request across a source-owned/branch-owned retained representation.
	/// All fields except a correctly scoped owned-original key remain exact.
	bool BranchRequestAttachmentsEquivalent(const std::vector<MessageAttachment>& source, const std::vector<MessageAttachment>& branch, const std::string& source_owner, const std::string& branch_owner);

	/// Invocation-scoped retention token. Prepare/ACK/destruction are worker-only;
	/// UI callers borrow a ready token whose lifetime belongs to a bounded worker owner.
	/// No provider input, native resume identity or URI/path fetch is authorized.
	class PreparedBranchOriginals final
	{
	  public:
		static std::unique_ptr<PreparedBranchOriginals> Prepare(const std::filesystem::path& root, const ChatSession& source, const std::string& final_branch_id, int message_index, const std::optional<std::vector<std::string>>& retained_attachment_ids, std::string& error, std::stop_token cancel = {}, const std::optional<std::vector<std::string>>& retained_markdown_store_files = std::nullopt);
		~PreparedBranchOriginals();
		PreparedBranchOriginals(const PreparedBranchOriginals&) = delete;
		PreparedBranchOriginals& operator=(const PreparedBranchOriginals&) = delete;
		const std::string& FinalBranchId() const
		{
			return m_final_owner;
		}
		bool MatchesRequest(const std::filesystem::path& root, const std::string& source_id, int message_index, const std::optional<std::vector<std::string>>& retained_attachment_ids, const std::optional<std::vector<std::string>>& retained_markdown_store_files = std::nullopt) const;
		bool MatchesSource(const ChatSession& source) const;
		/// Checks captured skill pairs, then rebinds only native keys; no original reads, ACK or publication.
		bool ApplyTo(ChatSession& candidate, std::string& error);
		void NotePrimaryAcknowledged() noexcept
		{
			m_primary_acknowledged = true;
		}
		bool PrimaryAcknowledged() const noexcept
		{
			return m_primary_acknowledged;
		}
		bool AcknowledgeOriginals(std::string& error);

	  private:
		PreparedBranchOriginals() = default;
		struct Part
		{
			std::size_t row = 0, source_part = 0, branch_part = 0;
			MessageAttachment expected;
			std::string replacement_key;
		};
		std::filesystem::path m_root;
		std::string m_source_owner, m_final_owner;
		int m_message_index = -1;
		std::optional<std::vector<std::string>> m_retained;
		// Only the original prefix skill fields are witnessed, independently of native-part metadata.
		struct SkillContext
		{
			std::vector<std::string> files, blocks;
		};
		std::optional<std::vector<std::string>> m_retained_skills;
		std::vector<SkillContext> m_skill_prefix;
		std::vector<Part> m_parts;
		std::unique_ptr<native_assets::PreparedBatch> m_batch;
		bool m_ready = false, m_consumed = false, m_primary_acknowledged = false;
	};
} // namespace uam
