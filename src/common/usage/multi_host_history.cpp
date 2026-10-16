#include "common/usage/antigravity_accounting.h"
#include "common/usage/multi_host_history.h"
#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/paths/path_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "remote/runner_client.h"
#include <unordered_set>
#include <stdexcept>
#include <atomic>

namespace uam::usage
{
	namespace
	{
		constexpr std::size_t kMaximumBytes = 64 * 1024 * 1024;
		std::int64_t Now()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}
		std::string Scope(const ExecutionHost& host)
		{
			return "host-" + host.id + "-" + hashing::Hex64Padded(hashing::Fnv1a64(nlohmann::json::array({host.ssh_alias, host.platform, host.runner_directory}).dump()));
		}
		bool SameTargets(const std::vector<ExecutionHost>& left, const std::vector<ExecutionHost>& right)
		{
			if (left.size() != right.size()) return false;
			for (std::size_t index = 0; index < left.size(); ++index)
				if (!execution_hosts::SameConnection(left[index], right[index]) || left[index].label != right[index].label) return false;
			return true;
		}
		RemoteAccountingFetcher DefaultFetcher(const std::filesystem::path& data_root)
		{
			return [data_root](const ExecutionHost& host, std::stop_token stop, std::string& error) -> std::optional<nlohmann::json>
			{
				if (host.runner_version.empty() || host.runner_protocol_version < 2)
				{ error = "Install this machine's UAM runner to collect usage."; return std::nullopt; }
				remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
				    remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version);
				nlohmann::json snapshot;
				const std::filesystem::path path = data_root / "usage" / "remote" / Scope(host) / "snapshot-v3.jsonl";
				if (!client.ReadUsageAccountingFile(path, snapshot, &error, stop)) return std::nullopt;
				return snapshot.is_object() ? std::optional<nlohmann::json>{std::move(snapshot)} : std::optional<nlohmann::json>{{{"receiptSnapshotPath", paths::Utf8PathString(path)}}};
			};
		}
		struct Data
		{
			std::vector<AccountingRecord> records;
			std::string source_scope = "local";
			std::shared_ptr<const ReceiptSnapshot> receipts;
			nlohmann::json metadata;
			std::int64_t as_of = 0;
			std::size_t bytes = 0;
			bool lifetime = false;
		};
		std::optional<Data> Decode(const nlohmann::json& document)
		{
			if (!document.is_object() || !document.contains("version") || (document["version"] != 1 && document["version"] != 2) ||
			    !document.contains("asOfMs") || !document["asOfMs"].is_number_integer() || document["asOfMs"] <= 0 || document["asOfMs"] > Now() ||
			    !document.contains("metadata") || !document["metadata"].is_object() || !document.contains("facts") || !document["facts"].is_array() || document["facts"].size() > 500000) return std::nullopt;
			Data data;
			if (document["version"] == 2)
			{
				if (!document.contains("range") || document["range"] != "all-recorded") return std::nullopt;
				data.lifetime = true;
			}
			else if (document.contains("range")) return std::nullopt;
			data.as_of = document["asOfMs"].get<std::int64_t>(); data.metadata = document["metadata"];
			const std::int64_t start = data.lifetime ? 0 : std::max<std::int64_t>(0, data.as_of / 86400000 * 86400000 - 89LL * 86400000);
			if (!data.metadata.contains("complete") || !data.metadata["complete"].is_boolean() || !data.metadata.contains("listingLimited") || !data.metadata["listingLimited"].is_boolean()) return std::nullopt;
			for (const char* key : {"filesChecked", "filesTotal", "bytesRead"})
				if (!data.metadata.contains(key) || !data.metadata[key].is_number_integer() || data.metadata[key] < 0 || data.metadata[key] > kMaxExactTokenCount) return std::nullopt;
			if (data.metadata["filesChecked"] > data.metadata["filesTotal"] || !data.metadata.contains("providers") || !data.metadata["providers"].is_array() || data.metadata["providers"].size() != provider_ids::kAllCliProviderIds.size()) return std::nullopt;
			std::unordered_set<std::string> providers, identities;
			for (const nlohmann::json& row : data.metadata["providers"])
			{
				if (!row.is_object() || !row.contains("providerId") || !row["providerId"].is_string() || !provider_ids::IsKnownCliProviderId(row["providerId"].get_ref<const std::string&>()) || !providers.insert(row["providerId"].get<std::string>()).second || !row.contains("available") || !row["available"].is_boolean()) return std::nullopt;
				for (const char* key : {"files", "errors", "invalidLines", "oversizedLines", "suppressedForkRecords", "pendingLines"})
					if (!row.contains(key) || !row[key].is_number_integer() || row[key] < 0 || row[key] > kMaxExactTokenCount) return std::nullopt;
			}
			for (const nlohmann::json& row : document["facts"])
			{
				std::optional<AccountingRecord> fact = DecodeAccountingRecord(row);
				if (!fact || ((fact->record.timestamp_ms < start || fact->record.timestamp_ms >= data.as_of) && !DecodeAntigravityAccountingRecord(fact->record)) || !identities.insert(fact->identity).second) return std::nullopt;
				data.records.push_back(std::move(*fact));
			}
			data.bytes = document.dump().size();
			if (data.bytes > kMaximumBytes) return std::nullopt;
			return data;
		}
		std::optional<Data> DecodeReceipts(std::shared_ptr<const ReceiptSnapshot> snapshot, bool pin = false)
		{
			if (!snapshot) return std::nullopt;
			if (pin)
			{
				// Cache publication atomically replaces its name; retain the prior inode for a last-good view.
				const std::filesystem::path path = io::MakeTempWritePath(snapshot->path.parent_path() / "receipt-view");
				std::error_code error; std::filesystem::create_hard_link(snapshot->path, path, error);
				if (error) { error.clear(); std::filesystem::copy_file(snapshot->path, path, std::filesystem::copy_options::none, error); }
				if (error) return std::nullopt;
				std::shared_ptr<ReceiptSnapshot> pinned = std::make_shared<ReceiptSnapshot>();
				pinned->path = path; pinned->metadata = snapshot->metadata; pinned->as_of = snapshot->as_of; pinned->bytes = snapshot->bytes; pinned->fingerprint = snapshot->fingerprint; pinned->facts_fingerprint = snapshot->facts_fingerprint; pinned->facts_bytes = snapshot->facts_bytes; pinned->lifetime = snapshot->lifetime; pinned->owned = true;
				snapshot = std::move(pinned);
			}
			std::optional<Data> data = Decode({{"version", snapshot->lifetime ? 2 : 1}, {"asOfMs", snapshot->as_of}, {"metadata", snapshot->metadata}, {"facts", nlohmann::json::array()}, {"range", snapshot->lifetime ? "all-recorded" : ""}});
			if (!snapshot->lifetime)
			{
				nlohmann::json header{{"version", 1}, {"asOfMs", snapshot->as_of}, {"metadata", snapshot->metadata}, {"facts", nlohmann::json::array()}};
				data = Decode(header);
			}
			if (data) { data->receipts = snapshot; data->bytes = 0; }
			return data;
		}
		struct LedgerCache
		{
			std::filesystem::path path;
			std::vector<std::string> keys;
			std::vector<std::string> sources;
			std::vector<std::shared_ptr<const ReceiptSnapshot>> receipts;
			std::shared_ptr<const Ledger> ledger;
			~LedgerCache() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
		};

		std::shared_ptr<const Ledger> MakeLedger(const std::vector<const Data*>& inputs, const std::filesystem::path& data_root, std::unique_ptr<LedgerCache>& cached, std::atomic<std::uint64_t>& imported_records, std::atomic<std::uint64_t>& ledger_builds, std::string& error, const std::function<bool()>& cancelled)
		{
			if (cancelled && cancelled()) return {};
			std::vector<std::string> keys;
			bool lifetime = true;
			for (const Data* data : inputs)
			{
				lifetime = lifetime && data->lifetime;
				nlohmann::json identity = nlohmann::json::array({data->source_scope, data->lifetime});
				if (data->receipts) identity.push_back(nlohmann::json::array({"receipts", data->receipts->facts_fingerprint.empty() ? data->receipts->fingerprint : data->receipts->facts_fingerprint, data->receipts->facts_fingerprint.empty() ? data->receipts->bytes : data->receipts->facts_bytes}));
				else
				{
					nlohmann::json facts = nlohmann::json::array();
					for (const AccountingRecord& entry : data->records) facts.push_back(EncodeAccountingRecord(entry));
					identity.push_back(std::move(facts));
				}
				keys.push_back(identity.dump());
			}
			bool same_inputs = cached && cached->keys == keys && cached->receipts.size() == inputs.size();
			for (std::size_t index = 0; same_inputs && index < inputs.size(); ++index)
			{
				const auto& previous = cached->receipts[index];
				const auto& current = inputs[index]->receipts;
				if (previous && current)
				{
					// Unchanged facts inherit validation only when their new header cannot narrow its scope.
					const auto cutoff = [](std::int64_t as_of) { return std::max<std::int64_t>(0, as_of / 86400000 * 86400000 - 89LL * 86400000); };
					if (current->as_of < previous->as_of || (!current->lifetime && cutoff(current->as_of) != cutoff(previous->as_of))) same_inputs = false;
				}
			}
			const std::int64_t now = Now();
			if (same_inputs && cached->ledger && now >= cached->ledger->GeneratedMs() && now / 30000 == cached->ledger->GeneratedMs() / 30000) return cached->ledger;
			std::unique_ptr<LedgerCache> replacement;
			if (!same_inputs)
			{
				static std::atomic<std::uint64_t> next{0};
				replacement = std::make_unique<LedgerCache>();
				replacement->path = data_root / "usage" / ("combined-" + std::to_string(now) + "-" + std::to_string(++next));
				replacement->keys = std::move(keys);
				for (const Data* data : inputs) replacement->sources.push_back(data->source_scope);
				if (paths::IsLinkOrReparsePointNoThrow(data_root / "usage") || paths::IsLinkOrReparsePointNoThrow(replacement->path)) { error = "Usage scratch storage is invalid."; return {}; }
				Store store(replacement->path);
				for (std::size_t index = 0; index < inputs.size(); ++index)
				{
					const Data& data = *inputs[index];
					std::vector<AccountingRecord> batch;
					const std::string scope = "source-" + std::to_string(index);
					const std::function<bool()> flush = [&]()
					{
						if (batch.empty()) return true;
						if (!store.ImportAccounting(batch, scope, error, cancelled)) return false;
						imported_records.fetch_add(batch.size());
						batch.clear(); return true;
					};
					const std::function<bool(const AccountingRecord&)> append = [&](const AccountingRecord& entry)
					{
						if (cancelled && cancelled()) return false;
						batch.push_back(entry);
						return batch.size() < 2048 || flush();
					};
					if (data.receipts)
					{
						if (!data.receipts->Visit(append, error, cancelled)) return {};
					}
					else for (const AccountingRecord& entry : data.records) if (!append(entry)) return {};
					if (!flush()) return {};
					replacement->receipts.push_back(data.receipts);
				}
			}
			LedgerCache& selected = replacement ? *replacement : *cached;
			// Clock ticks reread immutable accounting so rolling boundaries and future witnesses advance.
			std::optional<Ledger> ledger = Store(selected.path).ReadLifetimeLedger(now, error, cancelled);
			if (ledger && !lifetime)
			{
				// Legacy peers provide rolling coverage only; never advertise complete lifetime from them.
				nlohmann::json encoded = ledger->Encode(); encoded["version"] = 1; encoded.erase("range");
				const std::int64_t cutoff = ledger->WindowStart(90);
				nlohmann::json rows = nlohmann::json::array();
				for (nlohmann::json& row : encoded["rows"]) if (row[5] >= cutoff) rows.push_back(std::move(row));
				encoded["rows"] = std::move(rows);
				ledger = Ledger::Decode(encoded);
			}
			if (!ledger || (cancelled && cancelled())) return {};
			selected.ledger = std::make_shared<const Ledger>(std::move(*ledger));
			ledger_builds.fetch_add(1);
			if (replacement) cached = std::move(replacement);
			return cached->ledger;
		}

		nlohmann::json Coverage(const ExecutionHost& host, const Data* data, std::string_view error, bool cached, bool loading)
		{
			return {{"executionHostId", host.id}, {"label", host.label}, {"status", data ? cached ? "cached" : loading ? "loading" : "ready" : loading ? "loading" : "unavailable"},
			    {"asOfMs", data ? nlohmann::json(data->as_of) : nlohmann::json(nullptr)}, {"error", strings::SafeLine(error, 1024)}};
		}
	}

	MultiHostHistory::MultiHostHistory(std::filesystem::path data_root, std::vector<ExecutionHost> hosts)
	    : MultiHostHistory(data_root, std::make_shared<Collector>(data_root, true), std::move(hosts), DefaultFetcher(data_root)) {}

	MultiHostHistory::MultiHostHistory(std::filesystem::path data_root, std::shared_ptr<Collector> local, std::vector<ExecutionHost> hosts, RemoteAccountingFetcher fetch)
	    : m_dataRoot(std::move(data_root)), m_local(std::move(local)), m_fetch(std::move(fetch))
	{
		SetHosts(std::move(hosts));
		m_worker = std::jthread([this](std::stop_token stop) { Run(stop); });
	}

	MultiHostHistory::~MultiHostHistory()
	{
		m_worker.request_stop();
		std::shared_ptr<std::stop_source> scope_stop;
		{ std::lock_guard<std::mutex> lock(m_mutex); scope_stop = m_scopeStop; }
		scope_stop->request_stop();
		m_wake.notify_all();
		if (m_worker.joinable()) m_worker.join();
	}

	void MultiHostHistory::SetHosts(std::vector<ExecutionHost> hosts)
	{
		std::erase_if(hosts, [](const ExecutionHost& host) { return host.id == "local"; });
		std::shared_ptr<std::stop_source> previous;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (SameTargets(m_hosts, hosts)) return;
			previous = m_scopeStop; m_scopeStop = std::make_shared<std::stop_source>();
			m_hosts = std::move(hosts); m_views.clear(); ++m_generation;
		}
		previous->request_stop();
		m_wake.notify_all();
	}

	nlohmann::json MultiHostHistory::Query(std::string_view host_id, int days, const std::map<ModelKey, ModelPrices>& prices) const
	{
		if (days != 0 && days != 1 && days != 7 && days != 30 && days != 90) return {{"error", "Choose a valid usage period."}};
		if (host_id == "local") return m_local->Query(days, prices);
		View view;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (host_id != kAllMachinesHostId && !std::ranges::any_of(m_hosts, [host_id](const ExecutionHost& host) { return host.id == host_id; })) return {{"error", "Usage machine is no longer configured."}};
			const std::map<std::string, View>::const_iterator found = m_views.find(std::string(host_id));
			if (found == m_views.end()) return {{"pending", true}, {"collectionError", ""}};
			view = found->second;
		}
		if (!view.ledger) return {{"pending", true}, {"collecting", view.metadata.value("collecting", false)},
		    {"collectionError", view.metadata.value("collectionError", "Usage is unavailable for this machine.")}, {"coverage", std::move(view.coverage)}};
		nlohmann::json result = m_local->Render(*view.ledger, std::move(view.metadata), days, prices);
		result["coverage"] = std::move(view.coverage);
		return result;
	}

	void MultiHostHistory::Run(std::stop_token stop)
	{
		struct Peer
		{
			std::string scope;
			std::optional<Data> data;
			std::string error;
			bool cached = false;
			bool loading = true;
			std::chrono::steady_clock::time_point due{};
		};
		std::map<std::string, Peer> peers;
		std::map<std::string, std::unique_ptr<LedgerCache>> peer_ledgers;
		std::unique_ptr<LedgerCache> combined_ledger;
		std::vector<std::string> cache_scopes;
		std::shared_ptr<const ReceiptSnapshot> local_bytes;
		std::optional<Data> local;
		bool changed = true;
		std::uint64_t published_generation = 0;
		std::int64_t published_interval = 0;
		std::string local_error;
		bool local_collecting = true;
		try
		{
			while (!stop.stop_requested())
			{
				std::vector<ExecutionHost> hosts;
				std::shared_ptr<std::stop_source> scope_stop;
				std::uint64_t generation;
				{ std::lock_guard<std::mutex> lock(m_mutex); hosts = m_hosts; scope_stop = m_scopeStop; generation = m_generation; }
				std::stop_callback cancel(stop, [scope_stop]() { scope_stop->request_stop(); });
				const std::shared_ptr<const ReceiptSnapshot> bytes = m_local->Receipts();
				const std::pair<bool, std::string> local_status = m_local->CollectionStatus();
				if (local_status.first != local_collecting || local_status.second != local_error)
				{ local_collecting = local_status.first; local_error = local_status.second; changed = true; }
				if (bytes && bytes != local_bytes)
				{
					std::optional<Data> decoded = DecodeReceipts(bytes);
					if (decoded) { local = std::move(decoded); local_bytes = bytes; changed = true; }
					else { local_error = "Local accounting metadata is invalid."; changed = true; }
				}
				if (generation != published_generation)
					for (std::pair<const std::string, Peer>& peer : peers) peer.second.due = {};
				std::erase_if(peers, [&hosts](const std::pair<const std::string, Peer>& peer) { return !std::ranges::any_of(hosts, [&peer](const ExecutionHost& host) { return host.id == peer.first; }); });
				std::vector<std::string> scopes;
				for (const ExecutionHost& host : hosts) scopes.push_back(Scope(host));
				if (scopes != cache_scopes) { combined_ledger.reset(); cache_scopes = scopes; }
				std::erase_if(peer_ledgers, [&hosts](const auto& entry)
				{
					const auto host = std::ranges::find_if(hosts, [&entry](const ExecutionHost& item) { return item.id == entry.first; });
					return host == hosts.end() || (entry.second && !entry.second->sources.empty() && entry.second->sources.front() != Scope(*host));
				});
				std::size_t cache_bytes = local ? local->bytes : 0;
				for (const std::pair<const std::string, Peer>& peer : peers) if (peer.second.data) cache_bytes += peer.second.data->bytes;
				if (hosts.size() > 32)
				{
					View unavailable; unavailable.metadata = {{"collecting", false}, {"collectionError", "Usage supports up to 32 remote machines."}};
					std::map<std::string, View> unavailable_views;
					for (const ExecutionHost& host : hosts) unavailable_views.emplace(host.id, unavailable);
					unavailable_views.emplace(std::string(kAllMachinesHostId), std::move(unavailable));
					{ std::lock_guard<std::mutex> lock(m_mutex); if (generation == m_generation) m_views = std::move(unavailable_views); }
					std::unique_lock<std::mutex> wait_lock(m_waitMutex);
					m_wake.wait_for(wait_lock, stop, std::chrono::seconds(1), [&]() { return generation != m_generation; });
					continue;
				}
				for (const ExecutionHost& host : hosts)
				{
					if (!execution_hosts::IsPortableId(host.id) || !execution_hosts::IsSafeSshAlias(host.ssh_alias) || !execution_hosts::IsSafeRunnerDirectory(host.runner_directory))
					{
						Peer& invalid = peers[host.id];
						if (invalid.data) cache_bytes -= invalid.data->bytes;
						invalid.data.reset(); invalid.error = "This machine has invalid usage connection settings.";
						invalid.loading = false; invalid.due = std::chrono::steady_clock::time_point::max(); continue;
					}
					Peer& peer = peers[host.id];
					const std::string scope = Scope(host);
					if (peer.scope != scope)
					{
						if (peer.data) cache_bytes -= peer.data->bytes;
						peer = {}; peer.scope = scope;
						const std::filesystem::path receipt_path = m_dataRoot / "usage" / "remote" / scope / "snapshot-v3.jsonl";
						std::string receipt_error;
						if (!paths::IsLinkOrReparsePointNoThrow(m_dataRoot / "usage") && !paths::IsLinkOrReparsePointNoThrow(m_dataRoot / "usage" / "remote") && !paths::IsLinkOrReparsePointNoThrow(receipt_path.parent_path()) && !paths::IsLinkOrReparsePointNoThrow(receipt_path))
						{
							std::optional<Data> decoded = DecodeReceipts(ReadReceiptSnapshot(receipt_path, receipt_error, [&]() { return stop.stop_requested(); }), true);
							if (decoded) { peer.data = std::move(decoded); peer.cached = true; }
						}
						const std::filesystem::path path = m_dataRoot / "usage" / "remote" / scope / "snapshot-v1.json";
						std::string saved;
						if (!peer.data && !paths::IsLinkOrReparsePointNoThrow(m_dataRoot / "usage") && !paths::IsLinkOrReparsePointNoThrow(m_dataRoot / "usage" / "remote") && !paths::IsLinkOrReparsePointNoThrow(path.parent_path()) && !paths::IsLinkOrReparsePointNoThrow(path) && io::TryReadTextFile(path, saved, kMaximumBytes))
						{
							std::optional<Data> decoded = Decode(nlohmann::json::parse(saved, nullptr, false));
							if (decoded && decoded->bytes <= kMaximumBytes - std::min(kMaximumBytes, cache_bytes))
							{ cache_bytes += decoded->bytes; peer.data = std::move(decoded); peer.cached = true; }
						}
					}
				}
				// Rebuild only after publication, configuration changes or the next 30-second window tick.
				if (changed || generation != published_generation || Now() / 30000 != published_interval)
				{
					std::map<std::string, View> views;
					bool valid_totals = true;
					const std::function<void(nlohmann::json&, const nlohmann::json&)> add_count = [&](nlohmann::json& target, const nlohmann::json& source)
					{
						const std::int64_t increment = source.get<std::int64_t>();
						if (increment > kMaxExactTokenCount - target.get<std::int64_t>()) valid_totals = false;
						else target = target.get<std::int64_t>() + increment;
					};
					nlohmann::json metadata = EmptyHistoryMetadata(kAllMachinesHostId), coverage = nlohmann::json::array();
					metadata["complete"] = local.has_value() && local_error.empty(); metadata["collecting"] = local_collecting; metadata["collectionError"] = local_error;
					std::vector<const Data*> combined;
					bool lifetime = local && local->lifetime;
					const std::function<void(const Data&)> append = [&](const Data& data)
					{
						lifetime = lifetime && data.lifetime;
						combined.push_back(&data);
						metadata["complete"] = metadata["complete"].get<bool>() && data.metadata["complete"].get<bool>();
						metadata["listingLimited"] = metadata["listingLimited"].get<bool>() || data.metadata["listingLimited"].get<bool>();
						for (const char* key : {"filesChecked", "filesTotal", "bytesRead"}) add_count(metadata[key], data.metadata[key]);
						for (const nlohmann::json& source : data.metadata["providers"])
							for (nlohmann::json& target : metadata["providers"])
								if (source["providerId"] == target["providerId"])
								{
									target["available"] = target["available"].get<bool>() || source["available"].get<bool>();
									for (const char* key : {"files", "errors", "invalidLines", "oversizedLines", "suppressedForkRecords", "pendingLines"}) add_count(target[key], source[key]);
								}
					};
					if (local) append(*local);
					coverage.push_back(Coverage(execution_hosts::LocalHost(), local ? &*local : nullptr, local_error, false, local_collecting));
					for (const ExecutionHost& host : hosts)
					{
						Peer& peer = peers[host.id];
						if (peer.data) peer.data->source_scope = peer.scope;
						const Data* data = peer.data ? &*peer.data : nullptr;
						nlohmann::json row = Coverage(host, data, peer.error, peer.cached, peer.loading);
						coverage.push_back(row);
						View view;
						view.metadata = data ? data->metadata : EmptyHistoryMetadata(host.id); view.metadata["version"] = 3; view.metadata["executionHostId"] = host.id;
						view.metadata["complete"] = view.metadata.value("complete", false) && !peer.cached && peer.error.empty() && !peer.loading;
						view.metadata["collecting"] = peer.loading; view.metadata["collectionError"] = strings::SafeLine(peer.error, 1024); view.coverage = nlohmann::json::array({row});
						if (data) { std::string ledger_error; view.ledger = MakeLedger({data}, m_dataRoot, peer_ledgers[host.id], m_importedRecords, m_ledgerBuilds, ledger_error, [&]() { return stop.stop_requested() || scope_stop->stop_requested(); }); if (!view.ledger) view.metadata["collectionError"] = ledger_error; append(*data); }
						else metadata["complete"] = false;
						if (peer.cached || !peer.error.empty()) metadata["complete"] = false;
						metadata["collecting"] = metadata["collecting"].get<bool>() || peer.loading;
						views.emplace(host.id, std::move(view));
					}
					View total;
					std::string ledger_error;
					if (valid_totals && !combined.empty()) total.ledger = MakeLedger(combined, m_dataRoot, combined_ledger, m_importedRecords, m_ledgerBuilds, ledger_error, [&]() { return stop.stop_requested() || scope_stop->stop_requested(); });
					if (!total.ledger && !combined.empty()) metadata["collectionError"] = ledger_error.empty() ? "Could not combine usage accounting." : ledger_error;

					total.metadata = std::move(metadata); total.coverage = std::move(coverage); views.emplace(std::string(kAllMachinesHostId), std::move(total));
					{ std::lock_guard<std::mutex> lock(m_mutex); if (generation == m_generation && !stop.stop_requested()) m_views = std::move(views); }
					changed = false; published_generation = generation; published_interval = Now() / 30000;
				}
				const ExecutionHost* next = nullptr;
				for (const ExecutionHost& host : hosts)
					if (peers[host.id].due <= std::chrono::steady_clock::now() && (!next || peers[host.id].due < peers[next->id].due)) next = &host;
				if (next && !scope_stop->stop_requested())
				{
					Peer& peer = peers[next->id];
					std::string error;
					std::optional<nlohmann::json> document;
					try { if (m_fetch) document = m_fetch(*next, scope_stop->get_token(), error); }
					catch (const std::exception&) { error = "This machine's usage collection failed."; }
					changed = true;
					if (generation != m_generation || stop.stop_requested()) continue;
					peer.loading = false;
					if (document && document->is_object() && document->contains("pending") && (*document)["pending"] == true)
					{
						if (!document->contains("collectionError") || (*document)["collectionError"].is_string())
						{ error = document->value("collectionError", ""); peer.loading = error.empty(); }
						else error = "This machine returned invalid usage progress.";
					}
					else if (document && document->is_object() && document->contains("receiptSnapshotPath"))
					{
						const std::filesystem::path expected = m_dataRoot / "usage" / "remote" / peer.scope / "snapshot-v3.jsonl";
						if ((*document)["receiptSnapshotPath"] != paths::Utf8PathString(expected)) error = "This machine returned invalid receipt storage.";
						else
						{
							std::optional<Data> decoded = DecodeReceipts(ReadReceiptSnapshot(expected, error, [&]() { return stop.stop_requested() || scope_stop->stop_requested(); }), true);
							if (decoded) { peer.data = std::move(decoded); peer.cached = false; }
							else if (error.empty()) error = "This machine returned invalid accounting metadata.";
						}
					}
					else if (document)
					{
						std::optional<Data> decoded = Decode(*document);
						const std::size_t old_bytes = peer.data ? peer.data->bytes : 0;
						if (decoded && decoded->bytes <= kMaximumBytes - std::min(kMaximumBytes, cache_bytes - old_bytes))
						{
							const std::filesystem::path path = m_dataRoot / "usage" / "remote" / peer.scope / "snapshot-v1.json";
							if (!paths::IsLinkOrReparsePointNoThrow(m_dataRoot / "usage") && !paths::IsLinkOrReparsePointNoThrow(m_dataRoot / "usage" / "remote") && !paths::IsLinkOrReparsePointNoThrow(path.parent_path()) && !paths::IsLinkOrReparsePointNoThrow(path) && paths::CreateDirectoriesNoThrow(path.parent_path()) && io::AtomicWriteFile(path, document->dump()))
							{ peer.data = std::move(decoded); peer.cached = false; }
							else error = "Could not save this machine's usage cache.";
						}
						else error = "This machine returned invalid or oversized usage accounting.";
					}
					peer.error = error.empty() && !document ? "Usage collection is unavailable for this machine." : strings::SafeLine(error, 1024);
					if (!peer.error.empty() && peer.data) peer.cached = true;
					peer.due = std::chrono::steady_clock::now() + (peer.loading ? std::chrono::seconds(1) : std::chrono::seconds(30));
					continue;
				}
				std::unique_lock<std::mutex> wait_lock(m_waitMutex);
				m_wake.wait_for(wait_lock, stop, std::chrono::seconds(1), [&]() { return generation != m_generation; });
			}
		}
		catch (const std::exception&)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const std::string error = "Background machine accounting failed. Restart UAM to retry.";
			for (std::pair<const std::string, View>& entry : m_views)
			{
				entry.second.metadata["complete"] = false; entry.second.metadata["collecting"] = false;
				entry.second.metadata["collectionError"] = error;
				for (nlohmann::json& row : entry.second.coverage)
				{ row["status"] = row["asOfMs"].is_null() ? "unavailable" : "cached"; row["error"] = error; }
			}
			View unavailable; unavailable.metadata = {{"collecting", false}, {"collectionError", error}};
			m_views.try_emplace(std::string(kAllMachinesHostId), std::move(unavailable));
		}
	}
}
