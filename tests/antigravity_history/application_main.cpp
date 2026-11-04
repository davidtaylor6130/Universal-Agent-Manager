#include "../test_harness.h"
#include "application_platform.h"

#include <array>
#include <iostream>

int main()
{
	// Provider stores, settings and process work are inaccessible to this runner.
	// Native DB fixtures still pass their own exact roots to the real reader.
	uam_test::TempDir sandbox("uam-native-import-application");
	uam_test::SetNativeImportFixtureRoot(sandbox.root);
	uam_test::ScopedEnvVar home("HOME", sandbox.root.string());
	uam_test::ScopedEnvVar user_profile("USERPROFILE", sandbox.root.string());
	uam_test::ScopedEnvVar temporary("TMPDIR", sandbox.root.string());
	uam_test::ScopedEnvVar data("UAM_DATA_DIR", (sandbox.root / "default-data").string());
	uam_test::ScopedEnvVar gemini("GEMINI_CLI_HOME", (sandbox.root / "gemini").string());
	uam_test::ScopedEnvVar codex("CODEX_HOME", (sandbox.root / "codex").string());
	uam_test::ScopedEnvVar claude("CLAUDE_CONFIG_DIR", (sandbox.root / "claude").string());
	uam_test::ScopedEnvVar copilot("COPILOT_HOME", (sandbox.root / "copilot").string());
	uam_test::ScopedEnvVar xdg_data("XDG_DATA_HOME", (sandbox.root / "xdg-data").string());
	uam_test::ScopedEnvVar xdg_config("XDG_CONFIG_HOME", (sandbox.root / "xdg-config").string());
	uam_test::ScopedEnvVar xdg_cache("XDG_CACHE_HOME", (sandbox.root / "xdg-cache").string());
	uam_test::ScopedEnvVar app_data("APPDATA", (sandbox.root / "app-data").string());
	uam_test::ScopedEnvVar local_app_data("LOCALAPPDATA", (sandbox.root / "local-app-data").string());
	const std::array names{"SettingsAntigravityNativeImageOnlyMcpKeepsOwnedOriginalOutOfFrontendToolText", "SettingsAntigravityNativeManyRefusalsRetainOmissionCountAcrossStaging", "SettingsAntigravityNativeMetadataRowsDoNotConsumePersistedMessagePages", "SettingsNativeAssignmentsResolvePathAliasesAndRefuseLinkedWorktreeConflicts", "SettingsNativeMultiBatchPublicationOnlyCreatesAdmittedWorkspaceOwners", "SettingsNativeExplicitRescanPreservesSettledRemoteCandidatesWithoutRemoteWork", "SettingsNativeSharedTestHomeNeverReadsAmbientAntigravityHistory", "SettingsNativeDiscoveryIsLocalAndRejectsOverlappingScans", "SettingsNativeRescanPreservesSavedOwnershipAndTombstones", "SettingsNativeUnregisteredWorkspaceAssignmentIsReadOnlyUntilSelectedImport", "SettingsNativeUnregisteredAssignmentRetainsHiddenOwnershipAndLateDeletion", "SettingsNativeWorkspacePlanRejectsConcurrentCatalogueChanges", "SettingsNativeWorkspaceSaveFailureDoesNotPublishChats", "SettingsNativeAssignmentCanPublishIntoAnEmptyWorkspaceCatalogue", "SettingsNativeAssignmentRejectsLateHiddenOwnerBeforePublication", "SettingsNativeSixProviderAssignmentAndDeclineRetainExactSourceScope", "SettingsAntigravityNativeDiscoveryAssignmentAndSelectedOriginalPublication", "SettingsAntigravityNativeHiddenOwnerAndLateTombstonePreventPublication", "SettingsAntigravityLegacyDiscoveryAssignmentAndSelectedPublication", "NativeMediaPreparedSaveRetainsUnacknowledgedOriginalsOnRejectedPublication", "NativeAssetStorePublicationAttemptRetainsOriginalsAcrossFalseAndException", "NativeAssetStoreFailedAttemptReceiptRetainsOriginalsConservatively"};
	std::size_t failures = 0;
	for (const char* name : names)
	{
		const auto found = std::ranges::find_if(uam_test::Registry(), [name](const auto& test) { return test.name == name; });
		try
		{
			UAM_ASSERT(found != uam_test::Registry().end());
			found->fn();
			UAM_ASSERT_EQ(uam_test::NativeImportFixtureUnexpectedCalls(), std::size_t{0});
			std::cout << "PASS " << name << '\n';
		}
		catch (const std::exception& error)
		{
			++failures;
			std::cerr << "FAIL " << name << ": " << error.what() << '\n';
		}
	}
	std::cout << names.size() - failures << '/' << names.size() << " application fixtures passed\n";
	return failures ? 1 : 0;
}
