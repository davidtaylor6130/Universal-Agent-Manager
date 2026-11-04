#include "remote/runner_endpoint_identity.h"
#include "test_harness.h"

using namespace uam_test;

UAM_TEST(WindowsRunnerEndpointSeparatesInstallationRoots)
{
	const std::string first = uam::remote::WindowsRunnerEndpointDigest("same-user", "4.11.0", 3, "root-one");
	const std::string second = uam::remote::WindowsRunnerEndpointDigest("same-user", "4.11.0", 3, "root-two");
	UAM_ASSERT(first != second);
}

UAM_TEST(WindowsRunnerEndpointPreservesExplicitNamespace)
{
	const std::string first = uam::remote::WindowsRunnerEndpointDigest("same-user", "4.11.0", 3, "root-one", "acceptance");
	const std::string second = uam::remote::WindowsRunnerEndpointDigest("same-user", "4.11.0", 3, "root-two", "acceptance");
	UAM_ASSERT_EQ(first, second);
	std::uint64_t legacy = uam::hashing::kFnv1a64OffsetBasis;
	uam::hashing::UpdateFnv1a64(legacy, "same-user");
	uam::hashing::UpdateFnv1a64(legacy, "4.11.0");
	uam::hashing::UpdateFnv1a64(legacy, "3");
	uam::hashing::UpdateFnv1a64(legacy, "\nnamespace:acceptance");
	UAM_ASSERT_EQ(first, uam::hashing::Hex64Padded(legacy));
	UAM_ASSERT(first != uam::remote::WindowsRunnerEndpointDigest("different-user", "4.11.0", 3, "root-one", "acceptance"));
	UAM_ASSERT(first != uam::remote::WindowsRunnerEndpointDigest("same-user", "4.11.1", 3, "root-one", "acceptance"));
	UAM_ASSERT(first != uam::remote::WindowsRunnerEndpointDigest("same-user", "4.11.0", 4, "root-one", "acceptance"));
}

UAM_TEST(WindowsRunnerEndpointFailsClosedWithoutIdentity)
{
	UAM_ASSERT(uam::remote::WindowsRunnerEndpointDigest("same-user", "4.11.0", 3, "").empty());
	UAM_ASSERT(uam::remote::WindowsRunnerEndpointDigest("", "4.11.0", 3, "root-one", "acceptance").empty());
	UAM_ASSERT(!uam::remote::WindowsRunnerEndpointDigest("same-user", "4.11.0", 3, "", "acceptance").empty());
}
