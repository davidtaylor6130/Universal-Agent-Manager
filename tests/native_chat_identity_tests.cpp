#include "test_harness.h"

using namespace uam_test;

UAM_TEST(RemoteNativeHistoryIdentityUsesEffectiveWorktreeAndHost)
{
	ChatSession owned;
	owned.provider_id = "opencode-cli";
	owned.native_session_id = "same-native-session";
	owned.execution_host_id = "ssh-host";
	owned.workspace_directory = "/srv/main";
	owned.workspace_worktree_directory = "/srv/worktree";
	ChatSession native = owned;
	native.workspace_directory = "/srv/worktree";
	native.workspace_worktree_directory.clear();
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(owned), uam::chat_identity::NativeIdentityKeyForHistoryImport(native));
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForLocalDeduplication(owned), uam::chat_identity::NativeIdentityKeyForLocalDeduplication(native));
	ChatSession other = owned;
	other.execution_host_id = "another-host";
	UAM_ASSERT(uam::chat_identity::NativeIdentityKeyForHistoryImport(owned) != uam::chat_identity::NativeIdentityKeyForHistoryImport(other));
	other = owned;
	other.workspace_worktree_directory = "/srv/another-worktree";
	UAM_ASSERT(uam::chat_identity::NativeIdentityKeyForHistoryImport(owned) != uam::chat_identity::NativeIdentityKeyForHistoryImport(other));
	owned.workspace_directory = "C:\\Main";
	owned.workspace_worktree_directory = "C:\\Worktree";
	native.workspace_directory = "C:/Worktree";
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(owned), uam::chat_identity::NativeIdentityKeyForHistoryImport(native));
}
