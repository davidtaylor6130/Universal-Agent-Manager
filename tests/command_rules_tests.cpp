#include "test_harness.h"
#include "common/security/command_rules.h"

#include <array>
#include <chrono>
#include <string>
#include <vector>

using namespace uam_test;

namespace
{
	namespace rules = uam::command_rules;

	rules::Rule MakeRule(std::string id, rules::Action action, rules::Tool tool, std::string pattern = {})
	{
		rules::Rule rule;
		rule.id = std::move(id);
		rule.action = action;
		rule.tool = tool;
		rule.pattern = std::move(pattern);
		return rule;
	}

	rules::Context CommandContext(std::string_view command, bool allow_permitted = true, bool read_only = false)
	{
		rules::Context context;
		context.category = rules::Category::Command;
		context.command = command;
		context.read_only = read_only;
		context.provider_id = "claude-cli";
		context.workspace = "/work/project";
		context.allow_permitted = allow_permitted;
		return context;
	}

	rules::Outcome OutcomeFor(const std::vector<rules::Rule>& list, std::string_view command)
	{
		return rules::Evaluate(list, CommandContext(command)).outcome;
	}
}

UAM_TEST(CommandRulesDenyWinsOverAskAndAllow)
{
	const std::vector<rules::Rule> list = {
	    MakeRule("allow-npm", rules::Action::Allow, rules::Tool::Command, "npm test"),
	    MakeRule("ask-npm", rules::Action::Ask, rules::Tool::Command, "npm"),
	    MakeRule("deny-npm-test", rules::Action::Deny, rules::Tool::Command, "npm test"),
	};
	const rules::Decision decision = rules::Evaluate(list, CommandContext("npm test"));
	UAM_ASSERT(decision.outcome == rules::Outcome::Deny);
	UAM_ASSERT_EQ(decision.rule_id, std::string("deny-npm-test"));

	const std::vector<rules::Rule> ask_over_allow = {list[0], list[1]};
	UAM_ASSERT(OutcomeFor(ask_over_allow, "npm test") == rules::Outcome::AskUser);
	UAM_ASSERT(OutcomeFor({list[0]}, "npm test") == rules::Outcome::Approve);
	// Order in the list does not change precedence.
	UAM_ASSERT(OutcomeFor({list[2], list[0]}, "npm test") == rules::Outcome::Deny);
	UAM_ASSERT(OutcomeFor({list[0]}, "npm run build") == rules::Outcome::NoMatch);
}

UAM_TEST(CommandRulesAllowPrefixesAndGlobs)
{
	const std::vector<rules::Rule> list = {
	    MakeRule("allow-npm-test", rules::Action::Allow, rules::Tool::Command, "npm test"),
	    MakeRule("allow-npm-run", rules::Action::Allow, rules::Tool::Command, "npm run lint*"),
	    MakeRule("allow-cargo", rules::Action::Allow, rules::Tool::Command, "cargo ** --release"),
	    MakeRule("allow-quoted", rules::Action::Allow, rules::Tool::Command, "make \"two words\""),
	};
	for (const std::string_view command : {"npm test", "npm test -- --watch=false", "npm run lint", "npm run lint:fix", "cargo build --release", "cargo --locked build --release", "make 'two words'", "make two\\ words"})
		UAM_ASSERT(OutcomeFor(list, command) == rules::Outcome::Approve);
	for (const std::string_view command : {"npm", "npm tests", "npm run build", "cargo build", "make two words", "NPM test", "./npm test", "/tmp/npm test", "bin/npm test"})
		UAM_ASSERT(OutcomeFor(list, command) == rules::Outcome::NoMatch);
}

UAM_TEST(CommandRulesInjectionCannotRideAnAllowedPrefix)
{
	const std::vector<rules::Rule> list = {
	    MakeRule("allow-npm-test", rules::Action::Allow, rules::Tool::Command, "npm test"),
	    MakeRule("deny-rm", rules::Action::Deny, rules::Tool::Command, "rm"),
	};
	for (const std::string_view command : {
	         "npm test; rm -rf /", "npm test && rm -rf /", "npm test || rm -rf /", "npm test | rm -rf /", "npm test & rm -rf /",
	         "npm test\nrm -rf /", "npm test\r\nrm -rf /", "npm test $(rm -rf /)", "npm test `rm -rf /`", "npm test \"$(rm -rf /)\"",
	         "npm test \"`rm -rf /`\"", "npm test <(rm -rf /)", "npm test >(rm -rf /)", "npm test > /dev/null; rm x", "(rm -rf /)",
	         "npm test ; sudo rm -rf /", "npm test;env rm -rf /", "npm test && bash -c 'rm -rf /'", "npm test && sh -lc \"echo; rm -rf /\"",
	         "npm test && xargs -n1 rm < list", "npm test && find . -exec rm {} \\;", "npm test && eval 'rm -rf /'", "npm test && /bin/rm -rf /",
	         "npm test && RM -rf /", "npm test && r\\m -rf /", "npm test && 'r'm -rf /", "npm test && \"rm\" -rf /", "npm test && command rm x",
	         "npm test && timeout -s KILL 5 rm x", "npm test && env -S 'rm -rf /'", "npm test && cmd /c \"del x & rm y\"", "npm test 2>/dev/null; rm x"})
	{
		const rules::Outcome outcome = OutcomeFor(list, command);
		if (outcome != rules::Outcome::Deny) throw TestFailure("Injected rm was not denied: " + std::string(command));
	}
	// Dynamic program names cannot be checked against deny rules, so they wait for the user.
	for (const std::string_view command : {"npm test && $CMD -rf /", "npm test && $(echo rm) -rf /", "npm test && `echo rm` x", "npm test && $'\\x72m' -rf /", "npm test && \"${X}\" y", "npm test 'unterminated"})
	{
		const rules::Outcome outcome = OutcomeFor(list, command);
		if (outcome != rules::Outcome::AskUser) throw TestFailure("Dynamic program was not left for the user: " + std::string(command));
	}
	// Even without a deny rule, compound or dynamic text never matches an allow prefix.
	const std::vector<rules::Rule> allow_only = {list[0]};
	for (const std::string_view command : {"npm test; ls", "npm test && ls", "npm test $(ls)", "npm test `ls`", "npm test > out.txt", "npm test $HOME", "npm test *", "npm test ~/x", "FOO=1 npm test", "npm test {a,b}", "npm test \"$X\""})
	{
		const rules::Outcome outcome = rules::Evaluate(allow_only, CommandContext(command)).outcome;
		if (outcome == rules::Outcome::Approve) throw TestFailure("Allow prefix approved non-literal command: " + std::string(command));
	}
	// cmd.exe and PowerShell do not share POSIX quoting or comments, so quoted or commented operators never
	// ride an allow prefix either, and deny rules still see the program they would run there.
	for (const std::string_view command : {"npm test -- --grep 'a;b'", "npm test 'x&rm y'", "npm test \"\\\"&rm y\"", "npm test #&rm y", "npm test %X%", "npm test !X!", "npm test ^& rm y"})
	{
		if (rules::Evaluate(allow_only, CommandContext(command)).outcome == rules::Outcome::Approve) throw TestFailure("Allow prefix approved a cross-shell operator: " + std::string(command));
	}
	for (const std::string_view command : {"npm test 'x&rm y'", "npm test \"\\\"&rm y\"", "npm test #&rm y", "npm test ^& rm y"})
	{
		if (OutcomeFor(list, command) != rules::Outcome::Deny) throw TestFailure("Cross-shell rm was not denied: " + std::string(command));
	}
	UAM_ASSERT(OutcomeFor(list, "%COMSPEC% /c rm x") == rules::Outcome::Deny);
	UAM_ASSERT(OutcomeFor(list, "npm test && %TOOL% x") == rules::Outcome::AskUser);
	// Literal quoted arguments without operator characters still match.
	UAM_ASSERT(rules::Evaluate(allow_only, CommandContext("npm test -- --grep 'two words'")).outcome == rules::Outcome::Approve);
}

UAM_TEST(CommandRulesDenyFollowsWrappersButNotPlainArguments)
{
	const std::vector<rules::Rule> list = {MakeRule("deny-rm", rules::Action::Deny, rules::Tool::Command, "rm")};
	for (const std::string_view command : {"sudo -u root rm x", "nice -n 5 rm x", "FOO=1 rm x", "env A=1 rm x", "nohup rm x", "xargs -I {} rm {}", "time rm x", "git ls-files | xargs rm"})
		UAM_ASSERT(OutcomeFor(list, command) == rules::Outcome::Deny);
	for (const std::string_view command : {"grep rm file.txt", "echo rm", "git commit -m 'rm old files'", "npm run rm"})
		UAM_ASSERT(OutcomeFor(list, command) == rules::Outcome::NoMatch);
}

UAM_TEST(CommandRulesDefaultsDenyForcePushAndCredentials)
{
	const std::vector<rules::Rule> defaults = rules::DefaultRules();
	UAM_ASSERT_EQ(rules::Normalize(defaults).size(), defaults.size());
	for (const std::string_view command : {
	         "git push --force", "git push -f origin main", "git push origin main --force-with-lease", "git -C repo push --force", "git push -uf origin main",
	         "git push origin +main", "cat ~/.ssh/id_rsa", "cp .ssh/id_ed25519 x", "cat ~/.aws/credentials", "security find-generic-password -s x",
	         "security dump-keychain", "git credential fill", "git credential-osxkeychain get", "gh auth token", "bash -c 'git push --force'"})
	{
		if (OutcomeFor(defaults, command) != rules::Outcome::Deny) throw TestFailure("Default rules did not deny: " + std::string(command));
	}
	for (const std::string_view command : {"rm -rf build", "rm x", "cat .env", "source .env.local"})
		UAM_ASSERT(OutcomeFor(defaults, command) == rules::Outcome::AskUser);
	for (const std::string_view command : {"git push", "git push --follow-tags", "git push origin main"})
		UAM_ASSERT(OutcomeFor(defaults, command) == rules::Outcome::NoMatch);
	// The read-only default stands in for the reviewer only when the read filter admitted the command.
	UAM_ASSERT(rules::Evaluate(defaults, CommandContext("cat README.md", true, true)).outcome == rules::Outcome::Approve);
	UAM_ASSERT(rules::Evaluate(defaults, CommandContext("cat README.md", true, false)).outcome == rules::Outcome::NoMatch);
	// A deny still wins over the read-only allow.
	UAM_ASSERT(rules::Evaluate(defaults, CommandContext("cat .ssh/config", true, true)).outcome == rules::Outcome::Deny);
}

UAM_TEST(CommandRulesNeverApproveAboveTheCeiling)
{
	// An allow rule may only replace the AI reviewer's one-time approval; when the caller's ceiling refuses
	// that approval (not AI Review, high risk, Plan or captured read-only, provider decision), no rule set
	// produces Approve, whatever the request category or rule shape.
	std::vector<rules::Rule> every;
	for (const rules::Tool tool : {rules::Tool::Command, rules::Tool::ReadOnly, rules::Tool::Edit})
		every.push_back(MakeRule("allow-" + std::string(rules::ToolName(tool)), rules::Action::Allow, tool, tool == rules::Tool::Command ? "git" : ""));
	every.push_back(MakeRule("allow-rm", rules::Action::Allow, rules::Tool::Command, "rm"));
	const std::array categories = {rules::Category::Command, rules::Category::Read, rules::Category::Edit, rules::Category::Fetch, rules::Category::Other};
	for (const rules::Category category : categories)
		for (const bool read_only : {false, true})
			for (const std::string_view command : {"git push --force", "rm -rf /", "cat README.md", ""})
			{
				rules::Context context;
				context.category = category;
				context.command = command;
				context.read_only = read_only;
				context.allow_permitted = false;
				const rules::Decision decision = rules::Evaluate(every, context);
				UAM_ASSERT(decision.outcome != rules::Outcome::Approve);
			}
	// Allow rules cannot be written for every tool, web access or unclassified tools, or as a bare glob.
	for (const rules::Tool tool : {rules::Tool::Any, rules::Tool::Fetch, rules::Tool::Other})
		UAM_ASSERT(!rules::ValidationError(MakeRule("x", rules::Action::Allow, tool)).empty());
	for (const std::string_view pattern : {"*", "**", "** rm", "?m", "", "npm test; rm", "npm test && x", "npm $(x)", "'open"})
		UAM_ASSERT(!rules::ValidationError(MakeRule("x", rules::Action::Allow, rules::Tool::Command, std::string(pattern))).empty());
	UAM_ASSERT(rules::ValidationError(MakeRule("x", rules::Action::Deny, rules::Tool::Any)).empty());
	UAM_ASSERT(!rules::ValidationError(MakeRule("x", rules::Action::Deny, rules::Tool::Edit, "rm")).empty());
}

UAM_TEST(CommandRulesScopeByProviderAndWorkspace)
{
	rules::Rule rule = MakeRule("deny-make", rules::Action::Deny, rules::Tool::Command, "make");
	rule.provider_id = "codex-cli";
	UAM_ASSERT(OutcomeFor({rule}, "make") == rules::Outcome::NoMatch);
	rule.provider_id = "claude-cli";
	UAM_ASSERT(OutcomeFor({rule}, "make") == rules::Outcome::Deny);
	rule.workspace = "/work/other";
	UAM_ASSERT(OutcomeFor({rule}, "make") == rules::Outcome::NoMatch);
	rule.workspace = "/work";
	UAM_ASSERT(OutcomeFor({rule}, "make") == rules::Outcome::Deny);
	rule.workspace = "/work/proj"; // A sibling prefix is not a parent folder.
	UAM_ASSERT(OutcomeFor({rule}, "make") == rules::Outcome::NoMatch);
	rule.workspace = "/work/project/";
	UAM_ASSERT(OutcomeFor({rule}, "make") == rules::Outcome::Deny);
	rule.enabled = false;
	UAM_ASSERT(OutcomeFor({rule}, "make") == rules::Outcome::NoMatch);
}

UAM_TEST(CommandRulesToolCategoriesUseStructuredKinds)
{
	UAM_ASSERT(rules::CategoryForKind("execute", false) == rules::Category::Command);
	UAM_ASSERT(rules::CategoryForKind("commandExecution", false) == rules::Category::Command);
	UAM_ASSERT(rules::CategoryForKind("execute", true) == rules::Category::Command);
	UAM_ASSERT(rules::CategoryForKind("read", false) == rules::Category::Read);
	UAM_ASSERT(rules::CategoryForKind("read", true) == rules::Category::Fetch);
	UAM_ASSERT(rules::CategoryForKind("fileChange", false) == rules::Category::Edit);
	UAM_ASSERT(rules::CategoryForKind("mcp", false) == rules::Category::Other);

	const std::vector<rules::Rule> list = {MakeRule("deny-fetch", rules::Action::Deny, rules::Tool::Fetch), MakeRule("allow-edit", rules::Action::Allow, rules::Tool::Edit)};
	rules::Context fetch;
	fetch.category = rules::Category::Fetch;
	fetch.allow_permitted = true;
	UAM_ASSERT(rules::Evaluate(list, fetch).outcome == rules::Outcome::Deny);
	rules::Context edit;
	edit.category = rules::Category::Edit;
	edit.allow_permitted = true;
	UAM_ASSERT(rules::Evaluate(list, edit).outcome == rules::Outcome::Approve);
	edit.allow_permitted = false;
	const rules::Decision blocked = rules::Evaluate(list, edit);
	UAM_ASSERT(blocked.outcome == rules::Outcome::NoMatch && blocked.allow_blocked_by_ceiling);
}

UAM_TEST(CommandRulesNormalizeDropsInvalidDuplicateAndExcessRules)
{
	std::vector<rules::Rule> input = {
	    MakeRule("a", rules::Action::Deny, rules::Tool::Command, "rm"),
	    MakeRule("a", rules::Action::Deny, rules::Tool::Command, "mv"),
	    MakeRule("", rules::Action::Deny, rules::Tool::Command, "rm"),
	    MakeRule("b", rules::Action::Allow, rules::Tool::Any),
	    MakeRule("c", rules::Action::Ask, rules::Tool::Command, "line\nbreak"),
	};
	for (std::size_t i = 0; i < rules::kMaxRules + 10; ++i) input.push_back(MakeRule("n" + std::to_string(i), rules::Action::Ask, rules::Tool::Command, "x"));
	const std::vector<rules::Rule> normalized = rules::Normalize(input);
	UAM_ASSERT_EQ(normalized.size(), rules::kMaxRules);
	UAM_ASSERT_EQ(normalized.front().pattern, std::string("rm"));
	UAM_ASSERT_EQ(normalized[1].id, std::string("n0"));
}

UAM_TEST(CommandRulesStayLinearOnHostileCommands)
{
	// Many wrapper and -exec hops against `**` patterns once took minutes; caps and the linear matcher keep a
	// maximum-size command well inside a second, and anything past the caps waits for the user.
	std::string command = "find .";
	while (command.size() + 32 < rules::kMaxCommandBytes) command += " -exec git push -exec sudo env xargs";
	const auto started = std::chrono::steady_clock::now();
	const rules::Decision decision = rules::Evaluate(rules::DefaultRules(), CommandContext(command));
	const auto elapsed = std::chrono::steady_clock::now() - started;
	UAM_ASSERT(decision.outcome == rules::Outcome::AskUser || decision.outcome == rules::Outcome::Deny);
	if (elapsed > std::chrono::seconds(5)) throw TestFailure("Rule evaluation took " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms");
	std::string chained;
	while (chained.size() + 16 < rules::kMaxCommandBytes) chained += "git push -f;";
	UAM_ASSERT(OutcomeFor(rules::DefaultRules(), chained) == rules::Outcome::Deny);
}

UAM_TEST(CommandRulesGlobsAndBracesCannotHideAProgramOrPath)
{
	const std::vector<rules::Rule> defaults = rules::DefaultRules();
	// A program name the shell expands is unknown until it runs, so it waits for the user.
	for (const std::string_view command : {"/bin/r? -rf x", "/bin/r[m] -rf x", "{rm,-rf,x}", "r* -rf x"})
	{
		const rules::Outcome outcome = OutcomeFor(defaults, command);
		if (outcome != rules::Outcome::AskUser && outcome != rules::Outcome::Deny) throw TestFailure("Expanded program passed: " + std::string(command));
	}
	// An argument glob that could expand to a denied path counts as that path.
	for (const std::string_view command : {"cat ~/.ss?/id_rsa", "cat ~/.ssh/id_*", "cp {~/.ssh/id_rsa,x}", "cat ~/.aws/cred*"})
	{
		if (OutcomeFor(defaults, command) != rules::Outcome::Deny) throw TestFailure("Globbed credential path was not denied: " + std::string(command));
	}
	// A glob that cannot reach the pattern does not.
	UAM_ASSERT(OutcomeFor(defaults, "ls *.txt") == rules::Outcome::NoMatch);
	UAM_ASSERT(OutcomeFor(defaults, "cat src/*.cpp") == rules::Outcome::NoMatch);
	UAM_ASSERT(OutcomeFor(defaults, "git add *") == rules::Outcome::AskUser); // Could stage prod.env; never a credential deny.
	// A glob can still expand to an option-shaped file name, so it counts against option patterns.
	UAM_ASSERT(OutcomeFor(defaults, "git push origin *") == rules::Outcome::Deny);
	UAM_ASSERT(OutcomeFor(defaults, "cat *") == rules::Outcome::AskUser);
}

UAM_TEST(CommandRulesCoverWindowsPathsCaseAndShells)
{
	const std::vector<rules::Rule> defaults = rules::DefaultRules();
	for (const std::string_view command : {
	         "type C:\\Users\\me\\.ssh\\id_rsa", "Get-Content C:\\Users\\me\\.aws\\credentials", "cat ~/.SSH/id_rsa", "doas -u root rm x",
	         "iex 'git push --force'", "Invoke-Expression \"git push -f\"", "cmd.exe /c git push --force", "git.cmd push --force"})
	{
		const rules::Outcome outcome = OutcomeFor(defaults, command);
		if (outcome != rules::Outcome::Deny && outcome != rules::Outcome::AskUser) throw TestFailure("Windows or case form passed: " + std::string(command));
	}
	UAM_ASSERT(OutcomeFor(defaults, "cat .ENV") == rules::Outcome::AskUser);
	for (const std::string_view command : {"Remove-Item -Recurse build", "del /s build", "rd /s /q build", "rmdir build", "unlink x", "shred x", "find . -name '*.o' -delete", "ri build"})
	{
		if (OutcomeFor(defaults, command) != rules::Outcome::AskUser) throw TestFailure("Deletion did not ask: " + std::string(command));
	}
	for (const std::string_view command : {"powershell -EncodedCommand ZQBjAGgAbwA=", "pwsh -enc ZQBjAGgAbwA=", "powershell -ec ZQBj", "Invoke-Command -ScriptBlock x"})
	{
		if (OutcomeFor(defaults, command) != rules::Outcome::AskUser) throw TestFailure("Opaque PowerShell did not ask: " + std::string(command));
	}
}

UAM_TEST(CommandRulesLeaveRequestsWithoutATypedCommandToTheNormalFlow)
{
	// Command rules have nothing to read, so they neither hold nor deny; any-request rules still apply.
	UAM_ASSERT(OutcomeFor(rules::DefaultRules(), "") == rules::Outcome::NoMatch);
	UAM_ASSERT(OutcomeFor({MakeRule("ask-any", rules::Action::Ask, rules::Tool::Any)}, "") == rules::Outcome::AskUser);
}

UAM_TEST(CommandRulesValidateWorkspacesAndTestScopedRules)
{
	rules::Rule rule = MakeRule("deny-make", rules::Action::Deny, rules::Tool::Command, "make");
	for (const std::string_view folder : {"project", "~/project", "./x"})
	{
		rule.workspace = std::string(folder);
		UAM_ASSERT(!rules::ValidationError(rule).empty());
	}
	for (const std::string_view folder : {"/work/project", "C:\\work\\project", "c:/work", "\\\\server\\share"})
	{
		rule.workspace = std::string(folder);
		UAM_ASSERT(rules::ValidationError(rule).empty());
	}
	UAM_ASSERT(!rules::ValidationError(MakeRule("x", rules::Action::Allow, rules::Tool::Command, "./npm test")).empty());

	// A worktree chat still matches a rule for its source folder.
	rule.workspace = "/work/source";
	rules::Context context = CommandContext("make");
	context.workspace = "/work/.worktrees/feature";
	context.source_workspace = "/work/source/";
	UAM_ASSERT(rules::Evaluate({rule}, context).outcome == rules::Outcome::Deny);

	// The tester reports scoped rules as if they applied and keeps credential reads out of the read-only allow.
	rule.provider_id = "codex-cli";
	UAM_ASSERT(rules::TestCommand({rule}, "make", false).outcome == rules::Outcome::Deny);
	const std::vector<rules::Rule> read_only = {MakeRule("ro", rules::Action::Allow, rules::Tool::ReadOnly)};
	UAM_ASSERT(rules::TestCommand(read_only, "cat README.md", true).outcome == rules::Outcome::Approve);
	UAM_ASSERT(rules::TestCommand(read_only, "cat secrets.txt", true).outcome == rules::Outcome::NoMatch);
	UAM_ASSERT(rules::LooksCredentialLike("type C:\\Users\\me\\.SSH\\config"));
}
