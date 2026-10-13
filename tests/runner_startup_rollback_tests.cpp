#include "remote/runner_startup.h"
#include "test_harness.h"

using namespace uam_test;

#if defined(__APPLE__)
namespace
{
	void VerifyStartupRollback(bool previously_enabled, bool enabling = true, const std::string& prior_state = "", bool failed_restore = false, bool failed_file_restore = false)
	{
		TempDir temp("uam-startup-partial-rollback");
		const fs::path home = fs::canonical(temp.root);
		const fs::path marker = home / "enabled";
		const fs::path manager = home / "systemctl";
		UAM_ASSERT(uam::io::WriteTextFile(manager,
		    "#!/bin/sh\ncase \"$2\" in\n"
		    "is-enabled) if test \"$3\" = --quiet && test \"$UAM_STARTUP_FAIL_FILE_RESTORE\" = yes; then rm -f \"$UAM_STARTUP_UNIT\"; mkdir \"$UAM_STARTUP_UNIT\"; touch \"$UAM_STARTUP_UNIT/blocked\"; exit 1; fi; if test \"$3\" != --quiet && test -n \"$UAM_STARTUP_PRIOR_STATE\"; then echo \"$UAM_STARTUP_PRIOR_STATE\"; exit 0; fi; if test \"$3\" != --quiet; then if test -f \"$UAM_STARTUP_MARKER\"; then echo enabled; exit 0; else echo disabled; exit 1; fi; fi; test -f \"$UAM_STARTUP_MARKER\" && test \"$UAM_STARTUP_FAIL_VERIFY\" != yes;;\n"
		    "enable) touch \"$UAM_STARTUP_MARKER\";;\n"
		    "disable) if test \"$UAM_STARTUP_FAIL_RESTORE\" = yes; then exit 1; fi; rm -f \"$UAM_STARTUP_MARKER\"; if test -f \"$UAM_STARTUP_FAIL_DISABLE\"; then rm -f \"$UAM_STARTUP_FAIL_DISABLE\"; exit 1; fi;;\n"
		    "*) exit 0;;\nesac\n"));
		fs::permissions(manager, fs::perms::owner_all);
		ScopedEnvVar path("PATH", home.string() + ":/usr/bin:/bin");
		ScopedEnvVar state("UAM_STARTUP_MARKER", marker.string());
		ScopedEnvVar fail("UAM_STARTUP_FAIL_VERIFY", "yes");
		ScopedEnvVar prior("UAM_STARTUP_PRIOR_STATE", prior_state);
		ScopedEnvVar restore_fail("UAM_STARTUP_FAIL_RESTORE", failed_restore ? "yes" : "no");
		const fs::path disable_failure = home / "fail-disable";
		ScopedEnvVar disable_fail("UAM_STARTUP_FAIL_DISABLE", disable_failure.string());
		if (!enabling) UAM_ASSERT(uam::io::WriteTextFile(disable_failure, ""));
		const fs::path executable = home / "runner/new/uam-runner";
		const fs::path socket = home / "runner/socket";
		const fs::path unit = home / ".config/systemd/user" / (uam::remote::RunnerStartupIdentity(executable) + ".service");
		ScopedEnvVar unit_path("UAM_STARTUP_UNIT", unit.string());
		ScopedEnvVar file_fail("UAM_STARTUP_FAIL_FILE_RESTORE", failed_file_restore ? "yes" : "no");
		const std::string previous = "# UAM managed runner startup\nPrevious configuration\n";
		if (previously_enabled) UAM_ASSERT(uam::io::WriteTextFile(marker, ""));
		if (previously_enabled || !enabling)
		{
			UAM_ASSERT(uam::io::WriteTextFile(unit, previous));
		}
		std::string error;
		UAM_ASSERT(!uam::remote::ConfigureLinuxRunnerStartup(PlatformServicesFactory::Instance().process_service,
		    home, executable, socket, enabling, error));
		UAM_ASSERT_EQ(fs::exists(marker), previously_enabled || failed_restore);
		if (failed_restore || failed_file_restore) UAM_ASSERT(error.find("could not be fully restored") != std::string::npos);
		if (failed_file_restore) UAM_ASSERT(fs::is_directory(unit));
		else if (previously_enabled || !enabling) UAM_ASSERT_EQ(uam::io::ReadTextFile(unit), previous);
		else UAM_ASSERT(!fs::exists(unit));
	}
}

UAM_TEST(RemoteLinuxStartupFailedVerificationRemovesNewRegistration)
{
	VerifyStartupRollback(false);
}

UAM_TEST(RemoteLinuxStartupFailedVerificationPreservesPreviousRegistration)
{
	VerifyStartupRollback(true);
}
UAM_TEST(RemoteLinuxStartupPartialDisableRestoresEnabledRegistration)
{
	VerifyStartupRollback(true, false);
}

UAM_TEST(RemoteLinuxStartupPartialDisablePreservesDisabledRegistration)
{
	VerifyStartupRollback(false, false);
}
UAM_TEST(RemoteLinuxStartupRollbackPreservesRuntimeAndInactiveStates)
{
	VerifyStartupRollback(true, true, "enabled-runtime");
	for (const std::string& state : {std::string("static"), std::string("masked"), std::string("not-found")})
	{
		VerifyStartupRollback(false, true, state);
	}
	VerifyStartupRollback(false, true, "unrecognized");
}
UAM_TEST(RemoteLinuxStartupReportsFailedRegistrationRollback)
{
	VerifyStartupRollback(false, true, "", true);
}
UAM_TEST(RemoteLinuxStartupReportsFailedUnitRestoration)
{
	VerifyStartupRollback(true, true, "", false, true);
}
#endif
