#include "computer_use/computer_use_platform.h"
namespace uam::computer_use
{
	static bool Disabled(std::string* error)
	{
		if (error) *error = "Computer use is disabled in this build.";
		return false;
	}
	std::vector<Target> ListTargets(std::string* error)
	{
		Disabled(error);
		return {};
	}
	std::optional<ApplicationIdentity> ResolveApplication(std::string_view, std::string* error)
	{
		Disabled(error);
		return {};
	}
	bool LaunchApplication(const ApplicationIdentity&, std::string* error)
	{
		return Disabled(error);
	}
	Capture CaptureTarget(const std::string&, std::uint64_t, int, int)
	{
		Capture capture;
		capture.error = "Computer use is disabled in this build.";
		return capture;
	}
	bool AcquireControllerLock(std::string* error)
	{
		return Disabled(error);
	}
	void ReleaseControllerLock()
	{
	}
	bool EnsureCapturePermission(std::string* error)
	{
		return Disabled(error);
	}
	bool EnsureActionPermission(std::string* error)
	{
		return Disabled(error);
	}
	bool RequestCapturePermission(std::string* error)
	{
		return Disabled(error);
	}
	bool RequestActionPermission(std::string* error)
	{
		return Disabled(error);
	}
	ApplicationIdentity ApplicationIdentityForTarget(const std::string&, std::uint64_t, std::uint64_t)
	{
		return {};
	}
	void ConfigureVirtualCursorIdentity(const std::string&, const std::string&)
	{
	}
	bool OpenPermissionSettings(const std::string&, std::string* error)
	{
		return Disabled(error);
	}
	bool ExecuteAction(const Action&, const Capture&, const std::function<bool()>&, std::string* error, bool* input_applied)
	{
		if (input_applied) *input_applied = false;
		return Disabled(error);
	}
	int RunWithUi(const std::function<int()>& work)
	{
		return work();
	}
}
