"""Only disposable file:// repositories; never an installed app or saved SVN profile."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

if shutil.which("svn") is None or shutil.which("svnadmin") is None:
    if os.environ.get("UAM_REQUIRE_SVN") == "1":
        print("FAIL: UAM_REQUIRE_SVN=1 but svn or svnadmin is not installed", file=sys.stderr)
        sys.exit(1)
    print("SKIP: svn or svnadmin is not installed")
    sys.exit(77)

binary = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="uam-svn-composer-@") as temporary:
    root = pathlib.Path(temporary).resolve()
    repository, working_copy, config = root / "repository", root / "copy @literal", root / "config"
    config.mkdir()
    subprocess.run(["svnadmin", "create", str(repository)], check=True, timeout=30)
    svn = ["svn", "--config-dir", str(config), "--no-auth-cache", "--non-interactive"]

    def run(*args):
        subprocess.run(svn + list(args), check=True, timeout=30, stdout=subprocess.DEVNULL)

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
    run("checkout", url + "/lines/main", str(working_copy))
    (working_copy / "tracked.txt").write_text("fixture content\n")
    (working_copy / "child").mkdir()
    (working_copy / "child" / "retained.txt").write_text("nested fixture content\n")
    run("add", str(working_copy / "tracked.txt") + "@")
    run("add", str(working_copy / "child") + "@")
    run("commit", str(working_copy) + "@", "-m", "fixture content")
    run("copy", url + "/lines/main", url + "/lines/feature%20a", "-m", "fixture branch")
    branch_copy = root / "branch-edit"
    run("checkout", url + "/lines/feature%20a", str(branch_copy))
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
