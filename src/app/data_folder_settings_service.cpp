#include "data_folder_settings_service.h"

#include "app/data_folder_preflight.h"
#include "app/data_folder_transition.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/platform/retained_directory_identity.h"
#include "common/utils/io_utils.h"

#include <nlohmann/json.hpp>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	constexpr std::size_t kMaxPreferenceBytes = 64 * 1024;

	bool Exists(const fs::path& path, bool& exists)
	{
		std::error_code error;
		const auto status = fs::symlink_status(path, error);
		if (error == std::errc::no_such_file_or_directory)
		{
			exists = false;
			return true;
		}
		if (error)
		{
			return false;
		}
		exists = fs::exists(status);
		return true;
	}

	bool ReadRecord(const fs::path& path, std::string& text)
	{
		const auto witness = uam::platform::PublishedFileChangeWitness::Capture(path);
		const auto bytes = witness ? witness->ReadBounded(kMaxPreferenceBytes) : std::nullopt;
		if (!bytes)
		{
			return false;
		}
		text = *bytes;
		return true;
	}

	bool ValidAbsolute(const fs::path& path)
	{
		if (path.empty() || !path.is_absolute() || path == path.root_path() || path.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
		{
			return false;
		}
		for (const fs::path& component : path)
		{
			if (component == "." || component == "..")
			{
				return false;
			}
		}
		return true;
	}
} // namespace

std::string uam::DataFolderSettingsService::Encode(const Choice& choice)
{
	return nlohmann::json{{"version", 1}, {"source", uam::paths::Utf8PathString(choice.source)}, {"destination", uam::paths::Utf8PathString(choice.destination)}, {"revision", choice.revision}, {"operationId", choice.operation_id}, {"phase", choice.destination.empty() ? "stable" : "awaitingMigration"}}.dump();
}

bool uam::DataFolderSettingsService::Decode(const std::string& text, Choice& choice)
{
	const auto json = nlohmann::json::parse(text, nullptr, false);
	if (!json.is_object() || json.size() != 6 || !json.contains("version") || !json["version"].is_number_integer() || json["version"] != 1 || !json.contains("source") || !json["source"].is_string() || !json.contains("destination") || !json["destination"].is_string() || !json.contains("revision") || !json["revision"].is_string() || !json.contains("phase") || !json["phase"].is_string() || !json.contains("operationId") || !json["operationId"].is_string())
	{
		return false;
	}
	choice.source = uam::paths::PathFromUtf8(json["source"].get<std::string>());
	choice.destination = uam::paths::PathFromUtf8(json["destination"].get<std::string>());
	choice.revision = json["revision"].get<std::string>();
	choice.operation_id = json["operationId"].get<std::string>();
	return ValidAbsolute(choice.source) && (choice.destination.empty() || ValidAbsolute(choice.destination)) && !choice.revision.empty() && choice.revision.size() <= 128 && choice.operation_id.size() <= 128 && json["phase"] == (choice.destination.empty() ? "stable" : "awaitingMigration");
}

std::shared_ptr<uam::DataFolderSettingsService> uam::DataFolderSettingsService::Open(const fs::path& default_root, const std::optional<fs::path>& external_override, const IPlatformProcessService& process, std::string& error)
{
	error.clear();
	auto service = std::shared_ptr<DataFolderSettingsService>(new DataFolderSettingsService());
	service->m_process = &process;
	service->m_externalOverride = external_override.has_value();
	service->m_startupRoot = external_override.value_or(default_root);
	service->m_scope = process.GenerateUuid();
	if (service->m_scope.empty())
	{
		error = "Could not create the data-folder preference scope.";
		return {};
	}
	if (external_override)
	{
		if (!DataFolderTransition::CheckExternalSourceFence(*external_override, error))
			return {};
		return service;
	}
	std::error_code filesystem_error;
	service->m_controlRoot = fs::absolute(default_root, filesystem_error) / ".data-folder-control";
	if (filesystem_error || uam::paths::IsLinkOrReparsePointNoThrow(service->m_controlRoot))
	{
		error = "The data-folder preference directory is unavailable or linked.";
		return {};
	}
	service->m_controlLease = process.TryAcquireDataRootLock(service->m_controlRoot, &error);
	if (!service->m_controlLease)
	{
		return {};
	}
	service->m_controlIdentity = platform::RetainedDirectoryIdentity::Capture(service->m_controlRoot);
	if (!service->m_controlIdentity)
	{
		error = "Could not retain data-folder preference ownership.";
		return {};
	}
	const fs::path primary = service->m_controlRoot / "selection.json";
	const fs::path intent = service->m_controlRoot / "intent.json";
	bool has_intent = false;
	if (!Exists(primary, service->m_primaryExists) || !Exists(intent, has_intent))
	{
		error = "Could not inspect the data-folder preference records.";
		return {};
	}
	service->m_choice.source = fs::canonical(default_root, filesystem_error);
	if (filesystem_error)
	{
		error = "Could not resolve the default data folder.";
		return {};
	}
	service->m_choice.revision = "initial";
	if (service->m_primaryExists && (!ReadRecord(primary, service->m_primaryBytes) || !Decode(service->m_primaryBytes, service->m_choice)))
	{
		error = "The data-folder preference is invalid. UAM will not open another profile.";
		return {};
	}
	if (service->m_primaryExists && !has_intent)
	{
		service->m_held = true;
	}
	if (has_intent)
	{
		if (!ReadRecord(intent, service->m_intentBytes))
		{
			error = "The data-folder publication intent is unreadable.";
			return {};
		}
		const auto journal = nlohmann::json::parse(service->m_intentBytes, nullptr, false);
		Choice before, after;
		if (!journal.is_object() || journal.size() != 4 || !journal.contains("version") || journal["version"] != 1 || !journal.contains("phase") || (journal["phase"] != "publishing" && journal["phase"] != "acknowledged") || !journal.contains("before") || !journal.contains("after") || !Decode(journal["before"].dump(), before) || !Decode(journal["after"].dump(), after) || before.source != after.source)
		{
			error = "The data-folder publication intent is invalid. Recovery requires inspection.";
			return {};
		}
		// A complete matching staging receipt can be observed again; an interrupted one is never replayed.
		if (journal["phase"] != "acknowledged" || !service->m_primaryExists || service->m_primaryBytes != Encode(after))
		{
			service->m_choice = after;
			service->m_choice.source = before.source;
			service->m_held = true;
		}
	}
	// Version 1 only stages choices. No record in this version can authorize activating a custom root.
	if (!fs::equivalent(service->m_choice.source, default_root, filesystem_error) || filesystem_error)
	{
		error = "The data-folder source does not match this installation's default root. No profile was opened.";
		return {};
	}
	if (!service->m_controlIdentity->Current())
	{
		error = "The data-folder control directory changed during startup. No alternate profile was opened.";
		return {};
	}
	if (!DataFolderTransition::ProbeStartupRoot(default_root, {}, service->m_startupRoot, error))
		return {};
	return service;
}

std::filesystem::path uam::DataFolderSettingsService::StartupRoot() const
{
	return m_startupRoot;
}

void uam::DataFolderSettingsService::BindSourceLease(std::weak_ptr<uam::platform::DataRootLock> lease)
{
	std::string error;
	(void)AttachStartupOwner(lease.lock(), false, error);
}

bool uam::DataFolderSettingsService::AttachStartupOwner(std::shared_ptr<uam::platform::DataRootLock> lease, bool confirmed_normal_exit, std::string& error, bool recover_passive)
{
	std::lock_guard lock(m_mutex);
	m_sourceLease = lease;
	if (!lease || !lease->Current())
	{
		m_transition.reset();
		m_sourceIdentity.reset();
		return false;
	}
	if (m_externalOverride)
		return lease != nullptr;
	if (!m_transition)
		m_transition = DataFolderTransition::Open(m_controlRoot, m_startupRoot, lease, m_controlLease, *m_process, error);
	if (!m_transition)
	{
		m_held = true;
		return false;
	}
	if (m_transition->Held())
	{
		// Do not start even source writers against a retained passive snapshot. Explicit recovery
		// must first durably roll back a proven passive destination while retaining both leases.
		if (!recover_passive || !m_transition->RollbackBeforeActivation(error))
			return false;
	}
	if (!m_held && m_transition->Armed() && confirmed_normal_exit)
	{
		if (m_choice.destination.empty() || !m_transition->MatchesPendingChoice(m_choice.destination, m_choice.revision))
		{
			m_held = true;
			error = "The saved folder choice no longer matches the armed transaction. No destination was activated.";
			return false;
		}
		if (!m_transition->ActivateStopped(true, error) || !m_transition->ConfirmDestinationOwner(error))
			return false;
	}
	if (m_transition->State() == "active")
		m_startupRoot = m_transition->CurrentRoot();
	m_sourceLease = m_transition->CurrentLease();
	m_sourceIdentity = platform::RetainedDirectoryIdentity::Capture(m_transition->CurrentRoot());
	return m_sourceIdentity != nullptr;
}

std::shared_ptr<uam::platform::DataRootLock> uam::DataFolderSettingsService::StartupLease() const
{
	std::lock_guard lock(m_mutex);
	return m_transition ? m_transition->CurrentLease() : m_sourceLease.lock();
}

uam::DataFolderSettingsService::Result uam::DataFolderSettingsService::ArmRestart(const std::string& expected_revision)
{
	std::lock_guard lock(m_mutex);
	std::string error;
	if (m_externalOverride || m_held || !m_transition || m_choice.destination.empty() || expected_revision != TokenLocked() || !CheckPrimaryLocked())
		return {false, SnapshotLocked(), "The folder choice is overridden, uncertain or stale."};
	const bool armed = m_transition->Arm(m_choice.destination, m_choice.revision, error);
	return {armed, SnapshotLocked(), error};
}

uam::DataFolderSettingsService::Result uam::DataFolderSettingsService::RollbackPendingTransfer(const std::string& expected_revision)
{
	std::lock_guard lock(m_mutex);
	std::string error;
	if (m_externalOverride || m_held || !m_transition || expected_revision != TokenLocked())
		return {false, SnapshotLocked(), "Recovery requires the exact observed local transition owner."};
	const bool rolled_back = m_transition->RollbackBeforeActivation(error);
	return {rolled_back, SnapshotLocked(), error};
}

std::string uam::DataFolderSettingsService::TokenLocked() const
{
	return m_scope + "/" + m_choice.revision;
}

uam::DataFolderSettingsService::Snapshot uam::DataFolderSettingsService::SnapshotLocked() const
{
	Snapshot state;
	state.current_folder = m_startupRoot;
	const bool active = m_transition && m_transition->State() == "active" && m_choice.destination == m_startupRoot;
	const bool held = m_held || (m_transition && m_transition->Held());
	const bool passive_choice = m_transition && m_transition->RetainsPassiveDestination(m_choice.destination);
	state.pending_folder = active ? std::filesystem::path{} : m_choice.destination;
	state.external_override = m_externalOverride;
	state.revision = TokenLocked();
	state.last_operation_id = m_choice.operation_id;
	state.can_change = !m_externalOverride && !held && !m_sourceLease.expired() && m_sourceIdentity && m_sourceIdentity->Current();
	state.restart_ready = kTransfersValidated && state.can_change && !state.pending_folder.empty() && !passive_choice;
	state.state = held ? "publicationUnknown" : !state.pending_folder.empty() ? "awaitingMigration" : "none";
	state.message = m_externalOverride ? "UAM_DATA_DIR controls this launch. Folder choices are unavailable while the startup override is set." : held ? "The folder-choice or handoff publication is unconfirmed. Further changes remain held for recovery." : passive_choice ? "The source was restored and the passive copy is retained. Cancel this choice before choosing another empty folder." : active ? "The selected data folder is active. The original profile is retained and locked for recovery." : !m_choice.destination.empty() ? (kTransfersValidated ? "Folder choice saved. Stop active provider sessions before requesting a guarded restart." : "Folder choice saved. Transfer is unavailable in this build; restarting will keep the current folder.") : "Choose a separate empty folder. Existing chats stay in the current folder until a verified transfer is available.";
	return state;
}

uam::DataFolderSettingsService::Snapshot uam::DataFolderSettingsService::Read()
{
	std::lock_guard lock(m_mutex);
	if (!m_externalOverride && !m_held && !m_sourceLease.expired())
	{
		(void)CheckPrimaryLocked();
	}
	return SnapshotLocked();
}

bool uam::DataFolderSettingsService::CheckPrimaryLocked()
{
	const auto source_lease = m_sourceLease.lock();
	bool exists = false, intent = false;
	std::string bytes, intent_bytes;
	if (!m_controlLease || !m_controlLease->Current() || !source_lease || !source_lease->Current() || !m_controlIdentity || !m_controlIdentity->Current() || !m_sourceIdentity || !m_sourceIdentity->Current() || uam::paths::IsLinkOrReparsePointNoThrow(m_controlRoot) || !Exists(m_controlRoot / "selection.json", exists) || !Exists(m_controlRoot / "intent.json", intent) || intent != !m_intentBytes.empty() || (intent && (!ReadRecord(m_controlRoot / "intent.json", intent_bytes) || intent_bytes != m_intentBytes)) || exists != m_primaryExists || (exists && (!ReadRecord(m_controlRoot / "selection.json", bytes) || bytes != m_primaryBytes)))
	{
		m_held = true;
		return false;
	}
	return true;
}

uam::DataFolderSettingsService::Result uam::DataFolderSettingsService::PublishLocked(Choice candidate, const WriteGate& gate)
{
	if (!CheckPrimaryLocked())
	{
		return {false, SnapshotLocked(), "The saved folder choice changed or is uncertain. Further publication is held."};
	}
	candidate.revision = m_process->GenerateUuid();
	if (candidate.operation_id.empty())
	{
		candidate.operation_id = m_process->GenerateUuid();
	}
	if (candidate.revision.empty() || candidate.operation_id.empty() || candidate.operation_id.size() > 128)
	{
		return {false, SnapshotLocked(), "Could not create the folder-choice revision or action identity."};
	}
	const std::string bytes = Encode(candidate);
	auto journal = nlohmann::json{{"version", 1}, {"phase", "publishing"}, {"before", nlohmann::json::parse(Encode(m_choice))}, {"after", nlohmann::json::parse(bytes)}};
	const std::string publishing = journal.dump();
	journal["phase"] = "acknowledged";
	const std::string acknowledged = journal.dump();
	if (bytes.size() > kMaxPreferenceBytes || publishing.size() > kMaxPreferenceBytes || acknowledged.size() > kMaxPreferenceBytes)
	{
		return {false, SnapshotLocked(), "The selected folder exceeds the bootstrap record limit. Choose a shorter path."};
	}
	const auto owned = [this](uam::io::AtomicWriteStage) { const auto source = m_sourceLease.lock(); return source && source->Current() && m_controlLease && m_controlLease->Current() && m_controlIdentity && m_controlIdentity->Current() && m_sourceIdentity && m_sourceIdentity->Current(); };
	const auto intent_result = uam::io::AtomicWriteFileDetailed(m_controlRoot / "intent.json", publishing, owned);
	if (!intent_result.success)
	{
		m_held = true;
		return {false, SnapshotLocked(), "Could not confirm the folder-choice publication intent. No data was transferred."};
	}
	const auto result = uam::io::AtomicWriteFileDetailed(m_controlRoot / "selection.json", bytes, [&owned, &gate](uam::io::AtomicWriteStage stage) { return (!gate || gate(stage)) && owned(stage); });
	if (!result.success)
	{
		// Retain both the candidate and its prior source in the durable intent; never overwrite uncertain state.
		m_held = true;
		return {false, SnapshotLocked(), "The folder-choice save is unconfirmed. No data was transferred; publication is held."};
	}
	const auto receipt = uam::io::AtomicWriteFileDetailed(m_controlRoot / "intent.json", acknowledged, owned);
	if (!receipt.success || !owned(uam::io::AtomicWriteStage::Complete))
	{
		m_held = true;
		return {false, SnapshotLocked(), "The folder choice was written but its final acknowledgement is unconfirmed. Publication is held."};
	}
	m_choice = std::move(candidate);
	m_primaryBytes = bytes;
	m_intentBytes = acknowledged;
	m_primaryExists = true;
	return {true, SnapshotLocked(), {}};
}

uam::DataFolderSettingsService::Result uam::DataFolderSettingsService::Choose(const fs::path& destination, const std::string& expected_revision, const std::string& operation_id, const WriteGate& gate)
{
	std::lock_guard lock(m_mutex);
	const auto source_lease = m_sourceLease.lock();
	if (m_externalOverride || m_held || !m_transition || m_transition->Held() || !source_lease || !m_controlLease || expected_revision != TokenLocked() || (!m_choice.destination.empty() && (m_transition->State() != "active" || m_choice.destination != m_startupRoot)))
	{
		return {false, SnapshotLocked(), "Folder selection is overridden, held, stale or already pending. Refresh before changing it."};
	}
	// Preserve the final relative default route, while validating its retained absolute identity.
	const auto preflight = m_transition->InspectDestination(destination);
	if (preflight.state != DataFolderPreflightState::RequiresStoppedValidation)
	{
		if (preflight.blocker == DataFolderPreflightBlocker::InspectionFailed)
			m_held = true;
		return {false, SnapshotLocked(), preflight.explanation};
	}
	Choice candidate = m_choice;
	candidate.destination = preflight.destination_root;
	candidate.operation_id = operation_id;
	return PublishLocked(std::move(candidate), gate);
}

uam::DataFolderSettingsService::Result uam::DataFolderSettingsService::Cancel(const std::string& expected_revision, const std::string& operation_id, const WriteGate& gate)
{
	std::lock_guard lock(m_mutex);
	const auto source_lease = m_sourceLease.lock();
	if (m_externalOverride || m_held || !source_lease || !m_controlLease || expected_revision != TokenLocked() || m_choice.destination.empty())
	{
		return {false, SnapshotLocked(), "Folder selection cannot be canceled while overridden, held, stale or absent. Refresh its state."};
	}
	Choice candidate = m_choice;
	candidate.destination.clear();
	candidate.operation_id = operation_id;
	std::string rollback_error;
	if (m_transition && m_transition->State() != "active" && !m_transition->RollbackBeforeActivation(rollback_error))
		return {false, SnapshotLocked(), rollback_error};
	return PublishLocked(std::move(candidate), gate);
}
