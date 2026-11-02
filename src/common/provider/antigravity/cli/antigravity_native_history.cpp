#include "common/provider/antigravity_native_history.h"
#include "common/provider/antigravity/cli/antigravity_legacy_crypto.h"
#include "common/chat/native_attachment_assets.h"
#include "common/memory/memory_storage_guard.h"
#include "common/paths/path_utils.h"
#include "common/platform/private_temp_directory.h"
#include "common/provider/provider_ids.h"
#include "common/utils/env_utils.h"
#include "common/utils/time_utils.h"
#include "core/native_import_content.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

// Source-observed SQLite user_version 1 and authenticated legacy envelopes. See the pinned primary
// decoder and synthetic fixtures in docs/reference/antigravity-native-history-decision.md.
// Unknown step/schema variants fail the entire conversation; never use a valid
// prefix or a display log as evidence that complete native history was read.
namespace uam::antigravity::history
{
	namespace
	{
		namespace fs = std::filesystem;
		constexpr std::size_t kMaxBytes = 64U * 1024U * 1024U;
		constexpr std::size_t kMaxSteps = 100000;
		using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>;
		using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
		struct Refused : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};
		void Require(bool value, const char* reason)
		{
			if (!value)
				throw Refused(reason);
		}
		struct Budget
		{
			std::stop_token cancel;
			std::chrono::steady_clock::time_point deadline;
			bool Stopped() const
			{
				return cancel.stop_requested() || std::chrono::steady_clock::now() >= deadline;
			}
			void Check() const
			{
				Require(!Stopped(), "Antigravity history scan stopped before completion; retry discovery.");
			}
		};
		bool Uuid(std::string_view value)
		{
			if (value.size() != 36)
				return false;
			for (std::size_t i = 0; i < value.size(); ++i)
			{
				if (i == 8 || i == 13 || i == 18 || i == 23)
				{
					if (value[i] != '-')
						return false;
				}
				else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
					return false;
			}
			return true;
		}
		struct Field
		{
			unsigned number = 0, wire = 0;
			std::uint64_t integer = 0;
			std::string_view bytes;
		};
		std::uint64_t Varint(std::string_view bytes, std::size_t& position)
		{
			std::uint64_t value = 0;
			for (unsigned shift = 0; shift < 70; shift += 7)
			{
				Require(position < bytes.size(), "Antigravity history contains truncated protobuf data.");
				const unsigned char part = bytes[position++];
				Require(shift < 63 || part <= 1, "Antigravity history contains overflowing protobuf data.");
				value |= static_cast<std::uint64_t>(part & 127) << shift;
				if (!(part & 128))
				{
					Require(shift == 0 || part != 0, "Antigravity history contains noncanonical protobuf integers.");
					return value;
				}
			}
			throw Refused("Antigravity history contains invalid protobuf data.");
		}
		struct Proto
		{
			std::vector<Field> fields;
			explicit Proto(std::string_view bytes)
			{
				for (std::size_t position = 0; position < bytes.size();)
				{
					Require(fields.size() < kMaxSteps, "Antigravity native record exceeds the field limit.");
					const auto tag = Varint(bytes, position);
					Require((tag >> 3) > 0 && (tag >> 3) <= 536870911, "Antigravity history has an invalid protobuf tag.");
					Field field{static_cast<unsigned>(tag >> 3), static_cast<unsigned>(tag & 7)};
					if (field.wire == 0)
						field.integer = Varint(bytes, position);
					else
					{
						Require(field.wire == 1 || field.wire == 2 || field.wire == 5, "Antigravity history has an unsupported protobuf encoding.");
						const auto size = field.wire == 2 ? Varint(bytes, position) : field.wire == 1 ? 8U : 4U;
						Require(size <= bytes.size() - position, "Antigravity history contains truncated protobuf data.");
						field.bytes = bytes.substr(position, static_cast<std::size_t>(size));
						position += static_cast<std::size_t>(size);
					}
					fields.push_back(field);
				}
			}
			const Field* One(unsigned number, unsigned wire) const
			{
				const Field* result = nullptr;
				for (const Field& field : fields)
					if (field.number == number)
					{
						Require(!result && field.wire == wire, "Antigravity history contains ambiguous native fields.");
						result = &field;
					}
				return result;
			}
			std::string Text(unsigned number) const
			{
				const Field* field = One(number, 2);
				if (!field)
					return {};
				std::string value(field->bytes);
				// JSON's strict encoder checks UTF-8 without replacing invalid bytes.
				Require(value.find('\0') == std::string::npos, "Antigravity text contains an invalid zero byte.");
				(void)nlohmann::json(value).dump();
				return value;
			}
			std::uint64_t Number(unsigned number) const
			{
				const Field* field = One(number, 0);
				return field ? field->integer : 0;
			}
			std::string_view Bytes(unsigned number) const
			{
				const Field* field = One(number, 2);
				return field ? field->bytes : std::string_view{};
			}
			void Known(std::initializer_list<unsigned> allowed) const
			{
				for (const Field& field : fields)
					Require(std::find(allowed.begin(), allowed.end(), field.number) != allowed.end(), "Antigravity history uses an unrecognised content field; retry with a supported reader.");
			}
		};
		MessageAttachment InlineImage(std::string_view bytes, const std::string& event, const std::string& source_field,
		    std::size_t image_index, const std::shared_ptr<uam::native_assets::PayloadBudget>& payload_budget,
		    const Budget& budget, const std::string& tool_call_id = {})
		{
			const Proto image(bytes);
			image.Known({1, 2});
			MessageAttachment attachment;
			attachment.mime_type = image.Text(2);
			Require(attachment.mime_type.starts_with("image/") && attachment.mime_type.size() <= 255 && std::all_of(attachment.mime_type.begin(), attachment.mime_type.end(), [](unsigned char c) { return c > 32 && c < 127; }), "Antigravity native image MIME type is missing or invalid.");
			attachment.native_part_key = event + ":image:" + std::to_string(image_index);
			attachment.id = attachment.native_part_key;
			attachment.name = "Native image " + std::to_string(image_index + 1);
			attachment.kind = "image";
			attachment.origin_execution_host_id = "local";
			attachment.native_source_field = source_field;
			attachment.native_tool_call_id = tool_call_id;
			attachment.source = AttachmentSource::NativeInlinePending;
			std::string asset_error;
			attachment.native_inline_payload = uam::native_assets::MakeInlinePayload(image.Text(1), attachment.mime_type, payload_budget, asset_error, budget.cancel);
			Require(attachment.native_inline_payload != nullptr, "Antigravity native image is invalid or exceeds its bounded budget.");
			attachment.content_digest = attachment.native_inline_payload->Digest();
			attachment.size_bytes = attachment.native_inline_payload->DecodedSize();
			return attachment;
		}
		void ValidateRenderInfo(std::string_view bytes)
		{
			// Only this source-qualified scalar projection is supported. Complex
			// render content (including media blocks) refuses the whole history.
			const Proto render(bytes);
			render.Known({1, 2, 4, 5, 6, 7, 11});
			for (const auto number : {1U, 2U, 5U, 6U, 7U}) (void)render.Text(number);
			for (const auto number : {4U, 11U}) Require(render.Number(number) <= 1, "Antigravity native render flag is invalid.");
		}
		std::string Timestamp(std::string_view bytes)
		{
			if (bytes.empty())
				return {};
			const Proto stamp(bytes);
			stamp.Known({1, 2});
			const auto seconds = stamp.Number(1), nanos = stamp.Number(2);
			Require(seconds <= 253402300799ULL && nanos < 1000000000, "Antigravity history timestamp is invalid.");
			std::ostringstream out;
			out << uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(static_cast<std::time_t>(seconds)), "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(9) << nanos << 'Z';
			return out.str();
		}
		std::string Workspace(const std::string& uri)
		{
			Require(uri.starts_with("file:///") && uri.size() <= 32768, "Antigravity history lacks an exact local workspace URI.");
			std::string decoded;
			const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
			for (std::size_t i = 7; i < uri.size(); ++i)
			{
				char c = uri[i];
				Require(c != '?' && c != '#', "Antigravity workspace URI is ambiguous.");
				if (c == '%')
				{
					Require(i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0, "Antigravity workspace URI has invalid escaping.");
					c = static_cast<char>((hex(uri[i + 1]) << 4) | hex(uri[i + 2]));
					i += 2;
				}
				Require(static_cast<unsigned char>(c) >= 32 && c != '\\', "Antigravity workspace URI contains invalid path data.");
				decoded += c;
			}
			(void)nlohmann::json(decoded).dump();
			Require(!decoded.starts_with("//"), "Antigravity workspace URI does not attest a local path.");
			const auto path = uam::paths::PathFromDecodedLocalFileUri(decoded);
			Require(path.is_absolute(), "Antigravity history workspace is not absolute.");
			return uam::paths::Utf8PathString(path);
		}
		Statement Query(sqlite3* db, const char* sql)
		{
			sqlite3_stmt* raw = nullptr;
			const int result = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
			Statement statement(raw, sqlite3_finalize);
			Require(result == SQLITE_OK, "Antigravity history schema is not supported.");
			return statement;
		}
		std::string Column(sqlite3_stmt* statement, int index, bool blob = false)
		{
			const int type = sqlite3_column_type(statement, index);
			if (type == SQLITE_NULL)
				return {};
			Require(type == (blob ? SQLITE_BLOB : SQLITE_TEXT), "Antigravity history contains an invalid column type.");
			const int size = sqlite3_column_bytes(statement, index);
			Require(size >= 0 && static_cast<std::size_t>(size) <= kMaxBytes, "Antigravity native record exceeds its bound.");
			return size ? std::string(static_cast<const char*>(sqlite3_column_blob(statement, index)), size) : std::string{};
		}
		bool DatabaseSiblingExists(const fs::path& legacy)
		{
			fs::path database = legacy;
			database.replace_extension(".db");
			std::error_code error;
			const auto status = fs::symlink_status(database, error);
			Require(!error || error == std::errc::no_such_file_or_directory, "Could not inspect the current Antigravity history format.");
			return status.type() != fs::file_type::not_found && status.type() != fs::file_type::none;
		}
		std::string Hex(std::string_view value);
		std::string LoadLegacyDatabase(sqlite3* raw, const fs::path& encrypted_snapshot, const Budget& budget)
		{
			// The legacy plaintext is a native Trajectory, not a display log. An
			// authenticated, bounded monolithic source supplies every ordered Step.
			// Convert exact fields to a memory-only query view shared by both formats;
			// no plaintext file, provider ID or transcript content is manufactured.
			const auto size = fs::file_size(encrypted_snapshot);
			Require(size <= kMaxBytes, "Antigravity legacy history exceeds its bounded snapshot.");
			std::ifstream input(encrypted_snapshot, std::ios::binary);
			std::string encrypted(static_cast<std::size_t>(size), '\0');
			for (std::size_t offset = 0; offset < encrypted.size();)
			{
				budget.Check();
				const auto count = std::min<std::size_t>(65536, encrypted.size() - offset);
				input.read(encrypted.data() + offset, static_cast<std::streamsize>(count));
				Require(input.gcount() == static_cast<std::streamsize>(count), "Could not read complete Antigravity legacy ciphertext.");
				offset += count;
			}
			Require(input.peek() == std::char_traits<char>::eof() && !input.bad(), "Antigravity legacy snapshot changed while reading.");
			const auto plain = detail::DecryptLegacy(encrypted);
			Require(plain != nullptr, "Antigravity legacy history authentication failed; unsupported or incomplete data remains unimported.");
			const Proto trajectory(plain->bytes);
			trajectory.Known({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
			Require(std::none_of(trajectory.fields.begin(), trajectory.fields.end(), [](const Field& field) { return field.number == 5; }), "Antigravity legacy history has parent context whose completeness is not yet supported.");
			Require(trajectory.One(7, 2) != nullptr, "Antigravity legacy history lacks original workspace metadata.");
			Require(sqlite3_exec(raw,
			    "PRAGMA temp_store=MEMORY; PRAGMA user_version=1; CREATE TABLE trajectory_meta(trajectory_id TEXT,cascade_id TEXT,trajectory_type INTEGER,source INTEGER);"
			    "CREATE TABLE trajectory_metadata_blob(id TEXT PRIMARY KEY,data BLOB); CREATE TABLE parent_references(idx INTEGER PRIMARY KEY,data BLOB);"
			    "CREATE TABLE steps(idx INTEGER PRIMARY KEY,step_type INTEGER,status INTEGER,has_subtrajectory INTEGER,metadata BLOB,error_details BLOB,step_payload BLOB,step_format INTEGER,permissions BLOB,task_details BLOB,render_info BLOB)",
			    nullptr, nullptr, nullptr) == SQLITE_OK, "Could not prepare memory-only Antigravity legacy decoding.");
			const auto bind_blob = [](sqlite3_stmt* row, int parameter, std::string_view bytes)
			{
				Require(sqlite3_bind_blob(row, parameter, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT) == SQLITE_OK, "Could not retain original Antigravity legacy fields.");
			};
			const auto identity = Query(raw, "INSERT INTO trajectory_meta VALUES(?1,?2,?3,?4)");
			const auto id = trajectory.Text(1), cascade = trajectory.Text(6);
			Require(Uuid(id) && Uuid(cascade) && id != cascade && trajectory.Number(4) == 4 && trajectory.Number(8) == 17, "Antigravity legacy source lacks its exact CLI conversation identity.");
			Require(sqlite3_bind_text(identity.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
			    sqlite3_bind_text(identity.get(), 2, cascade.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
			    sqlite3_bind_int(identity.get(), 3, 4) == SQLITE_OK && sqlite3_bind_int(identity.get(), 4, 17) == SQLITE_OK &&
			    sqlite3_step(identity.get()) == SQLITE_DONE, "Could not retain original Antigravity legacy identity.");
			const auto metadata = Query(raw, "INSERT INTO trajectory_metadata_blob VALUES('main',?1)");
			bind_blob(metadata.get(), 1, trajectory.Bytes(7));
			Require(sqlite3_step(metadata.get()) == SQLITE_DONE, "Could not retain original Antigravity legacy workspace.");
			const auto insert = Query(raw, "INSERT INTO steps VALUES(?1,?2,?3,0,?4,?5,?6,0,?7,?8,?9)");
			std::size_t index = 0;
			for (const Field& field : trajectory.fields)
			{
				budget.Check();
				if (field.number != 2) continue;
				Require(field.wire == 2 && index < kMaxSteps, "Antigravity legacy step inventory is malformed or excessive.");
				const Proto step(field.bytes);
				Require(step.One(6, 2) == nullptr, "Antigravity legacy subtrajectory completeness is not supported.");
				Require(step.Number(1) <= 2147483647 && step.Number(4) <= 2147483647, "Antigravity legacy step enum exceeds its bound.");
				Require(sqlite3_reset(insert.get()) == SQLITE_OK && sqlite3_clear_bindings(insert.get()) == SQLITE_OK &&
				    sqlite3_bind_int64(insert.get(), 1, static_cast<sqlite3_int64>(index++)) == SQLITE_OK &&
				    sqlite3_bind_int(insert.get(), 2, static_cast<int>(step.Number(1))) == SQLITE_OK &&
				    sqlite3_bind_int(insert.get(), 3, static_cast<int>(step.Number(4))) == SQLITE_OK, "Could not retain native Antigravity step ordering.");
				bind_blob(insert.get(), 4, step.Bytes(5));
				bind_blob(insert.get(), 5, step.Bytes(31));
				bind_blob(insert.get(), 6, field.bytes);
				bind_blob(insert.get(), 7, step.Bytes(133));
				bind_blob(insert.get(), 8, step.Bytes(148));
				// SQLite's redundant render column is derived from the authenticated
				// original generic result; present-empty is distinct from absence.
				if (step.Number(1) == 132)
				{
					const Proto generic(step.Bytes(140)), result(generic.Bytes(2));
					if (const Field* render = result.One(3, 2))
					{
						if (render->bytes.empty()) Require(sqlite3_bind_zeroblob(insert.get(), 9, 0) == SQLITE_OK, "Could not retain native render presence.");
						else bind_blob(insert.get(), 9, render->bytes);
					}
				}
				Require(sqlite3_step(insert.get()) == SQLITE_DONE, "Could not retain every original Antigravity legacy step.");
			}
			nlohmann::json context{{"format", "legacy-aes256gcm-v1-agy-1.4.4-337dcde66844"}, {"trajectoryId", id}, {"metadataHex", Hex(trajectory.Bytes(7))}};
			for (const Field& field : trajectory.fields)
				if (field.number == 3 || field.number == 9 || field.number == 10)
				{
					Require(field.wire == 2, "Antigravity legacy generator/executor context is malformed.");
					context["sourceContext"].push_back({{"field", field.number}, {"payloadHex", Hex(field.bytes)}});
				}
			return context.dump();
		}
		std::string Revision(const fs::path& path)
		{
			fs::path wal = path;
			wal += "-wal";
			fs::path journal = path;
			journal += "-journal";
			std::error_code error;
			const bool journal_present = fs::exists(journal, error);
			Require(!error && !journal_present, "Antigravity history has a pending rollback journal; retry after native recovery.");
			const auto main = uam::memory::StorageRevision(path), sidecar = uam::memory::StorageRevision(wal);
			Require(main && *main != "missing" && sidecar, "Could not inspect the Antigravity history snapshot.");
			// Windows timestamps can stay put across a same-size commit, so add SQLite's own change markers:
			// the database change counter (bytes 24-27) and the WAL header salts/checkpoint sequence (bytes 0-31).
			const auto header = [](const fs::path& file, std::size_t offset, std::size_t size)
			{
				std::ifstream input(file, std::ios::binary);
				std::string bytes(size, '\0');
				if (input && input.seekg(static_cast<std::streamoff>(offset)) && input.read(bytes.data(), static_cast<std::streamsize>(size))) return Hex(bytes);
				return std::string();
			};
			return *main + '/' + *sidecar + '/' + header(path, 24, 4) + '/' + header(wal, 0, 32);
		}
		struct Snapshot
		{
			fs::path directory;
			~Snapshot()
			{
				if (!directory.empty())
				{
					std::error_code ignored;
					fs::remove_all(directory, ignored);
				}
			}
			void Copy(const fs::path& source, const Budget& budget)
			{
				// Never open the native DB with SQLite: even read-only SQLite can create
				// or update a native WAL shared-memory sidecar. Read bounded bytes into a
				// private ephemeral snapshot, with main+WAL revisions fenced around it.
				const auto temporary = uam::platform::CreatePrivateHistoryTempDirectory();
				Require(temporary.has_value(), "Could not allocate private Antigravity history staging.");
				directory = *temporary;
				std::size_t total = 0;
				for (const std::string suffix : {"", "-wal"})
				{
					fs::path path = source;
					path += suffix;
					std::error_code error;
					if (!fs::exists(path, error) && !error && !suffix.empty())
						continue;
					const auto size = fs::file_size(path, error);
					Require(!error && size <= kMaxBytes - total, "Antigravity history snapshot exceeds its bound or is unreadable.");
					total += static_cast<std::size_t>(size);
					std::ifstream input(path, std::ios::binary);
					std::ofstream output(directory / ("snapshot.db" + suffix), std::ios::binary | std::ios::trunc);
					Require(input.good() && output.good(), "Could not stage Antigravity history read-only.");
					std::array<char, 65536> buffer{};
					std::size_t copied = 0;
					while (input)
					{
						budget.Check();
						input.read(buffer.data(), buffer.size());
						const auto count = input.gcount();
						Require(static_cast<std::size_t>(count) <= size - copied, "Antigravity native history changed during staging; retry discovery.");
						output.write(buffer.data(), count);
						copied += static_cast<std::size_t>(count);
					}
					output.flush();
					Require(input.eof() && copied == size && output.good(), "Could not finish the complete Antigravity history snapshot.");
				}
			}
		};
		struct Summary
		{
			fs::path path;
			std::string revision, workspace;
			std::optional<std::size_t> steps;
		};
		struct SummaryIndex
		{
			Snapshot snapshot;
			Database database{nullptr, sqlite3_close_v2};
			fs::path path;
			std::string revision;
			void Open(const fs::path& store, const Budget& budget, const SnapshotCopied& copied)
			{
				path = store / "conversation_summaries.db";
				std::error_code error;
				if (!fs::exists(path, error) && !error)
				{
					path.clear();
					return;
				}
				Require(!error, "Could not inspect the Antigravity native workspace index.");
				revision = Revision(path);
				snapshot.Copy(path, budget);
				Require(Revision(path) == revision, "Antigravity workspace index changed during discovery; retry the scan.");
				sqlite3* raw = nullptr;
				const auto filename = uam::paths::Utf8PathString(snapshot.directory / "snapshot.db");
				const int opened = sqlite3_open_v2(filename.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
				database.reset(raw);
				Require(opened == SQLITE_OK, "Could not open the isolated Antigravity workspace index.");
				sqlite3_busy_timeout(raw, 50);
				sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, static_cast<int>(kMaxBytes));
				sqlite3_progress_handler(raw, 1000, [](void* state) { return static_cast<const Budget*>(state)->Stopped() ? 1 : 0; }, const_cast<Budget*>(&budget));
				Require(sqlite3_exec(raw, "PRAGMA trusted_schema=OFF; PRAGMA query_only=ON; BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK, "Could not read the Antigravity workspace index.");
				auto integrity = Query(raw, "PRAGMA quick_check");
				Require(sqlite3_step(integrity.get()) == SQLITE_ROW && Column(integrity.get(), 0) == "ok" && sqlite3_step(integrity.get()) == SQLITE_DONE, "Antigravity workspace index is incomplete or corrupt.");
				if (copied) copied();
			}
			void Fence() const
			{
				Require(path.empty() || Revision(path) == revision, "Antigravity workspace index changed during discovery; retry the scan.");
			}
			Summary Read(const std::string& session) const
			{
				Summary result;
				if (!database) return result;
				result.path = path;
				result.revision = revision;
				auto row = Query(database.get(), "SELECT workspace_uris,step_count,source,parent_conversation_id,nesting_depth,not_fully_idle,battle_id FROM conversation_summaries WHERE conversation_id=?1");
				Require(sqlite3_bind_text(row.get(), 1, session.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK, "Could not bind the exact Antigravity workspace identity.");
				const int read = sqlite3_step(row.get());
				if (read == SQLITE_ROW)
				{
				// The CLI leaves `source` empty on real stores; only a non-empty value that is not ours is foreign.
				Require((Column(row.get(), 2).empty() || Column(row.get(), 2) == "antigravity-cli") && Column(row.get(), 3).empty() && Column(row.get(), 6).empty() && sqlite3_column_type(row.get(), 4) == SQLITE_INTEGER && sqlite3_column_int64(row.get(), 4) == 0 && sqlite3_column_type(row.get(), 5) == SQLITE_INTEGER && sqlite3_column_int64(row.get(), 5) == 0, "Antigravity workspace index has active, nested or foreign conversation context.");
					Require(sqlite3_column_type(row.get(), 1) == SQLITE_INTEGER && sqlite3_column_int64(row.get(), 1) >= 0 && sqlite3_column_int64(row.get(), 1) <= static_cast<sqlite3_int64>(kMaxSteps), "Antigravity native workspace index has an invalid step count.");
					result.steps = static_cast<std::size_t>(sqlite3_column_int64(row.get(), 1));
					const auto uris = nlohmann::json::parse(Column(row.get(), 0), nullptr, false);
					Require(uris.is_array() && !uris.empty() && uris.size() <= 64, "Antigravity workspace index lacks an exact local workspace.");
					for (const auto& uri : uris)
					{
						Require(uri.is_string(), "Antigravity workspace index has invalid URI data.");
						const auto workspace = Workspace(uri.get<std::string>());
						Require(result.workspace.empty() || workspace == result.workspace, "Antigravity workspace index has ambiguous original workspaces.");
						result.workspace = workspace;
					}
					Require(sqlite3_step(row.get()) == SQLITE_DONE, "Antigravity native workspace identity is duplicated.");
				}
				else
					Require(read == SQLITE_DONE, "Could not read the exact Antigravity workspace index row.");
				Require(Revision(path) == revision, "Antigravity workspace index changed during discovery; retry the scan.");
				return result;
			}
		};
		ToolCall Call(std::string_view bytes)
		{
			const Proto fields(bytes);
			fields.Known({1, 2, 3, 9});
			ToolCall call;
			call.id = fields.Text(1);
			call.name = fields.Text(2);
			call.args_json = fields.Text(3);
			Require(!call.id.empty() && call.id.size() <= 4096 && !call.name.empty() && call.name.size() <= 4096, "Antigravity tool history lacks its native call identity.");
			const auto args = nlohmann::json::parse(call.args_json, nullptr, false);
			Require(call.args_json.empty() || args.is_object(), "Antigravity tool arguments are incomplete or invalid.");
			call.status = "pending";
			return call;
		}
		std::string Hex(std::string_view value)
		{
			constexpr char alphabet[] = "0123456789abcdef";
			std::string result;
			result.reserve(value.size() * 2);
			for (unsigned char c : value)
			{
				result += alphabet[c >> 4];
				result += alphabet[c & 15];
			}
			return result;
		}
		// Lossless source evidence accompanies known structured native tool results.
		// This is native history data, never a command or a fabricated result.
		std::string NativeRecord(int type, std::string_view body, std::string_view error)
		{
			return nlohmann::json{{"nativeProvider", "antigravity-cli"}, {"stepType", type}, {"payloadHex", Hex(body)}, {"errorDetailsHex", Hex(error)}}.dump();
		}
		nlohmann::json NativeFields(std::string_view bytes, unsigned depth = 0)
		{
			const Proto proto(bytes);
			nlohmann::json result = nlohmann::json::array();
			for (const Field& field : proto.fields)
			{
				nlohmann::json item{{"field", field.number}, {"wire", field.wire}};
				if (field.wire == 0)
					item["integer"] = field.integer;
				else
				{
					item["hex"] = Hex(field.bytes);
					const bool printable = !field.bytes.empty() && std::all_of(field.bytes.begin(), field.bytes.end(), [](unsigned char c) { return c >= 32 || c == '\n' || c == '\r' || c == '\t'; });
					if (printable)
					{
						try
						{
							const auto text = std::string(field.bytes);
							(void)nlohmann::json(text).dump();
							item["text"] = text;
						}
						catch (const nlohmann::json::exception&)
						{
						}
					}
					else if (field.wire == 2 && depth < 4 && !field.bytes.empty())
					{
						try
						{
							item["fields"] = NativeFields(field.bytes, depth + 1);
						}
						catch (const Refused&)
						{
						}
					}
				}
				result.push_back(std::move(item));
			}
			return result;
		}
		ChatSession Read(const fs::path& path, const Budget& budget, const std::shared_ptr<uam::native_assets::PayloadBudget>& payload_budget, const SnapshotCopied& snapshot_copied, const Summary& summary)
		{
			budget.Check();
			const bool legacy = path.extension() == ".pb";
			Require(!legacy || !DatabaseSiblingExists(path), "Antigravity legacy history was superseded by SQLite; retry discovery.");
			const std::string revision = Revision(path);
			Snapshot snapshot;
			snapshot.Copy(path, budget);
			if (snapshot_copied)
				snapshot_copied();
			Require(Revision(path) == revision, "Antigravity native history changed during staging; retry discovery.");
			sqlite3* raw = nullptr;
			const auto filename = legacy ? ":memory:" : uam::paths::Utf8PathString(snapshot.directory / "snapshot.db");
			const int opened = sqlite3_open_v2(filename.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | (legacy ? SQLITE_OPEN_CREATE : 0), nullptr);
			Database database(raw, sqlite3_close_v2);
			Require(opened == SQLITE_OK, "Could not open the isolated Antigravity history snapshot.");
			sqlite3_busy_timeout(raw, 50);
			sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, static_cast<int>(kMaxBytes));
			sqlite3_progress_handler(raw, 1000, [](void* state) { return static_cast<const Budget*>(state)->Stopped() ? 1 : 0; }, const_cast<Budget*>(&budget));
			std::string legacy_context;
			if (legacy) legacy_context = LoadLegacyDatabase(raw, snapshot.directory / "snapshot.db", budget);
			Require(sqlite3_exec(raw, "PRAGMA trusted_schema=OFF; PRAGMA query_only=ON; BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK, "Could not start an Antigravity history snapshot.");
			auto integrity = Query(raw, "PRAGMA quick_check");
			Require(sqlite3_step(integrity.get()) == SQLITE_ROW && Column(integrity.get(), 0) == "ok" && sqlite3_step(integrity.get()) == SQLITE_DONE, "Antigravity native database is incomplete or corrupt.");
			auto version = Query(raw, "PRAGMA user_version");
			Require(sqlite3_step(version.get()) == SQLITE_ROW && sqlite3_column_int(version.get(), 0) == 1, "Antigravity native history version is unsupported.");
			auto identity = Query(raw, "SELECT trajectory_id,cascade_id,trajectory_type,source FROM trajectory_meta");
			Require(sqlite3_step(identity.get()) == SQLITE_ROW, "Antigravity native conversation identity is missing.");
			const std::string trajectory = Column(identity.get(), 0), session = Column(identity.get(), 1);
			Require(Uuid(trajectory) && Uuid(session) && trajectory != session && session == path.stem().string() && sqlite3_column_type(identity.get(), 2) == SQLITE_INTEGER && sqlite3_column_type(identity.get(), 3) == SQLITE_INTEGER && sqlite3_column_int64(identity.get(), 2) == 4 && sqlite3_column_int64(identity.get(), 3) == 17, "Antigravity native identity does not attest this CLI conversation.");
			Require(sqlite3_step(identity.get()) == SQLITE_DONE, "Antigravity conversation has ambiguous trajectory ownership.");
			auto parents = Query(raw, "SELECT 1 FROM parent_references LIMIT 1");
			Require(sqlite3_step(parents.get()) == SQLITE_DONE, "Antigravity history references parent context that this reader cannot yet attest completely.");
			auto metadata_query = Query(raw, "SELECT data FROM trajectory_metadata_blob WHERE id='main'");
			Require(sqlite3_step(metadata_query.get()) == SQLITE_ROW, "Antigravity original workspace metadata is missing.");
			const std::string metadata_bytes = Column(metadata_query.get(), 0, true);
			const Proto metadata(metadata_bytes);
			Require(metadata.Text(6) == session, "Antigravity native metadata belongs to a different conversation.");
			Require(metadata.Text(5).empty() && metadata.Number(17) == 0 && metadata.Number(16) == 0 && metadata.Bytes(22).empty(), "Antigravity native metadata identifies inherited or battle context whose complete history is not yet supported.");
			std::string workspace;
			const auto admit_workspace = [&](const std::string& uri)
			{
				if (uri.empty())
					return;
				const auto candidate = Workspace(uri);
				Require(workspace.empty() || candidate == workspace, "Antigravity conversation has ambiguous original workspaces.");
				workspace = candidate;
			};
			admit_workspace(metadata.Text(7));
			for (const Field& field : metadata.fields)
				if (field.number == 1)
				{
					Require(field.wire == 2, "Antigravity workspace metadata is invalid.");
					const Proto item(field.bytes);
					admit_workspace(item.Text(1));
					admit_workspace(item.Text(2));
				}
			Require(summary.workspace.empty() || workspace.empty() || summary.workspace == workspace, "Antigravity workspace index disagrees with original transcript ownership.");
			if (workspace.empty())
				workspace = summary.workspace;
			Require(!workspace.empty(), "Antigravity history lacks its original workspace.");
			Require(sqlite3_step(metadata_query.get()) == SQLITE_DONE, "Antigravity native metadata is ambiguous.");
			ChatSession chat;
			chat.id = session;
			chat.native_session_id = session;
			chat.provider_id = uam::provider_ids::kAntigravityCli;
			chat.execution_host_id = "local";
			chat.workspace_directory = workspace;
			chat.created_at = Timestamp(metadata.Bytes(2));
			chat.updated_at = chat.created_at;
			auto rows = Query(raw, "SELECT idx,step_type,status,has_subtrajectory,metadata,error_details,step_payload,step_format,permissions,task_details,render_info FROM steps ORDER BY idx");
			std::map<std::string, std::pair<std::size_t, std::size_t>> calls;
			std::size_t index = 0, bytes = metadata_bytes.size();
			bool authored_content = false;
			std::vector<MessageBlock> leading_context;
			int row;
			while ((row = sqlite3_step(rows.get())) == SQLITE_ROW)
			{
				budget.Check();
				for (int column : {0, 1, 2, 3, 7})
					Require(sqlite3_column_type(rows.get(), column) == SQLITE_INTEGER, "Antigravity native step columns are malformed.");
				Require(index < kMaxSteps && sqlite3_column_int64(rows.get(), 0) == static_cast<sqlite3_int64>(index), "Antigravity native history has missing or excessive steps.");
				const auto native_type = sqlite3_column_int64(rows.get(), 1), native_status = sqlite3_column_int64(rows.get(), 2);
				Require(native_type >= 0 && native_type <= 2147483647 && native_status >= 0 && native_status <= 2147483647, "Antigravity native step enums exceed their bounds.");
				const int type = static_cast<int>(native_type), status = static_cast<int>(native_status);
				Require((status == 3 || status == 6 || status == 7) && sqlite3_column_int64(rows.get(), 3) == 0 && sqlite3_column_int64(rows.get(), 7) == 0, "Antigravity history has unfinished, nested or unsupported steps; retry after the native conversation settles.");
				const std::string step_meta = Column(rows.get(), 4, true), error = Column(rows.get(), 5, true), payload = Column(rows.get(), 6, true);
				const std::string permissions = Column(rows.get(), 8, true), task = Column(rows.get(), 9, true), render = Column(rows.get(), 10, true);
				const bool render_present = sqlite3_column_type(rows.get(), 10) != SQLITE_NULL;
				const auto row_bytes = payload.size() + step_meta.size() + error.size() + permissions.size() + task.size() + render.size();
				Require(row_bytes <= kMaxBytes - bytes, "Antigravity history exceeds the transcript bound.");
				bytes += row_bytes;
				const Proto step(payload), meta(step_meta);
				Require(step.Number(1) == static_cast<unsigned>(type) && step.Number(4) == static_cast<unsigned>(status) && step.Bytes(5) == step_meta, "Antigravity native step envelope is inconsistent.");
				const Proto location(meta.Bytes(20));
				if (!location.fields.empty())
					Require(location.Text(1) == trajectory && location.Text(4) == session && location.Number(2) == index, "Antigravity step belongs to a different native conversation.");
				unsigned payload_field = 0;
				switch (type)
				{
				case 14:
					payload_field = 19;
					break;
				case 15:
					payload_field = 20;
					break;
				case 8:
					payload_field = 14;
					break;
				case 9:
					payload_field = 15;
					break;
				case 21:
					payload_field = 28;
					break;
				case 23:
					payload_field = 30;
					break;
				case 38:
					payload_field = 47;
					break;
				case 5:
					payload_field = 10;
					break;
				case 7:
					payload_field = 13;
					break;
				case 98:
					payload_field = 111;
					break;
				case 101:
					payload_field = 114;
					break;
				case 90:
					payload_field = 103;
					break;
				case 132:
					payload_field = 140;
					break;
				case 81:
					payload_field = 93;
					break;
				default:
					throw Refused("Antigravity history has an unrecognised native step; the conversation remains unimported.");
				}
				step.Known({1, 4, 5, 31, 133, 148, payload_field});
				Require(step.Bytes(31) == error && step.Bytes(133) == permissions && step.Bytes(148) == task, "Antigravity redundant native error, permission or task fields disagree.");
				Require(step.One(payload_field, 2) != nullptr, "Antigravity native step payload is missing.");
				const Proto body(step.Bytes(payload_field));
				if (type == 132)
				{
					body.Known({1, 2});
					for (const auto& field : body.fields)
						if (field.number == 1)
						{
							Require(field.wire == 2, "Antigravity generic arguments are malformed.");
							const Proto entry(field.bytes);
							entry.Known({1, 2});
							(void)entry.Text(1); (void)entry.Text(2);
						}
					Require(body.One(2, 2) != nullptr, "Antigravity generic tool result is missing.");
					const Proto result(body.Bytes(2));
					// Other fields can carry originals or richer content. Do not accept
					// their text prefix while retaining usable content only as hex.
					result.Known({1, 3});
					(void)result.Text(1);
					const Field* nested = result.One(3, 2);
					Require(render_present == (nested != nullptr), "Antigravity redundant render presence disagrees.");
					if (nested)
					{
						ValidateRenderInfo(nested->bytes);
						ValidateRenderInfo(render);
						Require(nested->bytes == render, "Antigravity redundant render data disagrees.");
					}
				}
				else Require(!render_present, "Antigravity non-generic step has unexpected redundant render data.");
				Message message;
				message.provider = chat.provider_id;
				message.created_at = Timestamp(meta.Bytes(1));
				message.interrupted = status == 6;
				const std::string event = "antigravity:" + session + ":" + trajectory + ":" + std::to_string(index++);
				message.provider_event_ids.push_back(event);
				message.blocks.push_back({"native_step_metadata", nlohmann::json{{"nativeEventId", event}, {"nativeStepType", type}, {"nativeStatus", status}, {"createdAt", message.created_at}, {"metadataHex", Hex(step_meta)}, {"errorDetailsHex", Hex(error)}, {"permissionsHex", Hex(permissions)}, {"taskDetailsHex", Hex(task)}, {"renderInfoHex", Hex(render)}}.dump()});
				if (meta.One(9, 2))
				{
					const Proto usage(meta.Bytes(9));
					Require(usage.Number(2) <= 2147483647 && usage.Number(3) <= 2147483647, "Antigravity native usage counts exceed the message bound.");
					message.tokens_input = static_cast<int>(usage.Number(2));
					message.tokens_output = static_cast<int>(usage.Number(3));
				}
				if (!message.created_at.empty())
					chat.updated_at = message.created_at;
				if (type == 14)
				{
					body.Known({1, 2, 3, 5});
					Require(!meta.One(3, 0) || meta.Number(3) == 4 || meta.Number(3) == 5, "Antigravity input source is unrecognised; provenance cannot be inferred.");
					message.role = meta.Number(3) == 4 ? MessageRole::User : MessageRole::System;
					// The 1.1.16 minimal native shape can omit metadata. Retain its
					// typed USER_INPUT as unknown-provenance user history, never as a
					// fabricated explicit-human or original input-write receipt.
					if (!meta.One(3, 0))
						message.role = MessageRole::User;
					message.content = body.Text(2);
					std::string items;
					for (const Field& field : body.fields)
						if (field.number == 3)
						{
							Require(field.wire == 2, "Antigravity input item is malformed.");
							const Proto item(field.bytes);
							item.Known({1});
							if (!items.empty())
								items += '\n';
							items += item.Text(1);
						}
					Require(items.empty() || message.content.empty() || items == message.content, "Antigravity input representations disagree.");
					if (message.content.empty())
						message.content = items;
					if (message.content.empty())
						message.content = body.Text(1);
					// Keep the original query wrapper as passive source data when distinct.
					const auto query = body.Text(1);
					if (!query.empty() && query != message.content)
						message.blocks.push_back({"native_input_context", query});
					// The stable provider_event_ids locator records the native row. No
					// provider-issued input ID or original UAM write receipt was observed.
					for (const Field& field : body.fields)
						if (field.number == 5)
						{
							Require(field.wire == 2, "Antigravity image record is malformed.");
							const auto image_index = message.attachments.size();
							message.attachments.push_back(InlineImage(field.bytes, event, "CortexStepUserInput.images[" + std::to_string(image_index) + "]", image_index, payload_budget, budget));
						}
					authored_content = authored_content || uam::HasImportedConversationContent(message);
				}
				else if (type == 15)
				{
					body.Known({1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15, 16, 17, 18, 19});
					for (const Field& field : body.fields)
						Require(field.wire == (field.number == 5 || field.number == 12 ? 0U : 2U), "Antigravity response field has an invalid native encoding.");
					for (const unsigned field : {11U, 13U, 19U})
						(void)body.One(field, 2);
					Require(body.Number(5) <= 1, "Antigravity response has an invalid native reasoning flag.");
					Require(!meta.One(3, 0) || meta.Number(3) == 2, "Antigravity model step source is inconsistent.");
					message.role = MessageRole::Assistant;
					message.content = body.Text(1);
					message.thoughts = body.Text(3);
					const auto display = body.Text(8);
					if (!display.empty() && display != message.content)
						message.blocks.push_back({"native_display_text", display});
					for (const Field& field : body.fields)
						if (field.number == 7)
						{
							Require(field.wire == 2, "Antigravity tool call is malformed.");
							ToolCall call = Call(field.bytes);
							Require(calls.emplace(call.id, std::pair{chat.messages.size(), message.tool_calls.size()}).second, "Antigravity native tool call identity is duplicated.");
							message.blocks.push_back({"native_tool_call_record", Hex(field.bytes), call.id});
							message.blocks.push_back({"tool_call", {}, call.id});
							message.tool_calls.push_back(std::move(call));
						}
					// Preserve signature/callback/stop and numeric native model metadata as
					// inert source data, never as a claimed managed execution binding.
					nlohmann::json observed = nlohmann::json::object();
					if (body.One(4, 2))
						observed["thinkingSignature"] = body.Text(4);
					if (body.One(6, 2))
						observed["nativeResponseField6"] = body.Text(6);
					if (body.One(15, 2))
						observed["nativeProviderMessageId"] = body.Text(15);
					if (body.One(12, 0))
						observed["stopReason"] = body.Number(12);
					if (meta.One(11, 0))
						observed["modelEnum"] = meta.Number(11);
					if (body.One(5, 0))
						observed["nativeReasoningFlag"] = body.Number(5);
					if (body.One(14, 2))
						observed["nativeReasoning14Hex"] = Hex(body.Bytes(14));
					if (body.One(16, 2))
						observed["nativeReasoning16Hex"] = Hex(body.Bytes(16));
					// Retain every source-qualified response field, including native
					// grounding, knowledge, duration and passback context. These inert
					// bytes are not a managed binding or an original input-write receipt.
					observed["nativePlannerPayloadHex"] = Hex(step.Bytes(payload_field));
					if (!observed.empty())
						message.blocks.push_back({"native_response_metadata", observed.dump()});
					authored_content = authored_content || uam::HasImportedConversationContent(message);
				}
				else if (type == 23 || type == 98 || type == 101 || type == 90 || type == 81)
				{
					message.role = MessageRole::System;
					message.blocks.push_back({"native_context_record", NativeRecord(type, step.Bytes(payload_field), error)});
				}
				else
				{
					const auto metadata_call = meta.Bytes(4);
					const auto payload_call = type == 38 ? body.Bytes(2) : std::string_view{};
					const ToolCall native_call = Call(metadata_call.empty() ? payload_call : metadata_call);
					if (!metadata_call.empty() && !payload_call.empty())
					{
						const ToolCall mirror = Call(payload_call);
						Require(native_call.id == mirror.id && native_call.name == mirror.name && native_call.args_json == mirror.args_json, "Antigravity native tool identity representations disagree.");
					}
					const auto found = calls.find(native_call.id);
					Require(found != calls.end(), "Antigravity tool result has no original native call.");
					ToolCall& call = chat.messages.at(found->second.first).tool_calls.at(found->second.second);
					Require(call.name == native_call.name && call.args_json == native_call.args_json && call.status == "pending", "Antigravity native tool correlation is ambiguous.");
					// Refuse unrecognised result fields, especially original media that
					// cannot be represented by a text-only projection of these tools.
					if (type == 8) body.Known({1, 2, 3, 4, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20});
					if (type == 9) body.Known({1, 2, 3, 4, 5});
					if (type == 7) body.Known({1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
					if (type == 5) body.Known({1, 2, 4, 5, 7, 8, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 24, 25, 26, 27, 28});
					if (type == 21) body.Known({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37});
					std::string result;
					if (type == 8)
					{
						result = body.Text(4);
						if (result.empty())
							result = body.Text(9);
					}
					if (type == 21)
					{
						result = Proto(body.Bytes(21)).Text(1);
						if (result.empty())
							result = body.Text(4);
					}
					if (type == 7)
					{
						result = body.Text(3);
						if (!body.Text(5).empty())
							result += "\n" + body.Text(5);
					}
					if (type == 38)
					{
						body.Known({1, 2, 3, 5});
						(void)body.Text(1);
						result = body.Text(3);
						std::size_t image_index = 0;
						for (const Field& field : body.fields)
							if (field.number == 5)
							{
								Require(field.wire == 2, "Antigravity MCP image record is malformed.");
								auto& response = chat.messages.at(found->second.first);
								response.attachments.push_back(InlineImage(field.bytes, event, "CortexStepMcpTool.images[" + std::to_string(image_index) + "]", image_index, payload_budget, budget, call.id));
								++image_index;
							}
					}
					if (type == 132)
						result = Proto(body.Bytes(2)).Text(1);
					// Native structured edits/listings and all additional result fields
					// remain losslessly retained rather than inventing a derived success.
					message.role = MessageRole::System;
					message.blocks.push_back({"native_tool_result", NativeRecord(type, step.Bytes(payload_field), error), call.id});
					call.result_text = std::move(result);
					if (!error.empty())
					{
						const Proto details(error);
						if (details.One(9, 2))
						{
							const auto text = details.Text(9);
							if (!text.empty())
							{
								if (!call.result_text.empty())
									call.result_text += '\n';
								call.result_text += text;
							}
						}
					}
					// An absent textual result stays absent, including image-only MCP
					// output. Raw native evidence belongs only to canonical blocks;
					// exposing it as tool text bypasses frontend evidence filtering.
					if (type == 9 || type == 5)
						call.result_text = nlohmann::json{{"nativeStepType", type}, {"nativeFields", NativeFields(step.Bytes(payload_field))}, {"nativeErrorFields", NativeFields(error)}}.dump(2);
					call.status = status == 6 ? "cancelled" : status == 7 || !error.empty() || (type == 21 && body.Number(6) != 0) || (type == 7 && !body.Text(5).empty()) ? "error" : "completed";
					authored_content = true;
				}
				// Preserve source-only steps without consuming display/page indices.
				// Tool-call positions are assigned only to retained assistant rows;
				// reasoning, originals and native interruption markers stay visible.
				const bool visible = !uam::strings::IsBlank(message.content) || !uam::strings::IsBlank(message.thoughts) ||
				    !message.attachments.empty() || !message.tool_calls.empty() || !message.plan_entries.empty() ||
				    !uam::strings::IsBlank(message.plan_summary) || (message.role != MessageRole::System && message.interrupted);
				if (!visible)
				{
					auto& evidence = chat.messages.empty() ? leading_context : chat.messages.back().blocks;
					for (auto& block : message.blocks) evidence.push_back(std::move(block));
				}
				else
				{
					if (!leading_context.empty())
					{
						message.blocks.insert(message.blocks.begin(), std::make_move_iterator(leading_context.begin()), std::make_move_iterator(leading_context.end()));
						leading_context.clear();
					}
					chat.messages.push_back(std::move(message));
				}
			}
			Require(row == SQLITE_DONE, "Could not read every Antigravity native step.");
			Require(!summary.steps || *summary.steps == index, "Antigravity transcript and workspace index disagree on complete history length; retry discovery.");
			for (const auto& [id, location] : calls)
			{
				Message& response = chat.messages.at(location.first);
				ToolCall& call = response.tool_calls.at(location.second);
				// A native cancelled response is itself the terminal witness for an
				// unexecuted call; retain it without fabricating output or success.
				if (call.status == "pending" && response.interrupted)
					call.status = "cancelled";
				Require(call.status != "pending", "Antigravity native tool history is unfinished; retry discovery.");
			}
			Require(Revision(path) == revision, "Antigravity native history changed during discovery; retry the scan.");
			Require(!legacy || !DatabaseSiblingExists(path), "Antigravity legacy history was superseded during discovery; retry the scan.");
			Require(summary.path.empty() || Revision(summary.path) == summary.revision, "Antigravity workspace index changed while its native transcript was being read; retry discovery.");
			budget.Check();
			if (!authored_content)
				return {};
			if (legacy && !chat.messages.empty()) chat.messages.front().blocks.push_back({"native_legacy_trajectory_context", std::move(legacy_context)});
			chat.title = "Antigravity conversation";
			chat.messages_loaded = true;
			chat.persisted_message_count = chat.messages.size();
			return chat;
		}
	} // namespace

	Discovery Discover(const std::filesystem::path& root, const std::string& native_session, std::stop_token cancel, const SnapshotCopied& snapshot_copied, const WorkspaceFilter& workspace_filter, const SnapshotCopied& summary_snapshot_copied)
	{
		Discovery result;
		const auto issue = [&](std::string message) { if (result.errors.size() < 20) result.errors.push_back(std::move(message)); else ++result.omitted_error_count; };
		try
		{
			fs::path store = root;
			if (store.empty())
			{
				const auto home = uam::env::GetUserHomePath();
				Require(home && home->is_absolute(), "Could not resolve the Antigravity CLI history root.");
				store = *home / ".gemini" / "antigravity-cli";
			}
			Require(native_session.empty() || Uuid(native_session), "Antigravity native session identity is invalid.");
			std::error_code error;
			const fs::path conversations = store / "conversations";
			if (!fs::exists(conversations, error) && !error)
				return result;
			Require(!error && !fs::is_symlink(fs::symlink_status(conversations, error)) && fs::is_directory(conversations, error) && !error, "Could not inspect the Antigravity CLI conversations directory.");
			const Budget budget{cancel, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
			const auto payload_budget = std::make_shared<uam::native_assets::PayloadBudget>();
			std::vector<fs::path> files;
			if (!native_session.empty())
			{
				const auto legacy = conversations / (native_session + ".pb");
				files.push_back(DatabaseSiblingExists(legacy) ? conversations / (native_session + ".db") : legacy);
			}
			else
				for (fs::directory_iterator it(conversations, error), end; !error && it != end; it.increment(error))
				{
					budget.Check();
					Require(files.size() < 2048, "Antigravity history inventory exceeds this bounded scan; use an exact session refresh.");
					if (Uuid(it->path().stem().string()))
					{
						if (it->path().extension() == ".db")
							files.push_back(it->path());
						else if (it->path().extension() == ".pb" && !DatabaseSiblingExists(it->path()))
							files.push_back(it->path());
					}
				}
			Require(!error, "Could not finish the Antigravity CLI history inventory.");
			std::sort(files.begin(), files.end());
			SummaryIndex summaries;
			if (!files.empty()) summaries.Open(store, budget, summary_snapshot_copied);
			std::size_t retained_bytes = 0;
			for (const fs::path& file : files)
			{
				budget.Check();
				try
				{
					const Summary summary = summaries.Read(file.stem().string());
					if (workspace_filter && !summary.workspace.empty() && !workspace_filter(summary.workspace)) continue;
					ChatSession chat = Read(file, budget, payload_budget, snapshot_copied, summary);
					if (workspace_filter && !chat.native_session_id.empty() && !workspace_filter(chat.workspace_directory)) continue;
					if (chat.native_session_id.empty())
						continue;
					std::size_t size = 0;
					for (const Message& message : chat.messages)
					{
						size += message.content.size() + message.thoughts.size();
						for (const auto& block : message.blocks)
							size += block.text.size();
						for (const auto& call : message.tool_calls)
							size += call.args_json.size() + call.result_text.size();
					}
					Require(size <= kMaxBytes - retained_bytes, "Antigravity staged histories exceed this scan's memory budget; use scoped discovery.");
					retained_bytes += size;
					result.chats.push_back(std::move(chat));
				}
				catch (const Refused& failure)
				{
					issue("Antigravity conversation " + file.stem().string() + ": " + failure.what());
				}
				catch (const std::exception&)
				{
					issue("Antigravity native history could not be fully decoded; the affected conversation was not staged.");
				}
			}
			try { summaries.Fence(); }
			catch (...) { result.chats.clear(); throw; }
		}
		catch (const Refused& failure)
		{
			issue(failure.what());
		}
		catch (const std::exception&)
		{
			issue("Antigravity native history could not be fully decoded; no affected conversation was staged.");
		}
		return result;
	}
} // namespace uam::antigravity::history
