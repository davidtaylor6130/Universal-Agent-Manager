#pragma once
#include "common/platform/platform_services.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

class IPlatformProcessService;

namespace uam::io
{
	enum class AtomicWriteStage;
}

namespace uam::platform
{
	class RetainedDirectoryIdentity;
}

namespace uam
{
	class DataFolderTransition;
	/// Bootstrap preference ownership is separate from ownership of the active profile.
	/// This service serializes folder choices and delegates stopped-profile handoff to its retained transaction owner.
	class DataFolderSettingsService
	{
	  public:
		struct Snapshot
		{
			std::filesystem::path current_folder;
			std::filesystem::path pending_folder;
			std::string revision;
			std::string last_operation_id;
			std::string state = "none";
			bool external_override = false;
			bool can_change = false;
			bool restart_ready = false;
			std::string message;
		};
		struct Result
		{
			bool committed = false;
			Snapshot settings;
			std::string error;
		};
		using WriteGate = std::function<bool(uam::io::AtomicWriteStage)>;
		// Public activation stays closed until native, CEF, platform and user-facing validation is adequate.
		static constexpr bool kTransfersValidated = false;

		/// Capture external_override before Application exports its effective UAM_DATA_DIR.
		/// An external override bypasses the shared preference entirely, including its control lock.
		/// The platform process service must outlive this service (the application uses the platform singleton).
		static std::shared_ptr<DataFolderSettingsService> Open(const std::filesystem::path& default_root, const std::optional<std::filesystem::path>& external_override, const IPlatformProcessService& process, std::string& error);
		std::filesystem::path StartupRoot() const;
		void BindSourceLease(std::weak_ptr<uam::platform::DataRootLock> lease);
		bool AttachStartupOwner(std::shared_ptr<uam::platform::DataRootLock> lease, bool confirmed_normal_exit, std::string& error, bool recover_passive = false);
		std::shared_ptr<uam::platform::DataRootLock> StartupLease() const;
		Result ArmRestart(const std::string& expected_revision);
		Result RollbackPendingTransfer(const std::string& expected_revision);
		Snapshot Read();
		Result Choose(const std::filesystem::path& destination, const std::string& expected_revision, const std::string& operation_id = {}, const WriteGate& gate = {});
		Result Cancel(const std::string& expected_revision, const std::string& operation_id = {}, const WriteGate& gate = {});

	  private:
		struct Choice
		{
			std::filesystem::path source;
			std::filesystem::path destination;
			std::string revision;
			std::string operation_id;
		};
		Snapshot SnapshotLocked() const;
		Result PublishLocked(Choice candidate, const WriteGate& gate);
		bool CheckPrimaryLocked();
		std::string TokenLocked() const;
		static std::string Encode(const Choice& choice);
		static bool Decode(const std::string& text, Choice& choice);

		mutable std::mutex m_mutex;
		std::shared_ptr<uam::platform::DataRootLock> m_controlLease;
		std::weak_ptr<uam::platform::DataRootLock> m_sourceLease;
		std::shared_ptr<DataFolderTransition> m_transition;
		std::shared_ptr<const platform::RetainedDirectoryIdentity> m_controlIdentity;
		std::shared_ptr<const platform::RetainedDirectoryIdentity> m_sourceIdentity;
		std::filesystem::path m_controlRoot;
		std::filesystem::path m_startupRoot;
		Choice m_choice;
		std::string m_scope;
		std::string m_primaryBytes;
		std::string m_intentBytes;
		bool m_primaryExists = false;
		bool m_externalOverride = false;
		bool m_held = false;
		const IPlatformProcessService* m_process = nullptr;
	};
} // namespace uam
