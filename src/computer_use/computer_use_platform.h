#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace uam::computer_use
{
	struct Element
	{
		int id = 0;
		std::string role;
		std::string label;
		double x = 0;
		double y = 0;
		double width = 0;
		double height = 0;
		bool enabled = true;
	};

	struct Target
	{
		std::string kind;
		std::uint64_t id = 0;
		std::string title;
		double x = 0;
		double y = 0;
		double width = 0;
		double height = 0;
		bool primary = false;
		std::string input_mode;
		std::uint64_t process_id = 0;
	};

	struct Capture
	{
		bool ok = false;
		std::string error;
		std::string png;
		int width = 0;
		int height = 0;
		double desktop_x = 0;
		double desktop_y = 0;
		double desktop_width = 0;
		double desktop_height = 0;
		std::string target_kind;
		std::uint64_t target_id = 0;
		std::uint64_t process_id = 0;
		std::string application_id;
		std::string application_title;
		std::string input_mode;
		std::vector<Element> elements;
	};

	struct Action
	{
		std::string kind;
		int element_id = 0;
		double x = 0;
		double y = 0;
		double end_x = 0;
		double end_y = 0;
		std::string button = "left";
		int click_count = 1;
		double delta_x = 0;
		double delta_y = 0;
		std::string text;
		std::vector<std::string> keys;
		int duration_ms = 0;
	};

	struct ApplicationIdentity
	{
		std::string id;
		std::string title;
	};

	std::vector<Target> ListTargets(std::string* error_out = nullptr);
	Capture CaptureTarget(const std::string& kind, std::uint64_t id, int max_width, int max_height);
	bool AcquireControllerLock(std::string* error_out = nullptr);
	void ReleaseControllerLock();
	bool EnsureActionPermission(std::string* error_out = nullptr);
	bool ExecuteAction(const Action& action, const Capture& reference, const std::function<bool()>& cancelled, std::string* error_out = nullptr, bool* input_applied_out = nullptr);
	bool ConfirmComputerUse(const std::string& message);
	int RunWithUi(const std::function<int()>& work);
} // namespace uam::computer_use
