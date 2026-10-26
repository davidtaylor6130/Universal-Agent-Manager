#pragma once

#include "app/data_folder_preflight.h"
#include "common/platform/platform_services.h"
#include "common/platform/data_folder_tree.h"
#include "common/platform/retained_directory_identity.h"
#include "common/platform/data_folder_writer_lease.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class IPlatformProcessService;

namespace uam
{
	/// Owns a stopped transaction and both root leases. A failure never drops an acquired lease.
	/// The caller must retain this object through the active profile's entire lifetime.
	class DataFolderTransition final
	{
	  public:
		using Fault = std::function<bool(std::string_view, io::AtomicWriteStage)>;
		static bool ProbeStartupRoot(const std::filesystem::path& default_root, const std::optional<std::filesystem::path>& external_override, std::filesystem::path& selected, std::string& error, const platform::DataFolderTree::ReadObserver& control_read_observer = {});
		static std::shared_ptr<DataFolderTransition> Open(const std::filesystem::path& control_root, const std::filesystem::path& current_root, std::shared_ptr<uam::platform::DataRootLock> current_lease, std::shared_ptr<uam::platform::DataRootLock> control_lease, const IPlatformProcessService& process, std::string& error, const platform::DataFolderTree::ReadObserver& source_read_observer = {}, const platform::DataFolderTree::ReadObserver& control_read_observer = {});
		bool Arm(const std::filesystem::path& destination, std::string_view preference_revision, std::string& error, const Fault& fault = {});
		DataFolderPreflightResult InspectDestination(const std::filesystem::path& destination) const;
		bool ActivateStopped(bool confirmed_normal_exit, std::string& error, const Fault& fault = {});
		bool ConfirmDestinationOwner(std::string& error, const Fault& fault = {});
		bool RollbackBeforeActivation(std::string& error, const Fault& fault = {});
		std::filesystem::path CurrentRoot() const;
		std::shared_ptr<uam::platform::DataRootLock> CurrentLease() const;
		std::string State() const;
		bool Held() const
		{
			return m_held;
		}
		bool Armed() const;
		bool MatchesPendingChoice(const std::filesystem::path& destination, std::string_view durable_revision) const;
		bool RetainsPassiveDestination(const std::filesystem::path& destination) const;
		static bool CheckExternalSourceFence(const std::filesystem::path& root, std::string& error, bool allow_authoritative_root = false, const platform::DataFolderTree::ReadObserver& control_read_observer = {});

	  private:
		DataFolderPreflightResult InspectDestination(const std::filesystem::path& destination, DataFolderDestinationCheck destination_check) const;
		bool Save(std::string_view phase, std::string& error, const Fault& fault);
		bool Receipt(const std::shared_ptr<platform::DataFolderTree>& tree, std::string_view role, std::string_view phase, std::string& error, const Fault& fault);
		bool ValidateRetained(std::string& error);
		bool LoadRetainedHistory(std::string& error);
		bool ValidateHistory(std::string& error);
		bool OwnsRoots() const;
		struct RetainedSource
		{
			std::shared_ptr<platform::DataFolderTree> tree;
			std::shared_ptr<uam::platform::DataRootLock> lease;
			nlohmann::json record;
			bool passive = false;
			std::shared_ptr<platform::DataFolderWriterLease> writers;
		};
		std::vector<RetainedSource> m_retained;
		bool Hold(std::string& error, const std::string& reason);
		const IPlatformProcessService* m_process = nullptr;
		std::shared_ptr<platform::DataFolderTree> m_control;
		std::shared_ptr<const platform::RetainedDirectoryIdentity> m_bootstrapIdentity;
		std::shared_ptr<platform::DataFolderTree> m_source;
		std::shared_ptr<platform::DataFolderTree> m_destination;
		std::shared_ptr<uam::platform::DataRootLock> m_sourceLease;
		std::shared_ptr<uam::platform::DataRootLock> m_controlLease;
		std::shared_ptr<uam::platform::DataRootLock> m_destinationLease;
		std::shared_ptr<platform::DataFolderWriterLease> m_sourceWriters, m_destinationWriters;
		nlohmann::json m_record;
		std::string m_recordBytes;
		std::string m_intentBytes;
		bool m_held = false;
		bool m_destinationOwner = false;
	};
} // namespace uam
