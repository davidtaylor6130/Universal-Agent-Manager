#include "data_folder_transition.h"
#include "app/data_folder_preflight.h"
#include "common/config/settings_store.h"
#include "common/chat/data_folder_execution_guard.h"
#include "common/chat/chat_folder_store.h"
#include "common/paths/path_utils.h"
#include "common/paths/data_folder_reference.h"
#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"

#include <set>
#include <map>
#include <limits>

namespace fs = std::filesystem;
using Tree = uam::platform::DataFolderTree;
using Writers = uam::platform::DataFolderWriterLease;
using Json = nlohmann::json;

namespace
{
	constexpr std::size_t kRecordLimit = 128 * 1024;
	const fs::path kReceipt = ".uam-data-folder/receipt.json";
	const fs::path kReceiptIntent = ".uam-data-folder/receipt-intent.json";
	bool Exists(const fs::path& path)
	{
		std::error_code error;
		const auto state = fs::symlink_status(path, error);
		return error != std::errc::no_such_file_or_directory && (error || fs::exists(state));
	}
	bool ReadJson(const fs::path& path, Json& json, std::string& bytes)
	{
		const auto file = uam::platform::PublishedFileChangeWitness::Capture(path);
		const auto content = file ? file->ReadBounded(kRecordLimit) : std::nullopt;
		if (!content)
			return false;
		bytes = *content;
		json = Json::parse(bytes, nullptr, false);
		return json.is_object();
	}
	bool Valid(const Json& record, unsigned depth = 0)
	{
		if (depth > 16 || !record.is_object() || !record.contains("version") || record["version"] != 2)
			return false;
		const std::set<std::string> fields{"version", "id", "phase", "bootstrap", "source", "destination", "sourceIdentity", "destinationIdentity", "sourceDigest", "destinationDigest", "sourceObjects", "destinationObjects", "preferenceRevision", "priorSourceReceipt", "previous", "passiveSnapshots", "passiveDigest", "passiveObjects"};
		for (auto entry = record.begin(); entry != record.end(); ++entry)
			if (!fields.contains(entry.key()))
				return false;
		if (!record.contains("priorSourceReceipt") || !record.contains("previous") || !record.contains("passiveSnapshots"))
			return false;
		if (!record["priorSourceReceipt"].is_null())
		{
			const auto& receipt = record["priorSourceReceipt"];
			if (!receipt.is_object() || !receipt.contains("version") || receipt["version"] != 2)
				return false;
			for (const auto* field : {"id", "role", "phase", "identity", "sourceDigest", "destinationDigest", "bootstrap"})
				if (!receipt.contains(field) || !receipt[field].is_string())
					return false;
		}
		for (const auto* field : {"id", "phase", "bootstrap", "source", "destination", "sourceIdentity", "destinationIdentity", "sourceDigest", "destinationDigest", "sourceObjects", "destinationObjects", "preferenceRevision"})
		{
			if (!record.contains(field) || !record[field].is_string() || record[field].get_ref<const std::string&>().size() > 32768)
				return false;
		}
		const auto id = record["id"].get<std::string>();
		if (id.empty() || id.size() > 128 || id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-") != std::string::npos)
			return false;
		for (const auto* field : {"bootstrap", "source", "destination"})
		{
			const auto path = uam::paths::PathFromUtf8(record[field].get<std::string>());
			if (!path.is_absolute() || path == path.root_path() || path.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
				return false;
			for (const auto& component : path)
				if (component == "." || component == "..")
					return false;
		}
		if (record["source"].get_ref<const std::string&>() == record["destination"].get_ref<const std::string&>() || record["sourceIdentity"].get_ref<const std::string&>().empty() || record["preferenceRevision"].get_ref<const std::string&>().empty())
			return false;
		const auto phase = record["phase"].get<std::string>();
		const auto digest = [](const Json& value)
		{
			const auto& text = value.get_ref<const std::string&>();
			return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
		};
		for (const auto* field : {"sourceDigest", "destinationDigest", "sourceObjects", "destinationObjects"})
			if (!record[field].get_ref<const std::string&>().empty() && !digest(record[field]))
				return false;
		if ((phase == "staging" || phase == "verified" || phase == "starting" || phase == "active") && (record["destinationIdentity"].get_ref<const std::string&>().empty() || (!digest(record["sourceDigest"]) || !digest(record["sourceObjects"]))))
			return false;
		if ((phase == "verified" || phase == "starting" || phase == "active") && (!digest(record["destinationDigest"]) || !digest(record["destinationObjects"])))
			return false;
		if (record.contains("previous") && !record["previous"].is_null())
		{
			const auto& previous = record["previous"];
			if (!Valid(previous, depth + 1) || previous["phase"].get_ref<const std::string&>() != "active" || previous["bootstrap"].get_ref<const std::string&>() != record["bootstrap"].get_ref<const std::string&>() || previous["destination"].get_ref<const std::string&>() != record["source"].get_ref<const std::string&>() || previous["id"].get_ref<const std::string&>() == record["id"].get_ref<const std::string&>())
				return false;
		}
		if (phase == "rolledBack" && !record["sourceDigest"].get_ref<const std::string&>().empty() && (!record.contains("passiveDigest") || !record["passiveDigest"].is_string() || !digest(record["passiveDigest"]) || !record.contains("passiveObjects") || !record["passiveObjects"].is_string() || !digest(record["passiveObjects"])))
			return false;
		if (record.contains("passiveSnapshots"))
		{
			if (!record["passiveSnapshots"].is_array() || record["passiveSnapshots"].size() > 16)
				return false;
			for (const auto& snapshot : record["passiveSnapshots"])
			{
				if (!snapshot.is_object())
					return false;
				for (const auto* key : {"id", "path", "identity", "sourceDigest", "destinationDigest", "digest", "objects", "bootstrap"})
					if (!snapshot.contains(key) || !snapshot[key].is_string() || snapshot[key].get_ref<const std::string&>().size() > 32768)
						return false;
				const auto path = uam::paths::PathFromUtf8(snapshot["path"].get<std::string>());
				if (!path.is_absolute() || path == path.root_path() || snapshot["bootstrap"] != record["bootstrap"] || path.native().find(fs::path::value_type{}) != fs::path::string_type::npos || !digest(snapshot["digest"]) || !digest(snapshot["objects"]) || !digest(snapshot["sourceDigest"]))
					return false;
				for (const auto& component : path)
					if (component == "." || component == "..")
						return false;
			}
		}
		return std::set<std::string>{"armed", "staging", "verified", "starting", "active", "rolledBack"}.contains(record["phase"].get<std::string>());
	}
	struct PublicationWitnessPair
	{
		std::shared_ptr<const Tree::ReadWitness> primary, acknowledgement;
		bool Current() const
		{
			return primary && acknowledgement && primary->SameParent(*acknowledgement) && primary->Current() && acknowledgement->Current();
		}
	};
	bool ReadPublicationPair(const std::shared_ptr<Tree>& tree, const fs::path& primary_path, const fs::path& acknowledgement_path, std::string& primary_bytes, std::string& acknowledgement_bytes, PublicationWitnessPair& witness, std::string& error)
	{
		return tree && tree->Read(primary_path, kRecordLimit, primary_bytes, error, &witness.primary) && tree->Read(acknowledgement_path, kRecordLimit, acknowledgement_bytes, error, &witness.acknowledgement) && witness.Current();
	}
	bool AcknowledgedPrimary(const fs::path& control, const std::string& record_bytes, std::string* intent_bytes = nullptr, const Tree::ReadObserver& observer = {}, PublicationWitnessPair* retained = nullptr)
	{
		std::string primary, bytes, error;
		// Normalize the already-selected control path, including the final relative
		// default fallback, without introducing a different data-root search order.
		std::error_code path_error;
		const auto absolute_control = fs::absolute(control, path_error);
		if (path_error)
			return false;
		const auto tree = Tree::Open(absolute_control, error, {}, observer);
		PublicationWitnessPair witness;
		if (!ReadPublicationPair(tree, "transition.json", "transition-intent.json", primary, bytes, witness, error) || primary != record_bytes)
			return false;
		const auto intent = Json::parse(bytes, nullptr, false);
		if (!intent.is_object() || intent.size() != 4 || !intent.contains("version") || intent["version"] != 2 || !intent.contains("phase") || !intent["phase"].is_string() || intent["phase"].get_ref<const std::string&>() != "acknowledged" || !intent.contains("after") || !intent.contains("before") || intent["after"].dump() != record_bytes || (!intent["before"].is_null() && !Valid(intent["before"])) || !witness.Current())
			return false;
		if (intent_bytes)
			*intent_bytes = bytes;
		if (retained)
			*retained = std::move(witness);
		return true;
	}
	fs::path Path(const Json& json, const char* field)
	{
		return uam::paths::PathFromUtf8(json.at(field).get<std::string>());
	}
	bool ReceiptAcknowledged(const std::shared_ptr<Tree>& tree, const std::string& receipt_bytes, std::string& error, PublicationWitnessPair* retained = nullptr)
	{
		std::string primary, bytes;
		PublicationWitnessPair witness;
		if (!ReadPublicationPair(tree, kReceipt, kReceiptIntent, primary, bytes, witness, error) || primary != receipt_bytes)
			return false;
		const auto intent = Json::parse(bytes, nullptr, false);
		const bool confirmed = intent.is_object() && intent.size() == 4 && intent.contains("version") && intent["version"] == 2 && intent.contains("phase") && intent["phase"].is_string() && intent["phase"].get_ref<const std::string&>() == "acknowledged" && intent.contains("before") && intent.contains("after") && intent["after"].dump() == receipt_bytes && witness.Current();
		if (confirmed && retained)
			*retained = std::move(witness);
		return confirmed;
	}
	bool PlainReceipt(const std::shared_ptr<Tree>& tree, const Json& transaction, const char* role, Json& receipt, std::string& error, PublicationWitnessPair* retained = nullptr)
	{
		std::string bytes;
		PublicationWitnessPair witness;
		if (!tree || !tree->Read(kReceipt, kRecordLimit, bytes, error) || !ReceiptAcknowledged(tree, bytes, error, &witness))
			return false;
		receipt = Json::parse(bytes, nullptr, false);
		if (!receipt.is_object() || receipt.size() != 8 || !receipt.contains("version") || receipt["version"] != 2)
			return false;
		for (const auto* field : {"id", "role", "phase", "identity", "sourceDigest", "destinationDigest", "bootstrap"})
			if (!receipt.contains(field) || !receipt[field].is_string())
				return false;
		const bool confirmed = receipt.value("id", "") == transaction["id"].get_ref<const std::string&>() && receipt.value("role", "") == role && receipt.value("identity", "") == tree->Identity() && receipt.value("sourceDigest", "") == transaction["sourceDigest"].get_ref<const std::string&>() && receipt["bootstrap"] == transaction["bootstrap"] && witness.Current();
		if (confirmed && retained)
			*retained = std::move(witness);
		return confirmed;
	}
	bool ArmedSourceUnchanged(const std::shared_ptr<Tree>& tree, const Json& record, std::string& error)
	{
		if (!tree || !tree->Current())
			return false;
		const auto& prior = record["priorSourceReceipt"];
		if (prior.is_null())
			return !Exists(tree->RootPath() / ".uam-data-folder");
		std::string bytes;
		return tree->Read(kReceipt, kRecordLimit, bytes, error) && bytes == prior.dump() && ReceiptAcknowledged(tree, bytes, error);
	}
	bool RestoredSourceAcknowledged(const std::shared_ptr<Tree>& tree, const Json& record, std::string& error)
	{
		if (!tree || !tree->Current() || tree->Identity() != record["sourceIdentity"].get_ref<const std::string&>())
			return false;
		// Only a cancellation before any first transfer can leave the source receipt-free.
		if (record["sourceDigest"].get_ref<const std::string&>().empty() && record["priorSourceReceipt"].is_null() && record["previous"].is_null() && record["passiveSnapshots"].empty())
			return !Exists(tree->RootPath() / ".uam-data-folder");
		Json receipt;
		return PlainReceipt(tree, record, "source", receipt, error) && receipt["phase"] == "rolledBack" && receipt["destinationDigest"] == record["destinationDigest"];
	}
	bool PassiveSnapshot(const std::shared_ptr<Tree>& tree, const Json& snapshot, std::string& error)
	{
		Tree::Inventory entries;
		Json receipt;
		return tree && tree->Current() && tree->Identity() == snapshot["identity"].get_ref<const std::string&>() && tree->Scan(entries, error) && Tree::Digest(entries) == snapshot["digest"].get_ref<const std::string&>() && Tree::ObjectDigest(entries) == snapshot["objects"].get_ref<const std::string&>() && PlainReceipt(tree, snapshot, "destination", receipt, error) && receipt["phase"].get_ref<const std::string&>() == "rolledBack" && receipt["destinationDigest"].get_ref<const std::string&>() == snapshot["destinationDigest"].get_ref<const std::string&>();
	}
	Json PassiveDescriptor(const Json& record)
	{
		return {{"id", record["id"]}, {"path", record["destination"]}, {"identity", record["destinationIdentity"]}, {"sourceDigest", record["sourceDigest"]}, {"destinationDigest", record["destinationDigest"]}, {"digest", record["passiveDigest"]}, {"objects", record["passiveObjects"]}, {"bootstrap", record["bootstrap"]}};
	}
	Json ParseProfileRecord(const std::string& bytes)
	{
		// Owner parsers and JSON libraries can disagree on duplicate-key precedence.
		// Refuse the whole record rather than choosing different execution authority.
		bool unique = true;
		std::vector<std::set<std::string>> object_keys;
		auto record = Json::parse(bytes, [&](int, Json::parse_event_t event, Json& value)
		{
			if (event == Json::parse_event_t::object_start)
				object_keys.emplace_back();
			else if (event == Json::parse_event_t::object_end)
				object_keys.pop_back();
			else if (event == Json::parse_event_t::key && !object_keys.back().insert(value.get_ref<const std::string&>()).second)
				unique = false;
			return true;
		}, false);
		return unique ? std::move(record) : Json(Json::value_t::discarded);
	}
	bool HasSourceReference(const Json& value, const fs::path& source, const std::string& key = {}, unsigned depth = 0)
	{
		if (depth > 64)
			return true;
		if (value.is_string())
		{
			const auto path = uam::paths::PathFromUtf8(value.get<std::string>());
			// Retained authored prose and historical ownership witnesses remain bytes, not new runtime bindings.
			const bool archival = std::set<std::string>{"text", "content", "summary", "message", "description", "title", "name", "dataRoot", "data_root", "sourceDataRoot", "ownerDataRoot"}.contains(key);
			return !archival && ((path.is_absolute() && uam::paths::IsSameOrInsideRoot(source, path)) || uam::paths::EmbedsDataFolderReference(value.get_ref<const std::string&>(), source));
		}
		if (value.is_object())
			for (auto child = value.begin(); child != value.end(); ++child)
				if (HasSourceReference(child.value(), source, child.key(), depth + 1))
					return true;
		if (value.is_array())
			for (const auto& child : value)
				if (HasSourceReference(child, source, key, depth + 1))
					return true;
		return false;
	}
} // namespace

bool uam::DataFolderTransition::CheckExternalSourceFence(const fs::path& root, std::string& error, bool allow_authoritative_root, const Tree::ReadObserver& control_read_observer)
{
	if (!Exists(root / ".uam-data-folder"))
		return true;
	if (uam::paths::IsLinkOrReparsePointNoThrow(root / ".uam-data-folder"))
	{
		error = "The root handoff metadata is linked or unowned.";
		return false;
	}
	Json receipt;
	std::string bytes;
	const bool readable = ReadJson(root / kReceipt, receipt, bytes);
	bool typed = readable && receipt.size() == 8 && receipt.contains("version") && receipt["version"] == 2;
	for (const auto* field : {"id", "role", "phase", "identity", "sourceDigest", "destinationDigest", "bootstrap"})
		typed = typed && receipt.contains(field) && receipt[field].is_string();
	const auto tree = typed ? Tree::Open(fs::absolute(root), error) : nullptr;
	if (tree && allow_authoritative_root && ReceiptAcknowledged(tree, bytes, error))
	{
		// A root receipt is prepared before final transaction publication. Independent
		// writers must also observe the exact bootstrap acknowledgement, under their
		// shared root writer lease; readable active/rolledBack bytes alone grant nothing.
		const auto bootstrap = Path(receipt, "bootstrap");
		const auto control = bootstrap.is_absolute() ? Tree::Open(bootstrap / ".data-folder-control", error, {}, control_read_observer) : nullptr;
		std::string primary_bytes;
		if (control && control->Read("transition.json", kRecordLimit, primary_bytes, error))
		{
			const auto record = Json::parse(primary_bytes, nullptr, false);
			PublicationWitnessPair bootstrap_witness, root_witness;
			std::error_code ec;
			Json owned;
			const bool final = Valid(record) && AcknowledgedPrimary(control->RootPath(), primary_bytes, nullptr, control_read_observer, &bootstrap_witness);
			if (final && fs::equivalent(Path(record, "bootstrap"), bootstrap, ec) && !ec && ((receipt["role"] == "destination" && record["phase"] == "active" && fs::equivalent(Path(record, "destination"), root, ec)) || (receipt["role"] == "source" && record["phase"] == "rolledBack" && fs::equivalent(Path(record, "source"), root, ec))) && !ec && PlainReceipt(tree, record, receipt["role"].get_ref<const std::string&>().c_str(), owned, error, &root_witness) && tree->Identity() == record[receipt["role"] == "destination" ? "destinationIdentity" : "sourceIdentity"].get_ref<const std::string&>() && owned["phase"] == record["phase"] && owned["destinationDigest"] == record["destinationDigest"] && tree->Current() && control->Current() && bootstrap_witness.Current() && root_witness.Current())
				return true;
		}
	}
	error = "This folder has a retained or uncertain data-folder handoff. Open it through its original bootstrap location; an override cannot bypass ownership.";
	return false;
}

bool uam::DataFolderTransition::ProbeStartupRoot(const fs::path& default_root, const std::optional<fs::path>& external_override, fs::path& selected, std::string& error, const Tree::ReadObserver& control_read_observer)
{
	selected = external_override.value_or(default_root);
	if (external_override)
		return CheckExternalSourceFence(selected, error);
	const fs::path ledger = default_root / ".data-folder-control/transition.json";
	if (uam::paths::IsLinkOrReparsePointNoThrow(ledger.parent_path()))
	{
		error = "The bootstrap directory is linked or unowned. No profile was selected.";
		return false;
	}
	if (!Exists(ledger))
	{
		if (Exists(ledger.parent_path() / "transition-intent.json"))
		{
			error = "The handoff intent has no confirmed primary. Startup remains held.";
			return false;
		}
		return CheckExternalSourceFence(default_root, error);
	}
	Json record;
	std::string bytes;
	PublicationWitnessPair bootstrap_witness;
	std::error_code filesystem_error;
	if (!ReadJson(ledger, record, bytes) || !Valid(record) || !AcknowledgedPrimary(ledger.parent_path(), bytes, nullptr, control_read_observer, &bootstrap_witness) || !fs::equivalent(Path(record, "bootstrap"), default_root, filesystem_error) || filesystem_error)
	{
		error = "The data-folder handoff record is invalid. No fallback profile was selected.";
		return false;
	}
	if (record["phase"].get_ref<const std::string&>() == "starting")
	{
		error = "Destination startup ownership is unconfirmed. Both profiles and their receipts require recovery.";
		return false;
	}
	selected = record["phase"].get_ref<const std::string&>() == "active" ? Path(record, "destination") : Path(record, "source");
	if (!fs::is_directory(fs::symlink_status(selected, filesystem_error)) || filesystem_error || uam::paths::IsLinkOrReparsePointNoThrow(selected))
	{
		error = "The authoritative data-folder directory is missing or linked.";
		return false;
	}
	const auto tree = Tree::Open(fs::absolute(selected), error);
	if (!tree || tree->Identity() != record[record["phase"].get_ref<const std::string&>() == "active" ? "destinationIdentity" : "sourceIdentity"].get_ref<const std::string&>())
	{
		error = "The authoritative root directory was replaced. No fallback profile was selected.";
		return false;
	}
	if (record["phase"] == "armed" && !ArmedSourceUnchanged(tree, record, error))
	{
		error = "Source fencing began before staging was acknowledged. Startup remains held.";
		return false;
	}
	if (record["phase"] == "rolledBack" && !RestoredSourceAcknowledged(tree, record, error))
	{
		error = "The restored source receipt is missing, stale or uncertain. Startup remains held.";
		return false;
	}
	if (record["phase"].get_ref<const std::string&>() == "active")
	{
		Json receipt;
		if (!PlainReceipt(tree, record, "destination", receipt, error) || receipt["phase"].get_ref<const std::string&>() != "active")
		{
			error = "The active destination receipt is missing or uncertain.";
			return false;
		}
	}
	if (!bootstrap_witness.Current())
	{
		error = "The bootstrap primary or acknowledgement changed during startup admission.";
		return false;
	}
	return true;
}

std::shared_ptr<uam::DataFolderTransition> uam::DataFolderTransition::Open(const fs::path& control_root, const fs::path& current_root, std::shared_ptr<uam::platform::DataRootLock> current_lease, std::shared_ptr<uam::platform::DataRootLock> control_lease, const IPlatformProcessService& process, std::string& error, const platform::DataFolderTree::ReadObserver& source_read_observer, const platform::DataFolderTree::ReadObserver& control_read_observer)
{
	if (!current_lease || !current_lease->Current() || !control_lease || !control_lease->Current())
	{
		error = "Active profile and bootstrap leases are required.";
		return {};
	}
	auto transition = std::shared_ptr<DataFolderTransition>(new DataFolderTransition());
	transition->m_process = &process;
	transition->m_controlLease = std::move(control_lease);
	transition->m_control = Tree::Open(control_root, error);
	if (!transition->m_control)
		return {};
	transition->m_bootstrapIdentity = platform::RetainedDirectoryIdentity::Capture(transition->m_control->RootPath().parent_path());
	if (!transition->m_bootstrapIdentity)
	{
		error = "The default bootstrap directory identity cannot be retained.";
		return {};
	}
	if (!Exists(control_root / "transition.json"))
	{
		if (Exists(control_root / "transition-intent.json"))
		{
			error = "A handoff intent without a primary is unconfirmed.";
			return {};
		}
		std::error_code path_error;
		const auto resolved = fs::canonical(current_root, path_error);
		if (path_error)
		{
			error = "Cannot resolve the exclusively owned source root.";
			return {};
		}
		transition->m_source = Tree::Open(resolved, error, {}, source_read_observer);
		transition->m_sourceLease = std::move(current_lease);
		return transition->m_source ? transition : std::shared_ptr<DataFolderTransition>{};
	}
	if (!ReadJson(control_root / "transition.json", transition->m_record, transition->m_recordBytes) || !Valid(transition->m_record))
	{
		error = "Cannot load the exact handoff primary.";
		return {};
	}
	PublicationWitnessPair bootstrap_witness;
	const bool acknowledged = AcknowledgedPrimary(control_root, transition->m_recordBytes, &transition->m_intentBytes, control_read_observer, &bootstrap_witness);
	const bool active = transition->m_record["phase"].get_ref<const std::string&>() == "active";
	if (active)
		transition->m_destinationLease = current_lease;
	else
		transition->m_sourceLease = current_lease;
	transition->m_destinationOwner = active;
	if (!transition->LoadRetainedHistory(error))
	{
		transition->m_held = true;
		return transition;
	}
	std::error_code filesystem_error;
	if (!fs::equivalent(current_root, Path(transition->m_record, active ? "destination" : "source"), filesystem_error) || filesystem_error)
	{
		transition->Hold(error, "The handoff changed after startup routing.");
		return transition;
	}
	transition->m_source = Tree::Open(Path(transition->m_record, "source"), error, {}, source_read_observer);
	if (!transition->m_source)
	{
		transition->Hold(error, "The retained source directory is unavailable.");
		return transition;
	}
	if (active)
	{
		transition->m_destinationLease = std::move(current_lease);
		transition->m_sourceLease = process.TryAcquireDataRootLock(Path(transition->m_record, "source"), &error);
		transition->m_sourceWriters = Writers::TryAcquire(Path(transition->m_record, "source"), Writers::Access::Exclusive, error);
		transition->m_destination = Tree::Open(Path(transition->m_record, "destination"), error);
		transition->m_destinationOwner = true;
		if (!transition->m_sourceLease || !transition->m_sourceWriters || !transition->m_destination || !transition->ValidateRetained(error))
		{
			transition->m_held = true;
			return transition;
		}
	}
	else
	{
		transition->m_sourceLease = std::move(current_lease);
		if (transition->m_record["phase"] == "armed" && !ArmedSourceUnchanged(transition->m_source, transition->m_record, error))
		{
			transition->Hold(error, "Source fencing or cancellation is unconfirmed; the armed record cannot authorize profile writers.");
			return transition;
		}
		if (transition->m_record["phase"] == "rolledBack" && !RestoredSourceAcknowledged(transition->m_source, transition->m_record, error))
		{
			transition->Hold(error, "The restored source has no exact acknowledged receipt for this transaction.");
			return transition;
		}
		if (transition->m_record["phase"].get_ref<const std::string&>() != "armed" && (transition->m_record["phase"].get_ref<const std::string&>() != "rolledBack" || !transition->m_record["sourceDigest"].get_ref<const std::string&>().empty()))
		{
			transition->m_destination = Tree::Open(Path(transition->m_record, "destination"), error);
			if (!transition->m_destination || transition->m_destination->Identity() != transition->m_record["destinationIdentity"].get_ref<const std::string&>())
			{
				transition->Hold(error, "The passive destination directory is missing or replaced.");
				return transition;
			}
			transition->m_destinationLease = process.TryAcquireDataRootLock(Path(transition->m_record, "destination"), &error);
			transition->m_destinationWriters = Writers::TryAcquire(Path(transition->m_record, "destination"), Writers::Access::Exclusive, error);
			if (transition->m_record["phase"].get_ref<const std::string&>() != "rolledBack")
				transition->m_sourceWriters = Writers::TryAcquire(Path(transition->m_record, "source"), Writers::Access::Exclusive, error);
			transition->m_held = true;
			if (!transition->m_destinationLease || !transition->m_destinationWriters || !transition->m_destination || (transition->m_record["phase"].get_ref<const std::string&>() != "rolledBack" && !transition->m_sourceWriters))
				return transition;
			if (transition->m_record["phase"].get_ref<const std::string&>() == "rolledBack" && PassiveSnapshot(transition->m_destination, PassiveDescriptor(transition->m_record), error))
				transition->m_held = false;
		}
	}
	if (!acknowledged || !bootstrap_witness.Current())
		transition->Hold(error, "The handoff primary has no exact durable acknowledgement.");
	return transition;
}

bool uam::DataFolderTransition::Hold(std::string& error, const std::string& reason)
{
	m_held = true;
	error = reason;
	return false;
}
bool uam::DataFolderTransition::OwnsRoots() const
{
	if (!m_controlLease || !m_controlLease->Current() || !m_sourceLease || !m_sourceLease->Current() || (m_destinationLease && !m_destinationLease->Current()))
		return false;
	for (const auto& retained : m_retained)
		if (!retained.lease || !retained.lease->Current())
			return false;
	return true;
}
bool uam::DataFolderTransition::Save(std::string_view phase, std::string& error, const Fault& fault)
{
	std::string observed;
	if (!OwnsRoots() || (m_sourceWriters && !m_sourceWriters->Current()) || (m_destinationWriters && !m_destinationWriters->Current()))
		return Hold(error, "The profile writer freeze changed before publication.");
	if ((!m_recordBytes.empty() && (!m_control->Read("transition.json", kRecordLimit, observed, error) || observed != m_recordBytes)) || (m_recordBytes.empty() && Exists(m_control->RootPath() / "transition.json")))
		return Hold(error, "The transition primary changed outside its owner.");
	std::string prior_intent;
	if ((m_intentBytes.empty() && Exists(m_control->RootPath() / "transition-intent.json")) || (!m_intentBytes.empty() && (!m_control->Read("transition-intent.json", kRecordLimit, prior_intent, error) || prior_intent != m_intentBytes)))
		return Hold(error, "The handoff intent changed outside its owner.");
	m_record["phase"] = phase;
	if (!Valid(m_record))
		return Hold(error, "The candidate handoff schema is invalid; no primary was published.");
	const std::string candidate = m_record.dump();
	Json intent{{"version", 2}, {"before", m_recordBytes.empty() ? Json{} : Json::parse(m_recordBytes)}, {"after", m_record}, {"phase", "publishing"}};
	const auto publishing = intent.dump();
	if (candidate.size() > kRecordLimit || publishing.size() > kRecordLimit)
		return Hold(error, "The retained ownership chain exceeds the bounded handoff record limit.");
	const auto owned = [this](io::AtomicWriteStage) { return OwnsRoots(); };
	if (!m_control->Publish("transition-intent.json", publishing, owned).confirmed)
		return Hold(error, "The handoff write-ahead intent is unconfirmed.");
	m_intentBytes = publishing;
	const auto result = m_control->Publish("transition.json", candidate, [&](io::AtomicWriteStage stage) { return (!fault || fault(phase, stage)) && OwnsRoots(); });
	if (!result.confirmed)
		return Hold(error, "The handoff primary publication is unconfirmed; ownership and receipts remain held.");
	m_recordBytes = candidate;
	const auto archive = m_control->Publish(fs::path("transactions") / (m_record["id"].get<std::string>() + "." + std::string(phase) + ".json"), candidate, owned);
	if (!archive.confirmed)
		return Hold(error, "The handoff disposition archive is unconfirmed; ownership remains held.");
	intent["phase"] = "acknowledged";
	const auto acknowledged = intent.dump();
	if (!m_control->Publish("transition-intent.json", acknowledged, owned).confirmed)
		return Hold(error, "The exact handoff acknowledgement is unconfirmed.");
	m_intentBytes = acknowledged;
	return true;
}
bool uam::DataFolderTransition::Receipt(const std::shared_ptr<Tree>& tree, std::string_view role, std::string_view phase, std::string& error, const Fault& fault)
{
	if (!OwnsRoots() || !tree || !tree->Current())
		return Hold(error, "The receipt root identity changed.");
	Json previous;
	if (Exists(tree->RootPath() / kReceipt))
	{
		std::string bytes;
		if (!tree->Read(kReceipt, kRecordLimit, bytes, error) || !ReceiptAcknowledged(tree, bytes, error))
			return Hold(error, "The existing root receipt cannot be owned safely.");
		previous = Json::parse(bytes, nullptr, false);
		if (!(m_record.contains("priorSourceReceipt") && role == "source" && previous == m_record["priorSourceReceipt"]) && (!PlainReceipt(tree, m_record, std::string(role).c_str(), previous, error)))
			return Hold(error, "A foreign or replaced root receipt cannot be overwritten.");
	}
	else if (Exists(tree->RootPath() / kReceiptIntent))
		return Hold(error, "An unmatched root receipt intent remains unconfirmed.");
	const Json receipt{{"version", 2}, {"id", m_record["id"]}, {"role", role}, {"phase", phase}, {"identity", tree->Identity()}, {"sourceDigest", m_record["sourceDigest"]}, {"destinationDigest", m_record["destinationDigest"]}, {"bootstrap", m_record["bootstrap"]}};
	Json intent{{"version", 2}, {"before", previous}, {"after", receipt}, {"phase", "publishing"}};
	const auto owned = [this](io::AtomicWriteStage) { return OwnsRoots(); };
	if (!tree->Publish(kReceiptIntent, intent.dump(), owned).confirmed)
		return Hold(error, "The root receipt intent is unconfirmed; ownership remains held.");
	const auto result = tree->Publish(kReceipt, receipt.dump(), [&](io::AtomicWriteStage stage) { return (!fault || fault(role, stage)) && OwnsRoots(); });
	if (!result.confirmed)
		return Hold(error, "A root handoff receipt is unconfirmed; both acquired root leases remain held.");
	intent["phase"] = "acknowledged";
	return tree->Publish(kReceiptIntent, intent.dump(), owned).confirmed || Hold(error, "The root receipt acknowledgement is unconfirmed; ownership remains held.");
}

uam::DataFolderPreflightResult uam::DataFolderTransition::InspectDestination(const fs::path& destination) const
{
	return InspectDestination(destination, DataFolderDestinationCheck::RequireEmpty);
}

uam::DataFolderPreflightResult uam::DataFolderTransition::InspectDestination(const fs::path& destination, DataFolderDestinationCheck destination_check) const
{
	const auto identities_current = [&]()
	{
		if (!OwnsRoots() || !m_bootstrapIdentity || !m_bootstrapIdentity->Current() || !m_control || !m_control->Current() || !m_source || !m_source->Current() || (m_destination && !m_destination->Current()))
			return false;
		for (const auto& retained : m_retained)
			if (!retained.tree || !retained.tree->Current())
				return false;
		return true;
	};
	const auto unavailable = []()
	{
		DataFolderPreflightResult result;
		result.blocker = DataFolderPreflightBlocker::InspectionFailed;
		result.explanation = "The bootstrap or a retained data-folder identity changed. Folder selection remains held for recovery.";
		return result;
	};
	if (!identities_current())
		return unavailable();
	std::vector<fs::path> retained_roots{m_control->RootPath().parent_path(), m_source->RootPath()};
	for (const auto& retained : m_retained)
		retained_roots.push_back(retained.tree->RootPath());
	// The current rolled-back image has not yet been promoted into m_retained by Arm.
	// An ordinary armed destination is still a candidate, not a protected passive image.
	if (m_destination && (m_destinationOwner || (!m_record.is_null() && m_record["phase"] == "rolledBack" && !m_record["sourceDigest"].get_ref<const std::string&>().empty())))
		retained_roots.push_back(m_destination->RootPath());
	const auto result = InspectDataFolderTransition(CurrentRoot(), destination, false, retained_roots, destination_check);
	return identities_current() ? result : unavailable();
}

bool uam::DataFolderTransition::Arm(const fs::path& destination, std::string_view preference_revision, std::string& error, const Fault& fault)
{
	// Revalidate every call, including a retry of an already armed choice, before
	// preference publication or any promotion of a frozen root into the history chain.
	const auto preflight = InspectDestination(destination);
	if (preflight.state != DataFolderPreflightState::RequiresStoppedValidation)
	{
		if (preflight.blocker == DataFolderPreflightBlocker::InspectionFailed)
			return Hold(error, preflight.explanation);
		error = preflight.explanation;
		return false;
	}
	if (Armed() && !m_held)
	{
		std::error_code comparison_error;
		const auto target = Tree::Open(destination, error);
		if (!target || !fs::equivalent(destination, Path(m_record, "destination"), comparison_error) || comparison_error || target->Identity() != m_record["destinationIdentity"].get_ref<const std::string&>() || !m_source->Current())
			return Hold(error, "The armed source or destination identity changed.");
		m_record["preferenceRevision"] = preference_revision;
		return Save("armed", error, fault);
	}
	if (m_held || (!m_record.is_null() && m_record["phase"].get_ref<const std::string&>() != "rolledBack" && m_record["phase"].get_ref<const std::string&>() != "active"))
		return Hold(error, "An existing or uncertain handoff must be resolved before another transfer.");
	if (!m_record.is_null() && m_record["phase"] == "rolledBack" && !RestoredSourceAcknowledged(m_source, m_record, error))
		return Hold(error, "The restored source receipt no longer confirms this transaction.");
	if (m_destinationOwner && !ValidateRetained(error))
		return false;
	const auto source = m_destinationOwner ? m_destination : m_source;
	const auto selected_destination = Tree::Open(preflight.destination_root, error);
	if (!selected_destination)
		return Hold(error, "Cannot retain the chosen destination identity.");
	Json prior_source_receipt;
	if (Exists(source->RootPath() / ".uam-data-folder"))
	{
		std::string bytes;
		if (!source->Read(kReceipt, kRecordLimit, bytes, error))
			return Hold(error, "The source already contains unowned handoff metadata.");
		prior_source_receipt = Json::parse(bytes, nullptr, false);
		Json owned;
		const bool prior_active = m_destinationOwner && PlainReceipt(source, m_record, "destination", owned, error) && owned["phase"].get_ref<const std::string&>() == "active";
		const bool prior_rollback = !m_record.is_null() && m_record["phase"] == "rolledBack" && RestoredSourceAcknowledged(source, m_record, error);
		if (!prior_active && !prior_rollback)
			return Hold(error, "The source handoff metadata is not owned by the exact prior transaction.");
	}
	Json passive = m_record.is_null() ? Json::array() : m_record.value("passiveSnapshots", Json::array());
	if (!m_record.is_null() && m_record["phase"].get_ref<const std::string&>() == "rolledBack" && !m_record["sourceDigest"].get_ref<const std::string&>().empty())
	{
		const auto snapshot = PassiveDescriptor(m_record);
		if (!m_destinationLease || !m_destinationLease->Current() || !PassiveSnapshot(m_destination, snapshot, error))
			return Hold(error, "A rolled-back passive image no longer has confirmed ownership.");
		passive.push_back(snapshot);
		m_retained.push_back({m_destination, m_destinationLease, snapshot, true, m_destinationWriters});
		m_destinationWriters.reset();
		m_destination.reset();
		m_destinationLease.reset();
	}
	Json previous = m_destinationOwner ? m_record : (m_record.is_null() ? Json{} : m_record.value("previous", Json{}));
	if (m_destinationOwner)
	{
		m_retained.push_back({m_source, m_sourceLease, m_record, false, m_sourceWriters});
		m_sourceWriters.reset();
		m_source = m_destination;
		m_sourceLease = m_destinationLease;
		m_destination.reset();
		m_destinationLease.reset();
		m_destinationOwner = false;
	}
	m_record = {{"version", 2}, {"id", m_process->GenerateUuid()}, {"phase", "armed"}, {"bootstrap", paths::Utf8PathString(m_control->RootPath().parent_path())}, {"source", paths::Utf8PathString(preflight.source_root)}, {"destination", paths::Utf8PathString(preflight.destination_root)}, {"sourceIdentity", m_source->Identity()}, {"destinationIdentity", selected_destination->Identity()}, {"sourceDigest", ""}, {"destinationDigest", ""}, {"sourceObjects", ""}, {"destinationObjects", ""}, {"preferenceRevision", preference_revision}, {"priorSourceReceipt", prior_source_receipt}, {"previous", previous}, {"passiveSnapshots", passive}};
	if (!Valid(m_record))
		return Hold(error, "Could not establish a bounded handoff identity or retained ownership chain.");
	if (Exists(m_control->RootPath() / "transactions" / (m_record["id"].get<std::string>() + ".armed.json")))
		return Hold(error, "The generated transition identity already has a retained disposition.");
	return Save("armed", error, fault);
}

bool uam::DataFolderTransition::ActivateStopped(bool confirmed_normal_exit, std::string& error, const Fault& fault)
{
	if (!Armed() || !confirmed_normal_exit || m_held || !m_sourceLease || !m_sourceLease->Current() || !m_source->Current())
	{
		error = "A confirmed normal-exit restart and the original source owner are required.";
		return false;
	}
	// Check before opening a destination lock: even its marker would mutate a
	// historical inventory if the chosen directory moved under a protected root.
	const auto preflight = InspectDestination(Path(m_record, "destination"));
	if (preflight.state != DataFolderPreflightState::RequiresStoppedValidation)
		return Hold(error, preflight.explanation);
	if (!ValidateHistory(error))
		return false;
	if (Exists(m_source->RootPath() / ".data-folder-control"))
	{
		std::error_code identity_error;
		if (!fs::equivalent(m_source->RootPath() / ".data-folder-control", m_control->RootPath(), identity_error) || identity_error)
			return Hold(error, "A source contains an unrelated bootstrap directory; it cannot be silently excluded.");
	}
	// The retained bootstrap is excluded from profile copying. Do not silently exclude
	// unrelated user files or an interrupted publication hidden in that reserved folder.
	std::error_code control_error;
	std::size_t control_entries = 0;
	for (fs::directory_iterator cursor(m_control->RootPath(), control_error), end; !control_error && cursor != end; cursor.increment(control_error))
	{
		const auto name = cursor->path().filename();
		const auto status = cursor->symlink_status(control_error);
		if (control_error || ++control_entries > 256 || uam::paths::IsLinkOrReparsePointNoThrow(cursor->path()))
			return Hold(error, "Bootstrap inventory is linked, unbounded or unavailable.");
		if (name == "transactions" && fs::is_directory(status))
		{
			for (fs::directory_iterator archives(cursor->path(), control_error), archive_end; !control_error && archives != archive_end; archives.increment(control_error))
			{
				Json archive;
				std::string bytes;
				if (++control_entries > 256 || !ReadJson(archives->path(), archive, bytes) || !Valid(archive) || archives->path().filename() != archive["id"].get<std::string>() + "." + archive["phase"].get<std::string>() + ".json")
					return Hold(error, "A bootstrap archive is foreign or unconfirmed; it remains retained.");
			}
			continue;
		}
		if (!fs::is_regular_file(status) || (name != ".uam-data-root.lock" && name != "selection.json" && name != "intent.json" && name != "transition.json" && name != "transition-intent.json"))
			return Hold(error, "The retained bootstrap contains foreign or interrupted data; it cannot be silently excluded.");
	}
	if (control_error)
		return Hold(error, "Bootstrap inventory could not be confirmed.");
	m_destination = Tree::Open(preflight.destination_root, error);
	if (!m_destination || m_source->Identity() != m_record["sourceIdentity"].get_ref<const std::string&>() || m_destination->Identity() != m_record["destinationIdentity"].get_ref<const std::string&>())
		return Hold(error, "A root directory was replaced before transfer.");
	if (const auto rechecked = InspectDestination(preflight.destination_root); rechecked.state != DataFolderPreflightState::RequiresStoppedValidation)
		return Hold(error, rechecked.explanation);
	// Never wait while owning the source. A foreign destination owner leaves source authority intact.
	if (fault && !fault("beforeDestinationLock", io::AtomicWriteStage::BeforeReplace))
		return Hold(error, "Destination ownership was not attempted.");
	m_destinationLease = m_process->TryAcquireDataRootLock(preflight.destination_root, &error);
	if (!m_destinationLease)
		return Hold(error, "Destination ownership is unavailable; no transfer was started.");
	if (const auto rechecked = InspectDestination(preflight.destination_root, DataFolderDestinationCheck::SeparationOnly); rechecked.state != DataFolderPreflightState::RequiresStoppedValidation)
		return Hold(error, rechecked.explanation);
	std::error_code cwd_error;
	const auto current_directory = fs::current_path(cwd_error);
	if (cwd_error || uam::paths::IsSameOrInsideRoot(preflight.source_root, current_directory))
		return Hold(error, "Relaunch UAM from outside the current data folder before transferring it; inherited workspace context must not bind to the retained source.");
	m_sourceWriters = Writers::TryAcquire(preflight.source_root, Writers::Access::Exclusive, error);
	m_destinationWriters = Writers::TryAcquire(preflight.destination_root, Writers::Access::Exclusive, error);
	if (!m_sourceWriters || !m_destinationWriters)
		return Hold(error, "An independent profile writer is still active; no data was copied.");
	Tree::Inventory source, destination;
	if (!m_source->Scan(source, error) || !m_destination->Scan(destination, error) || !destination.empty())
		return Hold(error, "The destination is not an empty owned root or the source is unsafe.");
	std::uint64_t total = 0;
	std::map<fs::path, std::string> rewritten;
	for (const auto& entry : source)
	{
		if (entry.relative.begin() != entry.relative.end() && *entry.relative.begin() == "worktrees" && !entry.directory)
			return Hold(error, "Managed worktree relocation requires its owning service; this profile was not copied.");
		if (entry.directory)
			continue;
		if (entry.bytes > std::numeric_limits<std::uint64_t>::max() - total)
			return Hold(error, "The inventory byte count overflowed.");
		total += entry.bytes;
		if (entry.relative == "settings.txt" || entry.relative == "settings.txt.bak" || entry.relative == "folders.txt" || entry.relative == "folders.txt.bak" || entry.relative.extension() == ".json" || entry.relative.filename().string().ends_with(".json.bak"))
		{
			std::string bytes;
			if (!m_source->Read(entry.relative, 32 * 1024 * 1024, bytes, error))
				return Hold(error, "A structured record exceeds bounded reference validation.");
			if (entry.relative == "settings.txt" || entry.relative == "settings.txt.bak")
			{
				std::string mapped;
				if (!SettingsStore::RelocateDataFolderReferences(bytes, preflight.source_root, preflight.destination_root, mapped, error))
					return Hold(error, error);
				if (mapped != bytes)
					rewritten[entry.relative] = std::move(mapped);
			}
			else if (entry.relative == "folders.txt" || entry.relative == "folders.txt.bak")
			{
				if (!ChatFolderStore::ValidateDataFolderReferences(bytes, preflight.source_root, error))
					return Hold(error, error);
			}
			else
			{
				const auto record = ParseProfileRecord(bytes);
				const auto first = *entry.relative.begin();
				if (record.is_discarded())
					return Hold(error, "A structured profile record is invalid or has ambiguous duplicate JSON keys.");
				if ((first == "chats" || first == "chat-summaries" || first == "queue-outboxes") && !uam::chat_storage::StoppedFolderTransferAdmissible(record, error))
					return Hold(error, error);
				if (HasSourceReference(record, preflight.source_root))
					return Hold(error, "A structured resource or history reference cannot safely be relocated by its current owner.");
			}
		}
	}
	std::error_code space_error;
	if (fs::space(preflight.destination_root, space_error).available < total || space_error)
		return Hold(error, "The destination has insufficient or unknown free space.");
	m_record["sourceDigest"] = Tree::Digest(source);
	m_record["sourceObjects"] = Tree::ObjectDigest(source);
	// Fence both names durably before a staging publication can become uncertain.
	// Independent writers must discover the hold even after this owner exits.
	if (!Receipt(m_source, "source", "fenced", error, fault) || !Receipt(m_destination, "destination", "passive", error, fault) || !Save("staging", error, fault))
		return false;
	for (const auto& entry : source)
	{
		if (fault && !fault("copy", io::AtomicWriteStage::BeforeReplace))
			return Hold(error, "The passive copy was interrupted; source and destination remain held.");
		if (entry.directory ? !m_destination->MakeDirectory(entry.relative, error) : !m_source->CopyFile(entry, *m_destination, error))
			return Hold(error, error);
	}
	Tree::Inventory source_after, copied;
	if (!m_source->Scan(source_after, error) || Tree::Digest(source_after) != m_record["sourceDigest"].get_ref<const std::string&>() || Tree::ObjectDigest(source_after) != m_record["sourceObjects"].get_ref<const std::string&>() || !m_destination->Scan(copied, error) || Tree::Digest(copied) != m_record["sourceDigest"].get_ref<const std::string&>())
		return Hold(error, "The stopped source or passive destination changed during verification.");
	for (const auto& [relative, bytes] : rewritten)
	{
		if (!m_destination->Publish(relative, bytes).confirmed)
			return Hold(error, "The owning settings relocation publication is unconfirmed.");
	}
	if (!m_destination->Scan(copied, error))
		return Hold(error, error);
	m_record["destinationDigest"] = Tree::Digest(copied);
	m_record["destinationObjects"] = Tree::ObjectDigest(copied);
	if (!Save("verified", error, fault) || !Receipt(m_source, "source", "prepared", error, fault) || !Receipt(m_destination, "destination", "prepared", error, fault))
		return false;
	if (fault && !fault("beforeActivation", io::AtomicWriteStage::BeforeReplace))
		return Hold(error, "Verified destination remains passive; activation was not attempted.");
	if (!ValidateRetained(error) || !Save("starting", error, fault))
		return false;
	// Runtime ownership is not acknowledged until the caller adopts this lease/root and confirms it.
	return true;
}

bool uam::DataFolderTransition::LoadRetainedHistory(std::string& error)
{
	Json previous = m_record.value("previous", Json{});
	std::set<fs::path> roots{Path(m_record, "source"), Path(m_record, "destination")};
	while (!previous.is_null())
	{
		const auto root = Path(previous, "source");
		if (!roots.insert(root).second)
			return Hold(error, "A retained ownership chain repeats a root.");
		auto tree = Tree::Open(root, error);
		if (!tree || tree->Identity() != previous["sourceIdentity"].get_ref<const std::string&>())
			return Hold(error, "A retained source directory is missing or replaced.");
		auto lease = m_process->TryAcquireDataRootLock(root, &error);
		m_retained.push_back({tree, std::move(lease), previous, false, Writers::TryAcquire(root, Writers::Access::Exclusive, error)});
		if (!tree || !m_retained.back().lease || !m_retained.back().writers)
			return Hold(error, "A retained source owner is unavailable.");
		previous = previous.value("previous", Json{});
	}
	for (const auto& snapshot : m_record.value("passiveSnapshots", Json::array()))
	{
		const auto root = Path(snapshot, "path");
		if (!roots.insert(root).second)
			return Hold(error, "A retained passive image repeats another owned root.");
		auto tree = Tree::Open(root, error);
		if (!tree || tree->Identity() != snapshot["identity"].get_ref<const std::string&>())
			return Hold(error, "A retained passive directory is missing or replaced.");
		auto lease = m_process->TryAcquireDataRootLock(root, &error);
		m_retained.push_back({tree, std::move(lease), snapshot, true, Writers::TryAcquire(root, Writers::Access::Exclusive, error)});
		if (!tree || !m_retained.back().lease || !m_retained.back().writers)
			return Hold(error, "A retained passive image is owned elsewhere or unavailable.");
	}
	return ValidateHistory(error);
}
bool uam::DataFolderTransition::ValidateHistory(std::string& error)
{
	for (const auto& retained : m_retained)
	{
		if (!retained.writers || !retained.writers->Current())
			return Hold(error, "A historical profile writer freeze changed.");
		if (retained.passive)
		{
			if (!retained.lease || !retained.lease->Current() || !PassiveSnapshot(retained.tree, retained.record, error))
				return Hold(error, "A retained passive copy or rollback receipt changed.");
			continue;
		}
		Tree::Inventory inventory;
		Json receipt;
		if (!retained.lease || !retained.lease->Current() || !retained.tree || !retained.tree->Current() || retained.tree->Identity() != retained.record["sourceIdentity"].get_ref<const std::string&>() || !retained.tree->Scan(inventory, error) || Tree::Digest(inventory) != retained.record["sourceDigest"].get_ref<const std::string&>() || Tree::ObjectDigest(inventory) != retained.record["sourceObjects"].get_ref<const std::string&>() || !PlainReceipt(retained.tree, retained.record, "source", receipt, error) || receipt["phase"].get_ref<const std::string&>() != "active")
			return Hold(error, "A historical source snapshot or ownership receipt changed; no new profile was opened.");
	}
	return true;
}
bool uam::DataFolderTransition::ValidateRetained(std::string& error)
{
	Tree::Inventory inventory, destination_inventory;
	Json source, destination;
	if (!ValidateHistory(error))
		return false;
	if (!m_sourceWriters || !m_sourceWriters->Current() || (m_record["phase"].get_ref<const std::string&>() != "active" && (!m_destinationWriters || !m_destinationWriters->Current())) || !m_sourceLease || !m_sourceLease->Current() || !m_destinationLease || !m_destinationLease->Current() || !m_source || !m_destination || !m_source->Current() || !m_destination->Current() || m_source->Identity() != m_record["sourceIdentity"].get_ref<const std::string&>() || m_destination->Identity() != m_record["destinationIdentity"].get_ref<const std::string&>() || !m_source->Scan(inventory, error) || Tree::Digest(inventory) != m_record["sourceDigest"].get_ref<const std::string&>() || Tree::ObjectDigest(inventory) != m_record["sourceObjects"].get_ref<const std::string&>() || !PlainReceipt(m_source, m_record, "source", source, error) || !PlainReceipt(m_destination, m_record, "destination", destination, error) || source.value("phase", "") != (m_record["phase"].get_ref<const std::string&>() == "active" ? "active" : "prepared") || destination.value("phase", "") != (m_record["phase"].get_ref<const std::string&>() == "active" ? "active" : "prepared") || source.value("destinationDigest", "") != m_record["destinationDigest"].get_ref<const std::string&>() || destination.value("destinationDigest", "") != m_record["destinationDigest"].get_ref<const std::string&>() || (m_record["phase"].get_ref<const std::string&>() != "active" && (!m_destination->Scan(destination_inventory, error) || Tree::Digest(destination_inventory) != m_record["destinationDigest"].get_ref<const std::string&>() || Tree::ObjectDigest(destination_inventory) != m_record["destinationObjects"].get_ref<const std::string&>())))
		return Hold(error, "Source authority, root identities or matching handoff receipts changed. Neither image was merged or replaced.");
	return true;
}
bool uam::DataFolderTransition::ConfirmDestinationOwner(std::string& error, const Fault& fault)
{
	if (m_held || m_record.is_null() || m_record["phase"].get_ref<const std::string&>() != "starting" || !ValidateRetained(error))
		return false;
	if (!Receipt(m_source, "source", "active", error, fault) || !Receipt(m_destination, "destination", "active", error, fault) || !Save("active", error, fault))
		return false;
	m_destinationOwner = true;
	m_destinationWriters.reset(); // Current-profile writers may start only after the active acknowledgement.
	return true;
}
bool uam::DataFolderTransition::RollbackBeforeActivation(std::string& error, const Fault& fault)
{
	if (m_record.is_null())
		return true;
	if (m_record.dump() != m_recordBytes || !AcknowledgedPrimary(m_control->RootPath(), m_recordBytes))
		return Hold(error, "Unconfirmed transition publication cannot authorize rollback.");
	if (!m_sourceLease || !m_sourceLease->Current() || !m_source || !m_source->Current() || m_source->Identity() != m_record["sourceIdentity"].get_ref<const std::string&>() || !ValidateHistory(error))
		return Hold(error, "The original source ownership cannot be confirmed for rollback.");
	if (m_destinationOwner || m_record["phase"].get_ref<const std::string&>() == "starting" || m_record["phase"].get_ref<const std::string&>() == "active")
		return Hold(error, "Destination ownership may have begun. Automatic rollback would discard newer work; a reverse transaction is required.");
	if (m_record["phase"].get_ref<const std::string&>() == "rolledBack")
		return (!m_held && RestoredSourceAcknowledged(m_source, m_record, error)) || Hold(error, "The restored source receipt no longer confirms completed rollback.");
	if (m_record["phase"] == "armed")
	{
		if (!ArmedSourceUnchanged(m_source, m_record, error))
			return Hold(error, "Source fencing may have begun. Armed cancellation cannot infer passivity.");
		// A follow-up retains an older active/restored receipt. Publish the exact
		// cancellation identity before final acknowledgement, without touching any
		// historical/passive receipt. First-ever cancellation keeps a plain root plain.
		if (!m_record["priorSourceReceipt"].is_null() && !Receipt(m_source, "source", "rolledBack", error, fault))
			return false;
	}
	else
	{
		if (!m_sourceWriters || !m_sourceWriters->Current() || !m_destinationWriters || !m_destinationWriters->Current())
			return Hold(error, "Both stopped writer leases are required for passive rollback.");
		Tree::Inventory source;
		if (!m_source->Scan(source, error) || (Tree::Digest(source) != m_record["sourceDigest"].get_ref<const std::string&>() || Tree::ObjectDigest(source) != m_record["sourceObjects"].get_ref<const std::string&>()))
			return Hold(error, "The stopped source changed. Rollback cannot validate its authority.");
		Json receipt;
		if (!m_destinationLease || !m_destinationLease->Current() || !PlainReceipt(m_destination, m_record, "destination", receipt, error) || (receipt.value("phase", "") != "passive" && receipt.value("phase", "") != "prepared"))
			return Hold(error, "Destination passivity or ownership is unconfirmed. Rollback remains held.");
		Tree::Inventory passive;
		if (!m_destination->Scan(passive, error))
			return Hold(error, "The passive image cannot be retained safely for rollback.");
		m_record["passiveDigest"] = Tree::Digest(passive);
		m_record["passiveObjects"] = Tree::ObjectDigest(passive);
		if (!Receipt(m_source, "source", "rolledBack", error, fault) || !Receipt(m_destination, "destination", "rolledBack", error, fault))
			return false;
	}
	if (!Save("rolledBack", error, fault))
		return false;
	m_held = false;
	m_sourceWriters.reset(); // Source is mutable again; the passive destination remains exclusively frozen.
	if (m_record["sourceDigest"].get_ref<const std::string&>().empty())
	{
		// An armed-only cancellation has no copied image or passive authority to retain.
		m_destinationWriters.reset();
		m_destinationLease.reset();
		m_destination.reset();
	}
	return true;
}
fs::path uam::DataFolderTransition::CurrentRoot() const
{
	return m_destinationOwner || (!m_record.is_null() && m_record["phase"].get_ref<const std::string&>() == "starting" && !m_held) ? Path(m_record, "destination") : !m_record.is_null() ? Path(m_record, "source") : m_source->RootPath();
}
std::shared_ptr<uam::platform::DataRootLock> uam::DataFolderTransition::CurrentLease() const
{
	return m_destinationOwner || (!m_record.is_null() && m_record["phase"].get_ref<const std::string&>() == "starting" && !m_held) ? m_destinationLease : m_sourceLease;
}
std::string uam::DataFolderTransition::State() const
{
	return m_held ? "held" : m_record.is_null() ? "none" : m_record["phase"].get<std::string>();
}
bool uam::DataFolderTransition::Armed() const
{
	return !m_record.is_null() && m_record["phase"].get_ref<const std::string&>() == "armed";
}

bool uam::DataFolderTransition::MatchesPendingChoice(const fs::path& destination, std::string_view durable_revision) const
{
	std::error_code error;
	return Armed() && m_record["preferenceRevision"].get_ref<const std::string&>() == durable_revision && fs::equivalent(destination, Path(m_record, "destination"), error) && !error;
}

bool uam::DataFolderTransition::RetainsPassiveDestination(const fs::path& destination) const
{
	if (m_record.is_null() || m_record["phase"].get_ref<const std::string&>() != "rolledBack" || m_record["sourceDigest"].get_ref<const std::string&>().empty())
		return false;
	const auto retained = Path(m_record, "destination");
	std::error_code error;
	return destination == retained || (fs::equivalent(destination, retained, error) && !error);
}
