#include "test_harness.h"
#include "app/svn_history.h"
#include "app/vcs_provider.h"
#include "common/platform/platform_services.h"

#include <algorithm>
#include <stop_token>

using namespace uam_test;

namespace
{
	std::vector<std::string> Revisions(const uam::VcsHistoryPage& page)
	{
		std::vector<std::string> revisions;
		for (const uam::VcsHistoryEntry& entry : page.entries) revisions.push_back(entry.revision);
		return revisions;
	}

	bool Has(const std::vector<std::string>& args, const std::string& value)
	{
		return std::ranges::find(args, value) != args.end();
	}

	ProcessExecutionResult Exit(const int code, std::string output = {})
	{
		ProcessExecutionResult result;
		result.ok = true;
		result.exit_code = code;
		result.output = std::move(output);
		return result;
	}

	std::string LogXml(const int newest, const int oldest)
	{
		std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<log>\n";
		for (int revision = newest; revision >= oldest; --revision)
			xml += "<logentry\n   revision=\"" + std::to_string(revision) + "\">\n<author>dev</author>\n<date>2026-01-02T03:04:05.000000Z</date>\n<msg>change " + std::to_string(revision) + "</msg>\n</logentry>\n";
		return xml + "</log>\n";
	}

	std::string InfoXml(const std::string& revision, const std::string& relative_url, const std::string& root = "https://svn.example.test/repo")
	{
		return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<info>\r\n<entry\r\n   path=\".\"\r\n   revision=\"" + revision + "\"\r\n   kind=\"dir\">\r\n<url>" + root + relative_url.substr(1) +
		       "</url>\r\n<relative-url>" + relative_url + "</relative-url>\r\n<repository>\r\n<root>" + root + "</root>\r\n<uuid>d7397596-24e3-4444-990a-c95097028d41</uuid>\r\n</repository>\r\n<commit\r\n   revision=\"3\">\r\n</commit>\r\n</entry>\r\n</info>\r\n";
	}

	/// Real-repository cases need both client tools; hosted CI runners have neither.
	bool SvnToolsAvailableForHistory()
	{
		if (RunTestCommand("svn --version --quiet") && RunTestCommand("svnadmin --version --quiet")) return true;
		std::fprintf(stderr, "SKIP real-svn history case: svn or svnadmin is not installed\n");
		return false;
	}

	void Svn(const fs::path& cwd, std::vector<std::string> args)
	{
		args.insert(args.begin() + 1, "--non-interactive");
		const ProcessExecutionResult result = uam::RunSvn(cwd, args);
		if (!result.ok || result.exit_code != 0) throw TestFailure("svn " + args.front() + " failed: " + result.output + result.error);
	}

	/// Disposable file:// repository with a trunk, one branch copied from it and a working copy switched to that branch.
	struct SvnHistoryRepository
	{
		TempDir temp{"uam-svn-history"};
		fs::path checkout = temp.root / "checkout";
		std::string url;
		uam::AppState app;
		ChatSession chat;
		static constexpr const char* kOddName = "we@ird %41.txt";

		SvnHistoryRepository()
		{
			const fs::path repository = temp.root / "repository";
			UAM_ASSERT(RunTestCommand("svnadmin create " + ShellQuoteForTest(uam::paths::Utf8PathString(repository))));
			const std::string generic = repository.generic_string();
			url = "file://" + std::string(generic.starts_with('/') ? "" : "/") + generic;
			Svn(temp.root, {"mkdir", url + "/trunk", url + "/branches", "-m", "layout"});
			Svn(temp.root, {"checkout", url + "/trunk", checkout.string()});
			UAM_ASSERT(uam::io::WriteTextFile(checkout / "a.txt", "one\n"));
			UAM_ASSERT(uam::io::WriteTextFile(checkout / kOddName, "x\n"));
			Svn(checkout, {"add", "--", "a.txt", std::string(kOddName) + "@"});
			// From a file: a multi-line message with quotes does not survive every platform's argument quoting.
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "message.txt", "add files\n\nwith <markup> & \"quotes\""));
			Svn(checkout, {"commit", "-F", (temp.root / "message.txt").string()});
			UAM_ASSERT(uam::io::WriteTextFile(checkout / "a.txt", "one\ntwo\n"));
			Svn(checkout, {"commit", "-m", "edit a"});
			Svn(checkout, {"delete", "--", "a.txt"});
			Svn(checkout, {"commit", "-m", "delete a"});
			Svn(temp.root, {"copy", url + "/trunk", url + "/branches/feat", "-m", "branch feat"});
			Svn(checkout, {"switch", url + "/branches/feat", "."});
			UAM_ASSERT(uam::io::WriteTextFile(checkout / kOddName, "x\ny\n"));
			Svn(checkout, {"commit", "-m", "branch edit"});
			Svn(checkout, {"update"});
			// Revision 7 exists only in the repository: the working copy stays at 6.
			Svn(temp.root, {"mkdir", url + "/branches/feat/incoming", "-m", "incoming folder"});
			chat.workspace_directory = checkout.string();
			app.settings.svn_enabled = true;
		}
	};
} // namespace

UAM_TEST(SvnLogXmlKeepsOnlyCompleteEntriesAndDecodesText)
{
	const std::string xml =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<log>\n"
	    "<logentry\n   revision=\"12\">\n<author>a &amp; b</author>\n<date>2026-01-02T03:04:05.000000Z</date>\n"
	    "<paths>\n<path\n   prop-mods=\"false\"\n   text-mods=\"false\"\n   kind=\"dir\"\n   copyfrom-path=\"/trunk\"\n   copyfrom-rev=\"9\"\n   action=\"A\">/branches/a&lt;b</path>\n"
	    "<path\n   action=\"M\"\n   kind=\"file\">/branches/a&lt;b/caf&#233;.txt</path>\n</paths>\n"
	    "<msg>first &lt;line&gt;&#10;second &quot;line&quot; &unknown; &#x1F600;</msg>\n</logentry>\n"
	    "<logentry\r\n   revision=\"11\">\r\n<date>2026-01-01T00:00:00.000000Z</date>\r\n<msg>windows\r\nline &#13;kept</msg>\r\n</logentry>\r\n"
	    "<logentry\n   revision=\"10\">\n<author>cut";
	const std::vector<uam::VcsHistoryEntry> entries = uam::svn_history::ParseLogXml(xml);
	UAM_ASSERT_EQ(entries.size(), std::size_t{2});
	UAM_ASSERT_EQ(entries[0].revision, std::string("12"));
	UAM_ASSERT_EQ(entries[0].author, std::string("a & b"));
	UAM_ASSERT_EQ(entries[0].date, std::string("2026-01-02T03:04:05.000000Z"));
	UAM_ASSERT_EQ(entries[0].message, std::string("first <line>\nsecond \"line\" &unknown; \xF0\x9F\x98\x80"));
	UAM_ASSERT_EQ(entries[0].paths.size(), std::size_t{2});
	UAM_ASSERT_EQ(entries[0].paths[0].action, std::string("A"));
	UAM_ASSERT_EQ(entries[0].paths[0].path, std::string("/branches/a<b"));
	UAM_ASSERT_EQ(entries[0].paths[0].kind, std::string("dir"));
	UAM_ASSERT_EQ(entries[0].paths[0].copy_from_path, std::string("/trunk"));
	UAM_ASSERT_EQ(entries[0].paths[0].copy_from_revision, std::string("9"));
	UAM_ASSERT_EQ(entries[0].paths[1].path, std::string("/branches/a<b/caf\xC3\xA9.txt"));
	UAM_ASSERT(entries[0].paths[1].copy_from_path.empty());
	UAM_ASSERT(entries[1].author.empty());
	// Text-mode CRLF is the stream's, not the message's; a carriage return svn escaped is.
	UAM_ASSERT_EQ(entries[1].message, std::string("windows\nline \rkept"));
	UAM_ASSERT(entries[1].paths.empty());

	const std::vector<uam::VcsHistoryEntry> capped = uam::svn_history::ParseLogXml(xml, 1);
	UAM_ASSERT_EQ(capped[0].paths.size(), std::size_t{1});
	UAM_ASSERT_EQ(capped[0].paths_omitted, std::size_t{1});
}

UAM_TEST(SvnLogPagesWithOneExtraEntryAndFollowsTheCursor)
{
	std::vector<std::vector<std::string>> calls;
	const uam::VcsCommandRunner svn = [&](const std::vector<std::string>& args, int)
	{
		calls.push_back(args);
		if (args.front() == "info") return Exit(0, InfoXml("8", "^/project%20a/trunk"));
		return Exit(0, Has(args, "HEAD:0") ? LogXml(9, 7) : LogXml(6, 6));
	};
	uam::VcsHistoryRequest request;
	request.limit = 2;
	const uam::VcsHistoryPage first = uam::svn_history::Log(svn, request);
	UAM_ASSERT(first.error.empty());
	UAM_ASSERT(Revisions(first) == std::vector<std::string>({"9", "8"}));
	UAM_ASSERT_EQ(first.next, std::string("7"));
	UAM_ASSERT_EQ(first.working_revision, std::string("8"));
	// The location is the repository path itself, not its URL-encoded form.
	UAM_ASSERT_EQ(first.location, std::string("^/project a/trunk"));
	UAM_ASSERT_EQ(first.repository_root, std::string("https://svn.example.test/repo"));
	UAM_ASSERT_EQ(first.repository_uuid, std::string("d7397596-24e3-4444-990a-c95097028d41"));
	UAM_ASSERT(calls[0] == std::vector<std::string>({"log", "--xml", "--non-interactive", "--limit", "3", "--revision", "HEAD:0", "--verbose", "--", "."}));

	calls.clear();
	request.before = first.next;
	request.search = "  fix  ";
	const uam::VcsHistoryPage second = uam::svn_history::Log(svn, request);
	UAM_ASSERT(Revisions(second) == std::vector<std::string>({"6"}));
	UAM_ASSERT(second.next.empty());
	UAM_ASSERT(Has(calls[0], "7:0"));
	UAM_ASSERT(Has(calls[0], "--search") && Has(calls[0], "fix"));
	// Only the newest page asks where the working copy is.
	UAM_ASSERT_EQ(calls.size(), std::size_t{1});

	request.before = "HEAD; rm";
	UAM_ASSERT(!uam::svn_history::Log(svn, request).error.empty());
	request.before = "-3";
	UAM_ASSERT(!uam::svn_history::Log(svn, request).error.empty());
}

UAM_TEST(SvnLogFallsBackToTheWorkingCopyRevisionAndSurvivesOversizedOutput)
{
	// The branch no longer exists at HEAD: history is read from the working copy's own revision.
	const uam::VcsCommandRunner moved = [&](const std::vector<std::string>& args, int)
	{
		if (args.front() == "info") return Exit(0, "5\n");
		return Has(args, "HEAD:0") ? Exit(1, "svn: E160013: path not found\n") : Exit(0, LogXml(5, 4));
	};
	const uam::VcsHistoryPage fallback = uam::svn_history::Log(moved, {});
	UAM_ASSERT(fallback.error.empty());
	UAM_ASSERT(Revisions(fallback) == std::vector<std::string>({"5", "4"}));

	// Output cut at the capture limit keeps the whole entries and continues after them.
	const uam::VcsCommandRunner cut = [&](const std::vector<std::string>& args, int)
	{
		if (args.front() == "info") return Exit(0, "9\n");
		ProcessExecutionResult result = Exit(0, LogXml(9, 8) + "<logentry\n   revision=\"7\">\n<author>partial");
		result.output_truncated = true;
		return result;
	};
	const uam::VcsHistoryPage partial = uam::svn_history::Log(cut, {});
	UAM_ASSERT(Revisions(partial) == std::vector<std::string>({"9", "8"}));
	UAM_ASSERT_EQ(partial.next, std::string("7"));

	// One revision too large to list with its paths is listed without them.
	const uam::VcsCommandRunner huge = [&](const std::vector<std::string>& args, int)
	{
		if (args.front() == "info") return Exit(0, "9\n");
		if (!Has(args, "--verbose")) return Exit(0, LogXml(9, 8));
		ProcessExecutionResult result = Exit(-1);
		result.ok = false;
		result.error = std::string(uam::platform::kCapturedCommandOutputLimitError);
		return result;
	};
	const uam::VcsHistoryPage brief = uam::svn_history::Log(huge, {});
	UAM_ASSERT(brief.error.empty());
	UAM_ASSERT(Revisions(brief) == std::vector<std::string>({"9"}));
	UAM_ASSERT_EQ(brief.entries[0].paths_omitted, std::size_t{1});
	UAM_ASSERT_EQ(brief.next, std::string("8"));

	const uam::VcsCommandRunner broken = [](const std::vector<std::string>&, int) { return Exit(1, "svn: E155007: '/tmp/x' is not a working copy\n"); };
	UAM_ASSERT(uam::svn_history::Log(broken, {}).error.find("E155007") != std::string::npos);
}

UAM_TEST(SvnRevisionDiffEncodesThePathAndRetriesBeforeADeletion)
{
	std::vector<std::string> targets;
	const uam::VcsCommandRunner svn = [&](const std::vector<std::string>& args, int)
	{
		UAM_ASSERT(args[0] == "diff" && Has(args, "--internal-diff") && Has(args, "--non-interactive"));
		UAM_ASSERT(args[args.size() - 2] == "--");
		targets.push_back(args.back());
		return args.back().ends_with("@41") ? Exit(0, "Index: gone.txt\n-old\n") : Exit(1, "svn: E160013: not found\n");
	};
	std::string error;
	const std::string patch = uam::svn_history::RevisionDiff(svn, "42", "/trunk/a b/we@ird %41 \xC3\xA9.txt", error);
	UAM_ASSERT(error.empty());
	UAM_ASSERT(patch.find("-old") != std::string::npos);
	UAM_ASSERT(targets == std::vector<std::string>({"^/trunk/a%20b/we%40ird%20%2541%20%C3%A9.txt@42", "^/trunk/a%20b/we%40ird%20%2541%20%C3%A9.txt@41"}));

	// Nothing reaches svn unless the revision is a number and the path is a repository path.
	targets.clear();
	for (const auto& [revision, path] : std::vector<std::pair<std::string, std::string>>{{"HEAD", "/trunk/a"}, {"0", "/trunk/a"}, {"4 --old", "/trunk/a"}, {"4", "trunk/a"}, {"4", "--config-dir"}, {"4", "/"}, {"4", "/trunk/a\nb"}, {"4", ""}})
	{
		error.clear();
		UAM_ASSERT(uam::svn_history::RevisionDiff(svn, revision, path, error).empty());
		UAM_ASSERT(!error.empty());
	}
	UAM_ASSERT(targets.empty());

	const uam::VcsCommandRunner large = [](const std::vector<std::string>&, int)
	{
		ProcessExecutionResult result = Exit(0, "Index: big\n");
		result.output_truncated = true;
		return result;
	};
	error.clear();
	UAM_ASSERT(uam::svn_history::RevisionDiff(large, "4", "/trunk/big", error).empty());
	UAM_ASSERT(error.find("too large") != std::string::npos);
}

UAM_TEST(SvnUpdatePostponesConflictsAndReportsThem)
{
	std::vector<std::string> update_args;
	const uam::VcsCommandRunner svn = [&](const std::vector<std::string>& args, int)
	{
		if (args.front() == "info") return Exit(0, InfoXml("14", "^/trunk"));
		update_args = args;
		return Exit(0, "Updating '.':\nC    a.txt\nUpdated to revision 14.\nSummary of conflicts:\n  Text conflicts: 1\n");
	};
	const uam::VcsUpdateResult update = uam::svn_history::Update(svn);
	UAM_ASSERT(update.ok);
	UAM_ASSERT(update.conflicts);
	UAM_ASSERT_EQ(update.revision, std::string("14"));
	UAM_ASSERT(update_args == std::vector<std::string>({"update", "--non-interactive", "--accept", "postpone", "--", "."}));

	// Conflicts are read from the status columns, so a translated client reports them too; ordinary lines do not.
	for (const auto& [output, conflicted] : std::vector<std::pair<std::string, bool>>{
	         {"Aktualisiere \xC2\xBB.\xC2\xAB:\r\nC    a.txt\r\nAktualisiert zu Revision 14.\r\nKonflikt\xC3\xBC" "bersicht:\r\n", true},
	         {"Updating '.':\n C   dir\nUpdated to revision 14.\n", true},
	         {"Updating '.':\n   C tree\n", true},
	         {"Updating '.':\nU    Calc.cpp\nA    C\nUpdated to revision 14.\n", false},
	         {"Updating '.':\nAt revision 14.\n", false}})
	{
		const uam::VcsCommandRunner translated = [&](const std::vector<std::string>& args, int) { return Exit(0, args.front() == "info" ? InfoXml("14", "^/trunk") : output); };
		UAM_ASSERT_EQ(uam::svn_history::Update(translated).conflicts, conflicted);
	}

	const uam::VcsCommandRunner locked = [](const std::vector<std::string>&, int) { return Exit(1, "svn: E155004: Working copy '/x' locked.\n"); };
	const uam::VcsUpdateResult failed = uam::svn_history::Update(locked);
	UAM_ASSERT(!failed.ok);
	UAM_ASSERT(failed.error.find("E155004") != std::string::npos);
}

UAM_TEST(VcsHistoryAndUpdateAreDeclinedForSystemsThatDoNotDeclareThem)
{
	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = fs::temp_directory_path().string();
	const uam::VcsCommitService vcs;
	app.settings.git_enabled = true;
	UAM_ASSERT(vcs.History(app, chat, uam::VcsType::Git, {}).error.find("Git history is not available") != std::string::npos);
	UAM_ASSERT(vcs.Update(app, chat, uam::VcsType::Git).error.find("Git update is not available") != std::string::npos);
	std::string error;
	(void)vcs.RevisionDiff(app, chat, uam::VcsType::Git, "1", "/a", &error);
	UAM_ASSERT(error.find("Git history is not available") != std::string::npos);

	app.settings.svn_enabled = false;
	UAM_ASSERT(vcs.History(app, chat, uam::VcsType::Svn, {}).error.find("disabled") != std::string::npos);
	UAM_ASSERT(vcs.Update(app, chat, uam::VcsType::Svn).error.find("disabled") != std::string::npos);

	const auto declares = [](const uam::VcsType type, const uam::vcs::VcsCapability capability)
	{
		return uam::ranges::Contains(uam::VcsProviderFor(type).Declaration().capabilities, capability);
	};
	UAM_ASSERT(declares(uam::VcsType::Svn, uam::vcs::VcsCapability::History) && declares(uam::VcsType::Svn, uam::vcs::VcsCapability::Update));
	UAM_ASSERT(!declares(uam::VcsType::Git, uam::vcs::VcsCapability::History) && !declares(uam::VcsType::Git, uam::vcs::VcsCapability::Update));
	UAM_ASSERT_EQ(std::string(uam::vcs::VcsCapabilityId(uam::vcs::VcsCapability::History)), std::string("history"));
	UAM_ASSERT_EQ(std::string(uam::vcs::VcsCapabilityId(uam::vcs::VcsCapability::Update)), std::string("update"));
}

UAM_TEST(SvnHistoryPagesARealRepositoryAcrossItsBranchPoint)
{
	if (!SvnToolsAvailableForHistory()) return;
	SvnHistoryRepository repository;
	const uam::VcsCommitService vcs;

	uam::VcsHistoryRequest request;
	request.limit = 3;
	const uam::VcsHistoryPage first = vcs.History(repository.app, repository.chat, uam::VcsType::Svn, request);
	if (!first.error.empty()) throw TestFailure(first.error);
	// Revision 7 is newer than the working copy and still listed.
	UAM_ASSERT(Revisions(first) == std::vector<std::string>({"7", "6", "5"}));
	UAM_ASSERT_EQ(first.working_revision, std::string("6"));
	UAM_ASSERT_EQ(first.location, std::string("^/branches/feat"));
	UAM_ASSERT_EQ(first.next, std::string("4"));
	UAM_ASSERT_EQ(first.entries[1].message, std::string("branch edit"));
	UAM_ASSERT(!first.entries[1].author.empty() && first.entries[1].date.size() >= 20);
	UAM_ASSERT_EQ(first.entries[1].paths.size(), std::size_t{1});
	UAM_ASSERT_EQ(first.entries[1].paths[0].path, std::string("/branches/feat/") + SvnHistoryRepository::kOddName);
	UAM_ASSERT_EQ(first.entries[1].paths[0].action, std::string("M"));
	const uam::VcsHistoryPath& branch_point = first.entries[2].paths.at(0);
	UAM_ASSERT_EQ(branch_point.path, std::string("/branches/feat"));
	UAM_ASSERT_EQ(branch_point.copy_from_path, std::string("/trunk"));
	UAM_ASSERT_EQ(branch_point.copy_from_revision, std::string("4"));

	// History continues on trunk before the copy.
	request.before = first.next;
	const uam::VcsHistoryPage second = vcs.History(repository.app, repository.chat, uam::VcsType::Svn, request);
	UAM_ASSERT(Revisions(second) == std::vector<std::string>({"4", "3", "2"}));
	UAM_ASSERT_EQ(second.next, std::string("1"));
	UAM_ASSERT(second.working_revision.empty());
	UAM_ASSERT_EQ(second.entries[2].message, std::string("add files\n\nwith <markup> & \"quotes\""));
	request.before = second.next;
	const uam::VcsHistoryPage last = vcs.History(repository.app, repository.chat, uam::VcsType::Svn, request);
	UAM_ASSERT(Revisions(last) == std::vector<std::string>({"1"}));
	UAM_ASSERT(last.next.empty());

	request.before.clear();
	request.search = "delete";
	UAM_ASSERT(Revisions(vcs.History(repository.app, repository.chat, uam::VcsType::Svn, request)) == std::vector<std::string>({"4"}));

	std::stop_source stopped;
	stopped.request_stop();
	UAM_ASSERT(!vcs.History(repository.app, repository.chat, uam::VcsType::Svn, {}, stopped.get_token()).error.empty());
}

UAM_TEST(SvnRevisionDiffShowsEditsAdditionsDeletionsAndOddNames)
{
	if (!SvnToolsAvailableForHistory()) return;
	SvnHistoryRepository repository;
	const uam::VcsCommitService vcs;
	const auto diff = [&](const std::string& revision, const std::string& path)
	{
		std::string error;
		const std::string patch = vcs.RevisionDiff(repository.app, repository.chat, uam::VcsType::Svn, revision, path, &error);
		if (!error.empty()) throw TestFailure(error);
		return patch;
	};
	UAM_ASSERT(diff("2", "/trunk/a.txt").find("+one") != std::string::npos);
	UAM_ASSERT(diff("3", "/trunk/a.txt").find("+two") != std::string::npos);
	const std::string deleted = diff("4", "/trunk/a.txt");
	UAM_ASSERT(deleted.find("-one") != std::string::npos && deleted.find("-two") != std::string::npos);
	UAM_ASSERT(diff("6", std::string("/branches/feat/") + SvnHistoryRepository::kOddName).find("+y") != std::string::npos);
	// A copied folder has no text of its own to show.
	UAM_ASSERT(diff("5", "/branches/feat").find("@@") == std::string::npos);

	std::string error;
	UAM_ASSERT(vcs.RevisionDiff(repository.app, repository.chat, uam::VcsType::Svn, "3", "/trunk/never-existed.txt", &error).empty());
	UAM_ASSERT(!error.empty());
}

UAM_TEST(SvnUpdateBringsInNewRevisionsAndPostponesAConflict)
{
	if (!SvnToolsAvailableForHistory()) return;
	SvnHistoryRepository repository;
	const uam::VcsCommitService vcs;
	const uam::VcsUpdateResult clean = vcs.Update(repository.app, repository.chat, uam::VcsType::Svn);
	if (!clean.ok) throw TestFailure(clean.error);
	UAM_ASSERT(!clean.conflicts);
	UAM_ASSERT_EQ(clean.revision, std::string("7"));
	UAM_ASSERT(fs::is_directory(repository.checkout / "incoming"));

	// Another working copy commits to the same line that this one edits locally.
	const fs::path other = repository.temp.root / "other";
	Svn(repository.temp.root, {"checkout", repository.url + "/branches/feat", other.string()});
	UAM_ASSERT(uam::io::WriteTextFile(other / SvnHistoryRepository::kOddName, "theirs\n"));
	Svn(other, {"commit", "-m", "their edit"});
	UAM_ASSERT(uam::io::WriteTextFile(repository.checkout / SvnHistoryRepository::kOddName, "mine\n"));
	const uam::VcsUpdateResult conflicted = vcs.Update(repository.app, repository.chat, uam::VcsType::Svn);
	if (!conflicted.ok) throw TestFailure(conflicted.error);
	UAM_ASSERT(conflicted.conflicts);
	UAM_ASSERT_EQ(conflicted.revision, std::string("8"));
	// The local edit is kept for the user to resolve; nothing was committed or discarded.
	UAM_ASSERT(uam::io::ReadTextFile(repository.checkout / SvnHistoryRepository::kOddName).find("mine") != std::string::npos);
	const uam::VcsCommitStatus status = vcs.Status(repository.app, repository.chat, uam::VcsType::Svn, false);
	UAM_ASSERT(std::ranges::any_of(status.changed_files, [](const uam::VcsChangedFile& file) { return file.status.starts_with("C"); }));
}

namespace
{
	/// Scripted svn for the origin comparison: a branch copied in r20 from `/trunk a@19`, with switches for what fails.
	struct OriginScript
	{
		std::string relative_url = "^/branches/feat%20x";
		bool copied = true;
		bool mergeinfo_works = true;
		bool origin_exists = true;
		std::string recorded_merges;
		std::vector<std::vector<std::string>> calls;

		uam::VcsCommandRunner Runner()
		{
			return [this](const std::vector<std::string>& args, int)
			{
				calls.push_back(args);
				if (args.front() == "info") return Exit(0, InfoXml("31", relative_url));
				if (args.front() == "log" && Has(args, "0:HEAD"))
					return Exit(0, copied
					    ? "<log>\n<logentry\n   revision=\"20\">\n<author>dev</author>\n<paths>\n<path\n   kind=\"dir\"\n   copyfrom-path=\"/trunk a\"\n   copyfrom-rev=\"19\"\n   action=\"A\">/branches/feat x</path>\n</paths>\n<msg>branch</msg>\n</logentry>\n</log>\n"
					    : "<log>\n<logentry\n   revision=\"1\">\n<paths>\n<path\n   kind=\"dir\"\n   action=\"A\">/trunk a</path>\n</paths>\n<msg>start</msg>\n</logentry>\n</log>\n");
				if (args.front() == "mergeinfo") return mergeinfo_works ? Exit(0, "r24\r\nr27*\r\nr22\r\n") : Exit(1, "svn: E195016: merge tracking not supported\n");
				if (args.front() == "log" && Has(args, "--quiet")) return origin_exists ? Exit(0, LogXml(27, 21)) : Exit(1, "svn: E160013: '/trunk a' path not found\n");
				if (args.front() == "propget") return recorded_merges.empty() ? Exit(1, "svn: warning: W200017: Property 'svn:mergeinfo' not found\n") : Exit(0, recorded_merges);
				if (args.front() == "log" && Has(args, "HEAD:0")) return Exit(0, LogXml(31, 29) + LogXml(20, 18));
				if (args.front() == "log")
				{
					// One `--revision N` per requested revision, answered in the order asked.
					std::string xml;
					for (std::size_t index = 0; index + 1 < args.size(); ++index)
						if (args[index] == "--revision") xml += LogXml(std::stoi(args[index + 1]), std::stoi(args[index + 1]));
					return Exit(0, xml);
				}
				return Exit(1, "svn: E000000: unexpected\n");
			};
		}
	};
} // namespace

UAM_TEST(SvnOriginSummaryFindsTheCopyPointAndTheOriginsRevisions)
{
	OriginScript script;
	script.recorded_merges = "/trunk a:20-23\n";
	const uam::VcsOriginReply summary = uam::svn_history::Origin(script.Runner(), {});
	UAM_ASSERT(summary.error.empty());
	UAM_ASSERT_EQ(summary.branch_path, std::string("/branches/feat x"));
	UAM_ASSERT_EQ(summary.origin_path, std::string("/trunk a"));
	UAM_ASSERT_EQ(summary.copy_revision, std::string("20"));
	UAM_ASSERT_EQ(summary.origin_revision, std::string("19"));
	UAM_ASSERT(summary.behind_revisions == std::vector<std::string>({"27", "24", "22"}));
	UAM_ASSERT(summary.merges_recorded && !summary.behind_from_log);
	// Records the branch inherited from its origin say nothing about merges from the origin.
	OriginScript inherited;
	inherited.recorded_merges = "/branches/other:3-8\n/trunk another:2-9\n";
	UAM_ASSERT(!uam::svn_history::Origin(inherited.Runner(), {}).merges_recorded);
	const auto mergeinfo = std::ranges::find_if(script.calls, [](const std::vector<std::string>& args) { return args.front() == "mergeinfo"; });
	UAM_ASSERT(*mergeinfo == std::vector<std::string>({"mergeinfo", "--show-revs", "eligible", "--non-interactive", "--", "^/trunk%20a@HEAD", "^/branches/feat%20x@HEAD"}));

	// A working copy of a folder inside the branch is compared with the same folder inside the origin.
	OriginScript inside;
	inside.relative_url = "^/branches/feat%20x/src/app";
	const uam::VcsOriginReply nested = uam::svn_history::Origin(inside.Runner(), {});
	UAM_ASSERT_EQ(nested.origin_path, std::string("/trunk a/src/app"));
	UAM_ASSERT_EQ(nested.branch_path, std::string("/branches/feat x/src/app"));

	// A main line has no origin; that is an answer, not an error, and nothing else is asked of svn.
	OriginScript trunk;
	trunk.relative_url = "^/trunk%20a";
	trunk.copied = false;
	const uam::VcsOriginReply none = uam::svn_history::Origin(trunk.Runner(), {});
	UAM_ASSERT(none.error.empty() && none.origin_path.empty() && none.behind_revisions.empty());
	UAM_ASSERT_EQ(trunk.calls.size(), std::size_t{2});

	// Its own revisions since the copy come from its log, which carries on into the origin: the page ends at the copy.
	OriginScript own;
	uam::VcsOriginRequest ahead;
	ahead.view = "ahead";
	const uam::VcsOriginReply page = uam::svn_history::Origin(own.Runner(), ahead);
	UAM_ASSERT(Revisions(page.page) == std::vector<std::string>({"31", "30", "29"}));
	UAM_ASSERT(page.page.next.empty());
}

UAM_TEST(SvnOriginBehindFallsBackToTheOriginLogWithoutMergeTracking)
{
	OriginScript script;
	script.mergeinfo_works = false;
	const uam::VcsOriginReply summary = uam::svn_history::Origin(script.Runner(), {});
	UAM_ASSERT(summary.error.empty());
	UAM_ASSERT(summary.behind_from_log && !summary.merges_recorded);
	// Every origin revision after the one that was copied.
	UAM_ASSERT_EQ(summary.behind_revisions.size(), std::size_t{7});
	UAM_ASSERT_EQ(summary.behind_revisions.front(), std::string("27"));
	const auto fallback = std::ranges::find_if(script.calls, [](const std::vector<std::string>& args) { return args.front() == "log" && Has(args, "--quiet") && args.back() == "^/trunk%20a@HEAD"; });
	UAM_ASSERT(fallback != script.calls.end() && Has(*fallback, "HEAD:20"));

	// The origin was deleted: the copy point is still known and the origin's side is simply empty.
	OriginScript gone;
	gone.mergeinfo_works = false;
	gone.origin_exists = false;
	const uam::VcsOriginReply partial = uam::svn_history::Origin(gone.Runner(), {});
	UAM_ASSERT(partial.error.empty());
	UAM_ASSERT_EQ(partial.copy_revision, std::string("20"));
	UAM_ASSERT(partial.behind_revisions.empty() && !partial.behind_from_log);
}

UAM_TEST(SvnOriginBehindPageKeepsOnlyTheRequestedRevisions)
{
	OriginScript script;
	uam::VcsOriginRequest request;
	request.view = "behind";
	request.revisions = {"22", "27", "24"};
	const uam::VcsOriginReply page = uam::svn_history::Origin(script.Runner(), request);
	UAM_ASSERT(page.error.empty());
	UAM_ASSERT(Revisions(page.page) == std::vector<std::string>({"27", "24", "22"}));
	// Each revision is asked for by itself, so nothing between them is read.
	UAM_ASSERT(script.calls.back() == std::vector<std::string>({"log", "--xml", "--verbose", "--non-interactive", "--revision", "22", "--revision", "27", "--revision", "24", "--", "^/trunk%20a@HEAD"}));

	for (const std::string& bad : {std::string("HEAD"), std::string("27 --revprop"), std::string("-1")})
	{
		OriginScript refused;
		request.revisions = {"27", bad};
		UAM_ASSERT(!uam::svn_history::Origin(refused.Runner(), request).error.empty());
		// Refused before svn is asked anything.
		UAM_ASSERT(refused.calls.empty());
	}
}

UAM_TEST(SvnOriginComparesARealBranchWithItsTrunk)
{
	if (!SvnToolsAvailableForHistory()) return;
	SvnHistoryRepository repository;
	const uam::VcsCommitService vcs;
	// Trunk moves on after the copy.
	const fs::path trunk = repository.temp.root / "trunk";
	Svn(repository.temp.root, {"checkout", repository.url + "/trunk", trunk.string()});
	UAM_ASSERT(uam::io::WriteTextFile(trunk / SvnHistoryRepository::kOddName, "x\ntrunk\n"));
	Svn(trunk, {"commit", "-m", "trunk edit"});
	UAM_ASSERT(uam::io::WriteTextFile(trunk / "only on trunk.txt", "t\n"));
	Svn(trunk, {"add", "--", "only on trunk.txt"});
	Svn(trunk, {"commit", "-m", "trunk add"});
	UAM_ASSERT(uam::io::WriteTextFile(repository.checkout / "only on branch.txt", "b1\nb2"));
	Svn(repository.checkout, {"add", "--", "only on branch.txt"});
	Svn(repository.checkout, {"update"});
	Svn(repository.checkout, {"commit", "-m", "branch add"});

	const auto origin = [&](uam::VcsOriginRequest request)
	{
		const uam::VcsOriginReply reply = vcs.Origin(repository.app, repository.chat, uam::VcsType::Svn, request);
		if (!reply.error.empty()) throw TestFailure(request.view + ": " + reply.error);
		return reply;
	};
	const uam::VcsOriginReply summary = origin({});
	UAM_ASSERT_EQ(summary.origin_path, std::string("/trunk"));
	UAM_ASSERT_EQ(summary.branch_path, std::string("/branches/feat"));
	UAM_ASSERT_EQ(summary.copy_revision, std::string("5"));
	UAM_ASSERT_EQ(summary.origin_revision, std::string("4"));
	UAM_ASSERT(summary.behind_revisions == std::vector<std::string>({"9", "8"}));
	UAM_ASSERT(!summary.merges_recorded && !summary.behind_from_log);

	uam::VcsOriginRequest ahead;
	ahead.view = "ahead";
	ahead.limit = 2;
	const uam::VcsOriginReply first = origin(ahead);
	UAM_ASSERT(Revisions(first.page) == std::vector<std::string>({"10", "7"}));
	ahead.before = first.page.next;
	// The copy revision ends the branch's own history and is not listed.
	UAM_ASSERT(Revisions(origin(ahead).page) == std::vector<std::string>({"6"}));

	uam::VcsOriginRequest behind;
	behind.view = "behind";
	behind.revisions = summary.behind_revisions;
	const uam::VcsOriginReply missing = origin(behind);
	UAM_ASSERT(Revisions(missing.page) == std::vector<std::string>({"9", "8"}));
	UAM_ASSERT_EQ(missing.page.entries[0].message, std::string("trunk add"));
	UAM_ASSERT_EQ(missing.page.entries[0].paths.at(0).path, std::string("/trunk/only on trunk.txt"));

	// Recording a merge of r8 takes it out of what the branch is missing.
	UAM_ASSERT(uam::io::WriteTextFile(repository.checkout / SvnHistoryRepository::kOddName, "x\ny\n"));
	Svn(repository.checkout, {"update"});
	Svn(repository.checkout, {"merge", "--record-only", "-c", "8", repository.url + "/trunk", "."});
	Svn(repository.checkout, {"commit", "-m", "record trunk r8"});
	const uam::VcsOriginReply merged = origin({});
	UAM_ASSERT(merged.merges_recorded);
	UAM_ASSERT(merged.behind_revisions == std::vector<std::string>({"9"}));

	// Renaming the branch keeps its origin: the comparison follows the rename back to the copy from trunk.
	Svn(repository.temp.root, {"move", repository.url + "/branches/feat", repository.url + "/branches/feat renamed", "-m", "rename branch"});
	Svn(repository.checkout, {"switch", "--ignore-ancestry", repository.url + "/branches/feat renamed", "."});
	const uam::VcsOriginReply renamed = origin({});
	UAM_ASSERT_EQ(renamed.branch_path, std::string("/branches/feat renamed"));
	UAM_ASSERT_EQ(renamed.origin_path, std::string("/trunk"));
	UAM_ASSERT_EQ(renamed.copy_revision, std::string("5"));
	UAM_ASSERT(renamed.behind_revisions == std::vector<std::string>({"9"}));
	uam::VcsOriginRequest renamed_ahead;
	renamed_ahead.view = "ahead";
	UAM_ASSERT(Revisions(origin(renamed_ahead).page) == std::vector<std::string>({"12", "11", "10", "7", "6"}));

	// Trunk itself was not copied from anywhere.
	ChatSession trunk_chat;
	trunk_chat.workspace_directory = trunk.string();
	const uam::VcsOriginReply main_line = vcs.Origin(repository.app, trunk_chat, uam::VcsType::Svn, {});
	UAM_ASSERT(main_line.error.empty() && main_line.origin_path.empty());
	UAM_ASSERT(vcs.Origin(repository.app, repository.chat, uam::VcsType::Git, {}).error.find("Git origin comparison is not available") != std::string::npos);
}

UAM_TEST(SvnOriginTreatsALineRestoredFromItsOwnPastAsAMainLine)
{
	// trunk deleted, then restored with `svn cp ^/trunk@9 ^/trunk`: the copy's source is the line itself.
	uam::VcsCommandRunner svn = [](const std::vector<std::string>& args, int)
	{
		if (args.front() == "info") return Exit(0, InfoXml("12", "^/trunk"));
		return Exit(0, "<log>\n<logentry\n   revision=\"11\">\n<paths>\n<path\n   kind=\"dir\"\n   copyfrom-path=\"/trunk\"\n   copyfrom-rev=\"9\"\n   action=\"A\">/trunk</path>\n</paths>\n<msg>restore</msg>\n</logentry>\n</log>\n");
	};
	const uam::VcsOriginReply reply = uam::svn_history::Origin(svn, {});
	UAM_ASSERT(reply.error.empty());
	UAM_ASSERT(reply.origin_path.empty());

	uam::VcsOriginRequest unknown;
	unknown.view = "changes";
	UAM_ASSERT(!uam::svn_history::Origin(svn, unknown).error.empty());
}

UAM_TEST(DirectoriesOverlapComparesPathsAsTheirHostWould)
{
	using uam::paths::DirectoriesOverlap;
	UAM_ASSERT(DirectoriesOverlap("/work/repo", "/work/repo", false));
	UAM_ASSERT(DirectoriesOverlap("/work/repo/", "/work/repo/src", false));
	UAM_ASSERT(DirectoriesOverlap("/work/repo/src", "/work/./repo", false));
	UAM_ASSERT(!DirectoriesOverlap("/work/repo", "/work/repository", false));
	UAM_ASSERT(!DirectoriesOverlap("/work/repo", "/work/other", false));
	// An unknown workspace overlaps nothing, so it can never block an update.
	UAM_ASSERT(!DirectoriesOverlap("", "/work/repo", false));
	UAM_ASSERT(!DirectoriesOverlap("/work/repo", "", false));
	// A Windows host's paths, compared on any controller.
	UAM_ASSERT(DirectoriesOverlap("C:\\wc", "C:\\wc\\sub", true));
	UAM_ASSERT(DirectoriesOverlap("C:\\WC", "c:/wc/sub", true));
	UAM_ASSERT(!DirectoriesOverlap("/Users/x/Repo", "/Users/x/repo", false));
	UAM_ASSERT(DirectoriesOverlap("/Users/x/Repo", "/Users/x/repo", true));
}

