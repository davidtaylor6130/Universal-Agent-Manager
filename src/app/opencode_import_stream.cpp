#include "app/opencode_import_stream.h"
#include "common/provider/opencode_v2_session.h"
#include "app/runtime_orchestration_services.h"
#include "core/native_import_content.h"
#include "common/platform/platform_services.h"
#include "common/paths/app_paths.h"
#include <utility>
#include "common/utils/io_utils.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace
{
	class ExportReader : public nlohmann::json_sax<nlohmann::json>
	{
	  public:
		ExportReader(const std::function<bool(const Message&)>& emit, std::stop_token stop, std::string& error)
		    : m_emit(emit), m_stop(stop), m_error(error) {}
		bool null() override { return Scalar(nullptr); }
		bool boolean(bool value) override { return Scalar(value); }
		bool number_integer(number_integer_t value) override { return Scalar(value); }
		bool number_unsigned(number_unsigned_t value) override { return Scalar(value); }
		bool number_float(number_float_t value, const string_t&) override { return Scalar(value); }
		bool string(string_t& value) override { return Scalar(std::move(value)); }
		bool binary(binary_t&) override { return Fail("OpenCode export contained binary JSON."); }
		bool start_object(std::size_t) override { return Start(false); }
		bool start_array(std::size_t) override { return Start(true); }
		bool end_object() override { return End(); }
		bool end_array() override { return End(); }
		bool key(string_t& value) override
		{
			if (m_stop.stop_requested()) return Fail("OpenCode history loading was canceled.");
			if (!m_frames.empty()) m_keys.back() = value;
			else if (m_depth == 1)
			{
				m_root_key = value;
				if (value == "info" && std::exchange(m_info_seen, true)) return Fail("OpenCode export repeated session information.");
				if (value == "messages" && std::exchange(m_messages_seen, true)) return Fail("OpenCode export repeated messages.");
			}
			return true;
		}
		bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override
		{
			return Fail("OpenCode export contained incomplete or malformed JSON.");
		}
		bool Complete(const ChatSession& identity)
		{
			if (!m_info.is_object() || !m_messages_seen || !m_messages_array || m_depth != 0) return Fail("OpenCode export is missing session information or messages.");
			if (!m_info.contains("id") || !m_info["id"].is_string() || !m_info.contains("directory") || !m_info["directory"].is_string()) return Fail("OpenCode export omitted the session identity or workspace.");
			const std::string directory = m_info["directory"].get<std::string>();
			if (m_info["id"].get<std::string>() != identity.native_session_id || directory.empty() ||
			    !FolderDirectoryMatches(uam::paths::NormalizeExistingPath(PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(uam::strings::Trim(directory))),
			        uam::paths::NormalizeExistingPath(PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(uam::strings::Trim(identity.workspace_directory)))))
				return Fail("OpenCode exported a different session or workspace.");
			return !m_stop.stop_requested() || Fail("OpenCode history loading was canceled.");
		}
	  private:
		bool Fail(std::string error) { if (m_error.empty()) m_error = std::move(error); return false; }
		bool Scalar(nlohmann::json value)
		{
			if (m_stop.stop_requested()) return Fail("OpenCode history loading was canceled.");
			if (m_frames.empty())
			{
				if (m_depth == 0 || (m_depth == 1 && (m_root_key == "info" || m_root_key == "messages")) || (m_depth == 2 && m_in_messages)) return Fail("OpenCode export contained malformed message data.");
				if (m_depth == 1)
				{
					if ((m_root_key == "nextCursor" || m_root_key == "next_cursor") && !value.is_null() && (!value.is_string() || !value.get_ref<const std::string&>().empty()))
						return Fail("OpenCode export returned incomplete paginated history. Retry the import.");
					if ((m_root_key == "hasMore" || m_root_key == "has_more") && (!value.is_boolean() || value.get<bool>()))
						return Fail("OpenCode export returned incomplete paginated history. Retry the import.");
				}
				return true;
			}
			Append(std::move(value));
			return true;
		}
		void Append(nlohmann::json value)
		{
			if (m_frames.back().is_array()) m_frames.back().push_back(std::move(value));
			else m_frames.back()[m_keys.back()] = std::move(value);
		}
		bool Start(bool array)
		{
			if (m_stop.stop_requested()) return Fail("OpenCode history loading was canceled.");
			if (m_depth == 1 && (m_root_key == "nextCursor" || m_root_key == "next_cursor" || m_root_key == "hasMore" || m_root_key == "has_more")) return Fail("OpenCode export returned invalid pagination metadata.");
			if (m_depth == 0 && array) return Fail("OpenCode export is missing valid session information.");
			if (m_frames.empty() && m_depth == 1 && m_root_key == "messages")
			{
				if (!array) return Fail("OpenCode export is missing a valid messages array.");
				m_in_messages = m_messages_array = true;
			}
			else if (!m_frames.empty() || (m_depth == 1 && m_root_key == "info") || (m_depth == 2 && m_in_messages))
			{
				if (m_frames.empty())
				{
					if (array) return Fail("OpenCode export contained malformed message data.");
					m_capturing_info = m_depth == 1;
				}
				m_frames.push_back(array ? nlohmann::json::array() : nlohmann::json::object());
				m_keys.emplace_back();
			}
			++m_depth;
			return true;
		}
		bool End()
		{
			--m_depth;
			if (m_frames.empty())
			{
				if (m_depth == 1 && m_in_messages) m_in_messages = false;
				return true;
			}
			nlohmann::json value = std::move(m_frames.back());
			m_frames.pop_back();
			m_keys.pop_back();
			if (!m_frames.empty()) { Append(std::move(value)); return true; }
			if (m_capturing_info) { m_info = std::move(value); return true; }
			// Keep the established mapper and native tool metadata contract shared with remote and child history.
			nlohmann::json wrapper = {{"info", {{"id", "stream"}, {"directory", "/stream"}}}, {"messages", nlohmann::json::array()}};
			wrapper["messages"].push_back(std::move(value));
			ChatHistorySyncService::RemoteOpenCodeTranscript mapped = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(wrapper.dump());
			if (!mapped.success) return Fail(mapped.error);
			if (!mapped.verified_empty && !uam::HasImportedConversationContent(mapped.messages)) return Fail("OpenCode history contains entries that could not be imported. Retry after updating the provider.");
			for (const Message& message : mapped.messages)
				if (m_stop.stop_requested() || !m_emit(message)) return Fail("OpenCode history loading was canceled or its destination could not be written.");
			return true;
		}
		const std::function<bool(const Message&)>& m_emit;
		std::stop_token m_stop;
		std::string& m_error;
		std::size_t m_depth = 0;
		bool m_info_seen = false, m_messages_seen = false, m_messages_array = false, m_in_messages = false, m_capturing_info = false;
		std::string m_root_key;
		nlohmann::json m_info;
		std::vector<nlohmann::json> m_frames;
		std::vector<std::string> m_keys;
	};
}

namespace uam::opencode_import
{
	Spool Export(ProviderWorkerInvocation invocation, const std::filesystem::path& workspace, std::stop_token stop, std::string& error)
	{
		error.clear();
		if (stop.stop_requested()) { error = "OpenCode history loading was canceled."; return {}; }
		const std::optional<std::filesystem::path> temp = paths::TempDirectoryPathNoThrow();
		if (!temp) { error = "Could not locate private OpenCode export storage."; return {}; }
		const std::string id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (id.empty()) { error = "Could not create private OpenCode export identity."; return {}; }
		const std::filesystem::path directory = *temp / ("uam-opencode-export-" + id);
		std::error_code ec;
		if (!std::filesystem::create_directory(directory, ec) || ec) { error = "Could not create private OpenCode export storage."; return {}; }
		Spool source(new std::filesystem::path(directory / "export.json"), [](const std::filesystem::path* path)
		{
			paths::RemoveAllNoThrow(path->parent_path());
			delete path;
		});
#if !defined(_WIN32)
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
		if (ec) { error = "Could not protect private OpenCode export storage."; return {}; }
#endif
		io::AtomicWriteResult written;
		const bool complete = io::WriteAndSyncAtomicTemp(*source, [&](const std::function<bool(std::string_view)>& append)
		{
			invocation.output_sink = append;
			invocation.structured_stdout = true;
			const ProcessExecutionResult result = ExecuteProviderWorkerInvocation(invocation, workspace, opencode_v2::HistoryCommandTimeoutMilliseconds(invocation.argv), stop);
			if (!result.ok || result.exit_code != 0 || stop.stop_requested())
			{
				error = "OpenCode could not export the session: " + strings::NonEmptyOrFallback(result.error, "exit " + std::to_string(result.exit_code));
				return false;
			}
			return true;
		}, {}, written);
		if (!complete) { if (error.empty()) error = "OpenCode export could not be written: " + written.error; return {}; }
		return source;
	}

	bool Read(const std::filesystem::path& source, const ChatSession& identity, const std::function<bool(const Message&)>& emit, std::stop_token stop, std::string& error)
	try
	{
		error.clear();
		std::ifstream input(source, std::ios::binary);
		if (!input) { error = "OpenCode export could not be opened."; return false; }
		char character = 0;
		while (input.get(character) && character != '{')
			if (stop.stop_requested()) { error = "OpenCode history loading was canceled."; return false; }
		if (!input) { error = "OpenCode export did not contain JSON."; return false; }
		input.unget();
		ExportReader reader(emit, stop, error);
		if (!nlohmann::json::sax_parse(input, &reader)) return false;
		if (input.bad()) { error = "OpenCode export could not be completely read."; return false; }
		return reader.Complete(identity);
	}
	catch (const std::exception&)
	{
		error = "OpenCode export could not be completely read or mapped.";
		return false;
	}
}
