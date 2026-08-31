#pragma once

#include "common/platform/platform_state_fields.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class IPlatformProcessService;

namespace uam::remote
{
	struct ProcessPollResult
	{
		bool running = false;
		int exit_code = -1;
		std::string standard_output;
		std::string standard_error;
	};

	struct DirectoryListing
	{
		std::string directory;
		std::string parent_directory;
		std::vector<std::pair<std::string, std::string>> directories;
		bool truncated = false;
	};

	std::vector<std::string> SshBridgeArgv(const std::string& ssh_alias,
	                                       const std::string& platform,
	                                       const std::string& version,
	                                       const std::string& runner_directory = {});

	class RunnerClient
	{
	  public:
		RunnerClient(IPlatformProcessService& process_service,
		             std::vector<std::string> bridge_argv,
		             std::string expected_version = {});
		~RunnerClient();
		RunnerClient(const RunnerClient&) = delete;
		RunnerClient& operator=(const RunnerClient&) = delete;

		bool Connect(std::string* error_out = nullptr);
		bool StartProcess(const std::string& session_id,
		                  const std::filesystem::path& working_directory,
		                  const std::vector<std::string>& argv,
		                  const std::vector<std::pair<std::string, std::string>>& environment,
		                  std::string* error_out = nullptr,
		                  bool attach_if_exists = false);
		bool WriteProcess(const std::string& session_id, std::string_view bytes,
		                  std::string* error_out = nullptr);
		bool CloseProcessInput(const std::string& session_id, std::string* error_out = nullptr);
		bool PollProcess(const std::string& session_id, ProcessPollResult& result,
		                 std::string* error_out = nullptr,
		                 const std::function<bool()>& interrupt = {},
		                 bool* interrupted_out = nullptr);
		bool StopProcess(const std::string& session_id, std::string* error_out = nullptr);
		bool RemoveProcess(const std::string& session_id, std::string* error_out = nullptr);
		bool OpenChannel(const std::string& channel_id, std::string* error_out = nullptr,
		                 bool attach_if_exists = true);
		bool WriteChannel(const std::string& channel_id, std::string_view direction,
		                  std::string_view bytes, std::string* error_out = nullptr);
		bool PollChannel(const std::string& channel_id, std::string_view direction,
		                 std::string& bytes, std::string* error_out = nullptr);
		bool CloseChannel(const std::string& channel_id, std::string* error_out = nullptr);
		bool UploadFile(const std::string& upload_id,
		                const std::filesystem::path& remote_path,
		                std::string_view bytes, std::string* error_out = nullptr);
		bool RemoveFile(const std::string& request_id,
		                const std::filesystem::path& remote_path,
		                std::string* error_out = nullptr);
		bool CopyFile(const std::string& request_id,
		              const std::filesystem::path& source_path,
		              const std::filesystem::path& target_path, bool overwrite,
		              std::string* error_out = nullptr);
		bool ListDirectories(const std::filesystem::path& remote_path,
		                     DirectoryListing& result,
		                     std::string* error_out = nullptr);
		void Disconnect();

	  private:
		bool Request(nlohmann::json request, nlohmann::json& response,
		             std::string* error_out,
		             const std::function<bool()>& interrupt = {},
		             bool* interrupted_out = nullptr);
		bool ReadResponse(nlohmann::json& response, std::string* error_out,
		                  const std::function<bool()>& interrupt,
		                  bool* interrupted_out);

		IPlatformProcessService& m_processService;
		std::vector<std::string> m_bridgeArgv;
		uam::platform::StdioProcessPlatformFields m_bridge;
		std::string m_received;
		std::string m_expectedVersion;
		std::uint64_t m_nextRequestId = 1;
		bool m_connected = false;
		bool m_directoryBrowsing = false;
	};
}
