#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

struct PreparedChatSave;
namespace uam::native_assets { class PreparedBatch; }

namespace uam
{
	namespace native_agents { class LocalPassiveHistoryLoader; }

	/// A genuine local reader result. Only the reader can construct it; callers
	/// can inspect but cannot substitute messages or invent a resume identity.
	class AdmittedLocalPassiveNativeHistory final
	{
	public:
		AdmittedLocalPassiveNativeHistory(AdmittedLocalPassiveNativeHistory&&) noexcept = default;
		AdmittedLocalPassiveNativeHistory& operator=(AdmittedLocalPassiveNativeHistory&&) noexcept = default;
		AdmittedLocalPassiveNativeHistory(const AdmittedLocalPassiveNativeHistory&) = delete;
		AdmittedLocalPassiveNativeHistory& operator=(const AdmittedLocalPassiveNativeHistory&) = delete;
		const ChatSession& Candidate() const { return m_chat; }
		const std::optional<ChatSession>& Previous() const { return m_previous; }
	private:
		AdmittedLocalPassiveNativeHistory() = default;
		friend class native_agents::LocalPassiveHistoryLoader;
		friend class PreparedNativeMediaChatSave;
		ChatSession m_chat;
		std::optional<ChatSession> m_previous;
		std::filesystem::path m_root;
		std::string m_reference, m_parent_native_id, m_workspace, m_source_id, m_source_birth, m_primary, m_summary;
	};

	/// Final metadata staged from the actual owner after loading, never a new
	/// transcript/queue/native identity. Serialization still occurs on a worker.
	struct PassiveNativeOpenMetadata
	{
		std::shared_ptr<const std::uint8_t> incarnation;
		std::string display_parent_chat_id, display_parent_created_at;
		std::optional<std::string> last_opened_at;
	};

	/// Detached native-history save. Prepare, acknowledgement and destruction are
	/// worker-only. The caller must admit the final UAM owner/root/native identity
	/// before preparation and revalidate live ownership immediately before Publish.
	/// This object grants no permission to resume, replay or control a provider.
	class PreparedNativeMediaChatSave final
	{
	public:
		static std::unique_ptr<PreparedNativeMediaChatSave> Prepare(
		    const std::filesystem::path& resolved_root, ChatSession detached,
		    std::string_view admitted_storage_revision, std::string& error, std::stop_token cancel = {});
		static std::unique_ptr<PreparedNativeMediaChatSave> PreparePassive(
		    const std::filesystem::path& resolved_root, AdmittedLocalPassiveNativeHistory admitted,
		    const PassiveNativeOpenMetadata& metadata, std::string_view admitted_storage_revision,
		    std::string& error, std::stop_token cancel = {});
		~PreparedNativeMediaChatSave();
		PreparedNativeMediaChatSave(const PreparedNativeMediaChatSave&) = delete;
		PreparedNativeMediaChatSave& operator=(const PreparedNativeMediaChatSave&) = delete;
		const ChatSession& Candidate() const { return m_chat; }
		/// Exactly one publication attempt, through existing repository guards.
		bool Publish(std::string& error, const std::function<bool()>& still_owned = {}, bool sync_directory = true);
		bool PrimaryAcknowledged() const { return m_primary_acknowledged; }
		/// Worker only, after actual primary acknowledgement. A failed receipt does
		/// not roll back an acknowledged chat or authorize deleting its originals.
		bool AcknowledgeOriginals(std::string& error);
		std::optional<ChatSession> TakeCommittedChat();
	private:
		static std::unique_ptr<PreparedNativeMediaChatSave> PrepareAdmitted(
		    const std::filesystem::path& root, ChatSession detached, std::string_view revision,
		    std::string& error, std::stop_token cancel);
		PreparedNativeMediaChatSave() = default;
		ChatSession m_chat;
		std::shared_ptr<PreparedChatSave> m_prepared;
		std::unique_ptr<native_assets::PreparedBatch> m_originals;
		bool m_publish_attempted = false;
		bool m_primary_acknowledged = false;
		bool m_chat_taken = false;
	};
}
