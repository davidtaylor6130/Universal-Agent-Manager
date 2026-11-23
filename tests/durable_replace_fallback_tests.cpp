#include "data_folder_test_support.h"

#if !defined(_WIN32)
#include <sys/stat.h>

using namespace uam_test;

namespace
{
	/// Restores the calling thread's injected errnos and the process-wide swap capability cache.
	struct ReplaceFaults
	{
		ReplaceFaults() { Clear(); }
		~ReplaceFaults() { Clear(); }
		static void Clear()
		{
			uam::io::t_rename_swap_errno_for_tests = 0;
			uam::io::t_replace_link_errno_for_tests = 0;
			uam::io::t_replace_install_errno_for_tests = 0;
			uam::io::t_replace_install_drops_destination_for_tests = false;
			uam::io::ResetRenameSwapCapabilityCacheForTests();
		}
	};

	dev_t DeviceOf(const fs::path& path)
	{
		struct stat info{};
		UAM_ASSERT(lstat(path.c_str(), &info) == 0);
		return info.st_dev;
	}

	/// Names in a folder other than the expected durable files: a leaked kept predecessor or candidate shows up here.
	std::vector<std::string> Leftovers(const fs::path& folder, const std::vector<std::string>& expected)
	{
		std::vector<std::string> extra;
		for (const auto& entry : fs::directory_iterator(folder))
		{
			const std::string name = entry.path().filename().string();
			if (std::find(expected.begin(), expected.end(), name) == expected.end()) extra.push_back(name);
		}
		return extra;
	}
} // namespace

UAM_TEST(DurableReplaceFallsBackAndCachesVolumeWhenRenameSwapIsUnsupported)
{
	for (const int refused : {ENOTSUP, EINVAL, ENOTTY})
	{
		ReplaceFaults faults;
		TempDir temp("uam-durable-replace-fallback");
		const fs::path file = temp.root / "memory.md";
		UAM_ASSERT(uam::io::AtomicWriteFileDetailed(file, "first", {}, true).success);
		UAM_ASSERT(!uam::io::RenameSwapKnownUnsupported(DeviceOf(file)));

		uam::io::t_rename_swap_errno_for_tests = refused;
		const auto second = uam::io::AtomicWriteFileDetailed(file, "second", {}, true);
		UAM_ASSERT(second.success && second.primary_committed && !second.backup_degraded && second.replace_fallback);
		UAM_ASSERT(second.stage == uam::io::AtomicWriteStage::Complete && second.error.empty());
		UAM_ASSERT_EQ(uam::io::ReadTextFile(file), std::string("second"));
		UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::io::MakeBackupPath(file)), std::string("first"));
		UAM_ASSERT(uam::io::RenameSwapKnownUnsupported(DeviceOf(file)));
		UAM_ASSERT(Leftovers(temp.root, {"memory.md", "memory.md.bak"}).empty());

		// The cached refusal skips the exchange even once the injected refusal is gone.
		uam::io::t_rename_swap_errno_for_tests = 0;
		const auto third = uam::io::AtomicWriteFileDetailed(file, "third", {}, false);
		UAM_ASSERT(third.success && third.replace_fallback);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(file), std::string("third"));
		UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::io::MakeBackupPath(file)), std::string("first"));
		UAM_ASSERT(Leftovers(temp.root, {"memory.md", "memory.md.bak"}).empty());

		// A volume that supports the exchange keeps using it.
		uam::io::ResetRenameSwapCapabilityCacheForTests();
		const auto fourth = uam::io::AtomicWriteFileDetailed(file, "fourth", {}, true);
		UAM_ASSERT(fourth.success && !fourth.replace_fallback);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::io::MakeBackupPath(file)), std::string("third"));
	}
}

UAM_TEST(DurableReplaceFallbackCopiesPredecessorWhenHardLinksAreRefused)
{
	ReplaceFaults faults;
	TempDir temp("uam-durable-replace-copy");
	const fs::path file = temp.root / "AGENTS.md";
	UAM_ASSERT(uam::io::AtomicWriteFileDetailed(file, "instructions v1", {}, false).success);
	UAM_ASSERT(chmod(file.c_str(), 0640) == 0);
	uam::io::t_rename_swap_errno_for_tests = ENOTSUP;
	uam::io::t_replace_link_errno_for_tests = EPERM; // exFAT and SMB shares have no hard links.
	const auto replaced = uam::io::AtomicWriteFileDetailed(file, "instructions v2", {}, true);
	UAM_ASSERT(replaced.success && replaced.primary_committed && !replaced.backup_degraded && replaced.replace_fallback);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(file), std::string("instructions v2"));
	const fs::path backup = uam::io::MakeBackupPath(file);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(backup), std::string("instructions v1"));
	struct stat info{};
	UAM_ASSERT(stat(backup.c_str(), &info) == 0);
	UAM_ASSERT_EQ(static_cast<int>(info.st_mode & 07777), 0640);
	UAM_ASSERT(Leftovers(temp.root, {"AGENTS.md", "AGENTS.md.bak"}).empty());
}

UAM_TEST(DurableReplaceOtherSwapErrorsStillFailWithoutFallback)
{
	ReplaceFaults faults;
	TempDir temp("uam-durable-replace-refused");
	const fs::path file = temp.root / "settings.txt";
	UAM_ASSERT(uam::io::AtomicWriteFileDetailed(file, "kept", {}, false).success);
	uam::io::t_rename_swap_errno_for_tests = EACCES;
	const auto refused = uam::io::AtomicWriteFileDetailed(file, "lost", {}, true);
	UAM_ASSERT(!refused.success && !refused.primary_committed && !refused.replace_fallback);
	UAM_ASSERT(refused.error.find("Failed to atomically swap destination") == 0);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(file), std::string("kept"));
	UAM_ASSERT(!uam::io::RenameSwapKnownUnsupported(DeviceOf(file)));
	UAM_ASSERT(Leftovers(temp.root, {"settings.txt"}).empty());
}

UAM_TEST(DurableReplaceFallbackInstallFailureKeepsDestinationAndRemovesCopies)
{
	ReplaceFaults faults;
	TempDir temp("uam-durable-replace-install");
	const fs::path file = temp.root / "folders.txt";
	UAM_ASSERT(uam::io::AtomicWriteFileDetailed(file, "previous", {}, false).success);
	uam::io::t_rename_swap_errno_for_tests = EINVAL;
	for (const int link_error : {0, EPERM})
	{
		uam::io::t_replace_link_errno_for_tests = link_error;
		uam::io::t_replace_install_errno_for_tests = EIO;
		const auto failed = uam::io::AtomicWriteFileDetailed(file, "candidate", {}, true);
		UAM_ASSERT(!failed.success && !failed.primary_committed && failed.replace_fallback);
		UAM_ASSERT(failed.error.find("Failed to replace destination") == 0);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(file), std::string("previous"));
		UAM_ASSERT(Leftovers(temp.root, {"folders.txt"}).empty());
	}
}

UAM_TEST(DurableReplaceFallbackRestoresPredecessorWhenAFailedRenameDroppedTheName)
{
	ReplaceFaults faults;
	TempDir temp("uam-durable-replace-restore");
	const fs::path file = temp.root / "CLAUDE.md";
	UAM_ASSERT(uam::io::AtomicWriteFileDetailed(file, "previous", {}, false).success);
	uam::io::t_rename_swap_errno_for_tests = ENOTSUP;
	uam::io::t_replace_link_errno_for_tests = EPERM;
	uam::io::t_replace_install_errno_for_tests = EIO;
	uam::io::t_replace_install_drops_destination_for_tests = true;
	const auto failed = uam::io::AtomicWriteFileDetailed(file, "candidate", {}, true);
	UAM_ASSERT(!failed.success && !failed.primary_committed && failed.replace_fallback);
	UAM_ASSERT(failed.error.find("previous file was restored") != std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(file), std::string("previous"));
	UAM_ASSERT(Leftovers(temp.root, {"CLAUDE.md"}).empty());
}

UAM_TEST(DurableReplaceFallbackKeepsConditionalPredecessorEvidenceContract)
{
	for (const bool preserve_backup : {false, true})
	{
		ReplaceFaults faults;
		TempDir temp("uam-durable-replace-conditional");
		const fs::path file = temp.root / "chat.json";
		UAM_ASSERT(uam::io::AtomicWriteFileDetailed(file, "predecessor", {}, false).success);
		const fs::path candidate = uam::io::MakeTempWritePath(file);
		const fs::path evidence = temp.root / "chat.json.evidence";
		uam::io::AtomicWriteResult staged;
		UAM_ASSERT(uam::io::WriteAndSyncAtomicTemp(candidate, "candidate", {}, staged));
		uam::io::t_rename_swap_errno_for_tests = ENOTSUP;
		bool reached_cleanup = false;
		const auto committed = uam::io::CommitSyncedAtomicTemp(file, candidate, preserve_backup, [&](uam::io::AtomicWriteStage stage)
		{
			if (stage == uam::io::AtomicWriteStage::BeforeBackupCleanup)
			{
				// Exactly where the exchange leaves it: the predecessor sits at the candidate name, evidence stays reserved.
				reached_cleanup = true;
				UAM_ASSERT_EQ(uam::io::ReadTextFile(candidate), std::string("predecessor"));
				UAM_ASSERT(!fs::exists(evidence));
			}
			return true;
		}, true, true, evidence);
		UAM_ASSERT(reached_cleanup);
		UAM_ASSERT(committed.success && committed.primary_committed && committed.replace_fallback && committed.error.empty());
		UAM_ASSERT_EQ(uam::io::ReadTextFile(file), std::string("candidate"));
		UAM_ASSERT(!fs::exists(candidate) && !fs::exists(evidence));
		if (preserve_backup)
		{
			UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::io::MakeBackupPath(file)), std::string("predecessor"));
			UAM_ASSERT(Leftovers(temp.root, {"chat.json", "chat.json.bak"}).empty());
		}
		else
			UAM_ASSERT(Leftovers(temp.root, {"chat.json"}).empty());
	}
}
#endif
