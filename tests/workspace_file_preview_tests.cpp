#include "test_harness.h"
#include "common/paths/workspace_file_reader.h"

using namespace uam_test;

UAM_TEST(WorkspaceFilePreviewResolvesUnicodeAndSpacePathsWithinTheSelectedRoot)
{
	TempDir temp("uam-file-preview");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace / "src");
	const fs::path file = workspace / uam::paths::PathFromUtf8("src/café file.txt");
	UAM_ASSERT(uam::io::WriteTextFile(file, "first\nsecond\n"));
	const uam::workspace_files::TextFile relative = uam::workspace_files::Read(workspace, "src/café file.txt");
	UAM_ASSERT(relative.ok);
	UAM_ASSERT_EQ(relative.text, std::string("first\nsecond\n"));
	UAM_ASSERT_EQ(relative.path, uam::paths::Utf8PathString(fs::path("src") / uam::paths::PathFromUtf8("café file.txt")));
	UAM_ASSERT(uam::workspace_files::Read(workspace, uam::paths::Utf8PathString(file)).ok);
	UAM_ASSERT(uam::io::WriteTextFile(file, ""));
	UAM_ASSERT(uam::workspace_files::Read(workspace, "src/café file.txt").ok);
}

UAM_TEST(WorkspaceFilePreviewRejectsMissingBinaryInvalidAndOversizedFiles)
{
	TempDir temp("uam-file-preview-invalid");
	const fs::path file = temp.root / "file.txt";
	UAM_ASSERT(!uam::workspace_files::Read(temp.root, "missing.txt").ok);
	UAM_ASSERT(!uam::workspace_files::Read(temp.root, ".").ok);
	UAM_ASSERT(!uam::workspace_files::Read({}, "file.txt").ok);
	UAM_ASSERT(!uam::workspace_files::Read(uam::paths::PathFromUtf8(uam::paths::Utf8PathString(temp.root) + std::string("\0extra", 6)), "file.txt").ok);
	UAM_ASSERT(!uam::workspace_files::Read(temp.root, std::string("file\0.txt", 9)).ok);
	UAM_ASSERT(uam::io::WriteTextFile(file, std::string("bad\0text", 8)));
	UAM_ASSERT(!uam::workspace_files::Read(temp.root, "file.txt").ok);
	UAM_ASSERT(uam::io::WriteTextFile(file, std::string(1, static_cast<char>(0xff))));
	UAM_ASSERT(!uam::workspace_files::Read(temp.root, "file.txt").ok);
	UAM_ASSERT(uam::io::WriteTextFile(file, std::string(uam::workspace_files::kMaxPreviewBytes + 1, 'x')));
	UAM_ASSERT(!uam::workspace_files::Read(temp.root, "file.txt").ok);
	UAM_ASSERT(uam::io::WriteTextFile(file, std::string(uam::workspace_files::kMaxPreviewBytes, 'x')));
	UAM_ASSERT(uam::workspace_files::Read(temp.root, "file.txt").ok);
}

UAM_TEST(WorkspaceFilePreviewRejectsSiblingTraversalAndEscapingLinks)
{
	TempDir temp("uam-file-preview-scope");
	const fs::path workspace = temp.root / "project";
	const fs::path sibling = temp.root / "project-other";
	fs::create_directory(workspace); fs::create_directory(sibling);
	UAM_ASSERT(uam::io::WriteTextFile(sibling / "private.txt", "outside"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "inside.txt", "inside"));
	UAM_ASSERT(!uam::workspace_files::Read(workspace, "../project-other/private.txt").ok);
	UAM_ASSERT(!uam::workspace_files::Read(workspace, uam::paths::Utf8PathString(sibling / "private.txt")).ok);
#if !defined(_WIN32)
	fs::create_symlink(sibling / "private.txt", workspace / "outside-link.txt");
	fs::create_symlink(workspace / "inside.txt", workspace / "inside-link.txt");
	fs::create_directory_symlink(sibling, workspace / "outside-directory");
	UAM_ASSERT(!uam::workspace_files::Read(workspace, "outside-link.txt").ok);
	UAM_ASSERT(!uam::workspace_files::Read(workspace, "outside-directory/private.txt").ok);
	UAM_ASSERT(uam::workspace_files::Read(workspace, "inside-link.txt").ok);
	UAM_ASSERT(mkfifo((workspace / "pipe").c_str(), 0600) == 0);
	UAM_ASSERT(!uam::workspace_files::Read(workspace, "pipe").ok);
#endif
}
