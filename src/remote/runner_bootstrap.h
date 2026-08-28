#pragma once

#include <filesystem>
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
		std::vector<RunnerArtifact> artifacts;
		std::vector<BootstrapStep> steps;
	};

	struct BootstrapResult
	{
		bool ok = false;
		std::string platform;
		std::string architecture;
		std::string error;
	};

	bool BuildBootstrapPlan(const std::string& ssh_alias,
	                        const std::string& version,
	                        const std::string& nonce,
	                        std::vector<RunnerArtifact> artifacts,
	                        BootstrapPlan& plan,
	                        std::string* error_out = nullptr,
	                        const std::string& runner_directory = {});
	std::string BootstrapPlanPreview(const BootstrapPlan& plan);
	BootstrapResult ExecuteBootstrapPlan(const BootstrapPlan& plan,
	                                     std::stop_token stop_token = {});
}
