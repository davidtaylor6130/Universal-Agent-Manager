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

	struct BootstrapPlan
	{
		std::string ssh_alias;
		std::string version;
		std::string install_directory;
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
	                        const std::filesystem::path& local_runner,
	                        const std::string& version,
	                        const std::string& sha256,
	                        const std::string& nonce,
	                        BootstrapPlan& plan,
	                        std::string* error_out = nullptr);
	std::string BootstrapPlanPreview(const BootstrapPlan& plan);
	BootstrapResult ExecuteBootstrapPlan(const BootstrapPlan& plan,
	                                     std::stop_token stop_token = {});
}
