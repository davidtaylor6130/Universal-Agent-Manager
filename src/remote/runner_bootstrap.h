#pragma once

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace uam::remote
{
	struct BootstrapStep
	{
		std::string label;
		std::vector<std::string> argv;
		std::string expected_output;
	};

	struct RunnerArtifact
	{
		std::string platform;
		std::string architecture;
		std::filesystem::path path;
		std::string sha256;
	};

	struct BootstrapPlan
	{
		std::string ssh_alias;
		std::string version;
		std::string install_directory;
		std::string runner_directory;
		std::string nonce;
		std::string previous_platform;
		std::string previous_version;
		std::string previous_runner_directory;
		int previous_protocol_version = 0;
		std::vector<RunnerArtifact> artifacts;
		std::vector<BootstrapStep> steps;
	};

	class BootstrapInstallLease;

	struct BootstrapResult
	{
		bool ok = false;
		std::string platform;
		std::string architecture;
		std::string error;
		std::shared_ptr<BootstrapInstallLease> install_lease;
	};

	bool BuildBootstrapPlan(const std::string& ssh_alias,
	                        const std::string& version,
	                        const std::string& nonce,
	                        std::vector<RunnerArtifact> artifacts,
	                        BootstrapPlan& plan,
	                        std::string* error_out = nullptr,
	                        const std::string& runner_directory = {});
	/// <summary>OS-held target lock, scoped to the helper root and owned until transaction finalization.</summary>
	std::vector<std::string> BuildBootstrapLockArgv(const BootstrapPlan& plan, std::string_view platform);
	std::shared_ptr<BootstrapInstallLease> AcquireBootstrapInstallLease(const BootstrapPlan& plan, std::string_view platform, std::string& error, std::stop_token stop_token = {});
	std::string BootstrapPlanPreview(const BootstrapPlan& plan);
	BootstrapResult ExecuteBootstrapPlan(const BootstrapPlan& plan,
	                                     std::stop_token stop_token = {});
	bool FinalizeBootstrapPlan(const BootstrapPlan& plan,
	                           const BootstrapResult& result,
	                           bool keep_new_runner,
	                           std::string* error_out = nullptr,
	                           std::stop_token stop_token = {});
}
