"""Only disposable file:// repositories and a listen-once svnserve on 127.0.0.1; never an installed app or saved SVN profile."""
import os
import pathlib
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

if shutil.which("svn") is None or shutil.which("svnadmin") is None:
    if os.environ.get("UAM_REQUIRE_SVN") == "1":
        print("FAIL: UAM_REQUIRE_SVN=1 but svn or svnadmin is not installed", file=sys.stderr)
        sys.exit(1)
    print("SKIP: svn or svnadmin is not installed")
    sys.exit(77)

binary = pathlib.Path(sys.argv[1]).resolve()
posix = os.name != "nt"
# The fixture's own svn calls need a UTF-8 locale for the non-ASCII copy; the binary under test does not get one.
fixture_environment = dict(os.environ)
if posix:
    fixture_environment["LC_ALL"] = "en_US.UTF-8" if sys.platform == "darwin" else "C.UTF-8"
# `env -i PATH=...`: what a Finder launch gives the app, no locale, no SVN_SSH.
bare_environment = {"PATH": os.environ.get("PATH", "/usr/bin:/bin")}


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


with tempfile.TemporaryDirectory(prefix="uam-svn-composer-@") as temporary:
    root = pathlib.Path(temporary).resolve()
    repository, working_copy, config = root / "repository", root / "copy @literal", root / "config"
    config.mkdir()
    subprocess.run(["svnadmin", "create", str(repository)], check=True, timeout=30)
    svn = ["svn", "--config-dir", str(config), "--no-auth-cache", "--non-interactive"]

    def run(*args, cwd=None):
        subprocess.run(svn + list(args), check=True, timeout=30, stdout=subprocess.DEVNULL, cwd=cwd, env=fixture_environment)

    def revisions(path):
        """Lowest and highest working revision of every node, from svn's own recursive info."""
        found = set()
        output = subprocess.check_output(svn + ["info", "--depth", "infinity", "--show-item", "revision", str(path) + "@"], text=True, timeout=30)
        for line in output.splitlines():
            value = line.split()[0] if line.split() else ""
            if value.isdigit():
                found.add(int(value))
        return min(found), max(found)

    url = repository.as_uri()
    run("mkdir", url + "/lines", url + "/lines/main", "-m", "fixture layout")
    # SVN 1.14 reads "@" in a checkout destination as a peg revision, so check out to a plain relative name and rename.
    run("checkout", url + "/lines/main", "copy", cwd=root)
    (root / "copy").rename(working_copy)
    (working_copy / "tracked.txt").write_text("fixture content\n")
    (working_copy / "child").mkdir()
    (working_copy / "child" / "retained.txt").write_text("nested fixture content\n")
    run("add", str(working_copy / "tracked.txt") + "@")
    run("add", str(working_copy / "child") + "@")
    run("commit", str(working_copy) + "@", "-m", "fixture content")
    run("copy", url + "/lines/main", url + "/lines/feature%20a", "-m", "fixture branch")
    branch_copy = root / "branch-edit"
    run("checkout", url + "/lines/feature%20a", "branch-edit", cwd=root)
    (branch_copy / "tracked.txt").write_text("branch content\n")
    run("commit", str(branch_copy) + "@", "-m", "fixture branch content")
    for depth in ("empty", "files", "immediates", "exclude"):
        run("update", "--set-depth", depth, str(working_copy / "child") + "@")
        before_url = subprocess.check_output(svn + ["info", "--show-item", "url", str(working_copy) + "@"], text=True, timeout=30)
        subprocess.run([str(binary), "--sparse-integration", str(working_copy), str(config)], check=True, timeout=120)
        after_url = subprocess.check_output(svn + ["info", "--show-item", "url", str(working_copy) + "@"], text=True, timeout=30)
        assert after_url == before_url, "refusal must preserve the selected working-copy URL"
        run("update", "--set-depth", "infinity", str(working_copy / "child") + "@")
    # A commit leaves a mixed-revision working copy: the committed file is newer than the root. It is clean, so the
    # switch may proceed, and the pinned switch must bring every node to one revision.
    run("update", str(working_copy) + "@")
    (working_copy / "child" / "retained.txt").write_text("nested fixture content\nmixed revision edit\n")
    run("commit", str(working_copy) + "@", "-m", "mixed revision commit")
    lowest, highest = revisions(working_copy)
    assert lowest < highest, "the fixture must start from a mixed-revision working copy"
    subprocess.run([str(binary), "--integration", str(working_copy), str(config)], check=True, timeout=120)
    lowest, highest = revisions(working_copy)
    assert lowest == highest, "the pinned switch must leave one revision across the working copy, not %d to %d" % (lowest, highest)

    if posix:
        # A non-ASCII working copy with no locale in the launch environment: it fails without the shared SVN environment
        # (the control), then discovers, switches and refuses exactly like the ASCII copy with it.
        accented = root / "wcé @literal"
        run("checkout", url + "/lines/main", "accented", cwd=root)
        (root / "accented").rename(accented)
        subprocess.run([str(binary), "--locale-off-integration", str(accented), str(config)], check=True, timeout=120, env=bare_environment)
        subprocess.run([str(binary), "--integration", str(accented), str(config)], check=True, timeout=120, env=bare_environment)

        # svn+ssh: a stand-in ssh records its arguments and refuses like a server without the user's key.
        tools = root / "tools"
        tools.mkdir()
        record = root / "ssh-arguments"
        fake_ssh = tools / "ssh"
        fake_ssh.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + str(record) + "'\necho 'Permission denied (publickey).' >&2\nexit 255\n")
        fake_ssh.chmod(fake_ssh.stat().st_mode | stat.S_IXUSR)
        ssh_url = "svn+ssh://repository.invalid/repo"
        tool_environment = {"PATH": str(tools) + os.pathsep + bare_environment["PATH"]}
        subprocess.run([str(binary), "--ssh-probe", str(config), ssh_url], check=True, timeout=60, env=tool_environment)
        assert record.read_text().splitlines()[:2] == ["-o", "BatchMode=yes"], "ssh runs in batch mode when SVN_SSH is unset"
        subprocess.run([str(binary), "--ssh-probe", str(config), ssh_url], check=True, timeout=60, env=dict(tool_environment, SVN_SSH="ssh -o UserChoice=yes"))
        assert "BatchMode=yes" not in record.read_text() and "UserChoice=yes" in record.read_text(), "the user's own SVN_SSH wins"
        # A tunnel the user configured is kept: setting SVN_SSH would replace its `$SVN_SSH ...` command whole.
        user_home = root / "home"
        user_config = user_home / ".subversion"
        user_config.mkdir(parents=True)
        (user_config / "config").write_text("[tunnels]\nssh = $SVN_SSH ssh -o UserTunnel=yes\n")
        subprocess.run([str(binary), "--ssh-probe", str(user_config), ssh_url], check=True, timeout=60, env=dict(tool_environment, HOME=str(user_home)))
        assert "BatchMode=yes" not in record.read_text() and "UserTunnel=yes" in record.read_text(), "the user's configured tunnel wins"

    if shutil.which("svnserve"):
        # A repository that refuses anonymous access, served on 127.0.0.1 only for this run.
        served = root / "served"
        subprocess.run(["svnadmin", "create", str(served)], check=True, timeout=30)
        (served / "conf" / "svnserve.conf").write_text("[general]\nanon-access = none\nauth-access = write\npassword-db = passwd\n")
        (served / "conf" / "passwd").write_text("[users]\nfixture = fixture-password\n")
        run("mkdir", served.as_uri() + "/lines", served.as_uri() + "/lines/main", "-m", "served layout")
        port = free_port()
        server = subprocess.Popen(["svnserve", "-d", "--foreground", "--listen-host", "127.0.0.1", "--listen-port", str(port), "-r", str(root)])
        try:
            for _ in range(100):
                with socket.socket() as probe:
                    if probe.connect_ex(("127.0.0.1", port)) == 0:
                        break
                time.sleep(0.05)
            served_copy = root / "served copy"
            # The fixture signs in once without saving the password; the binary then has no credentials to offer.
            run("checkout", "--username", "fixture", "--password", "fixture-password", "svn://127.0.0.1:%d/served/lines/main" % port, str(served_copy))
            subprocess.run([str(binary), "--credentials-integration", str(served_copy), str(config)], check=True, timeout=120)
        finally:
            server.kill()
            server.wait(timeout=30)
