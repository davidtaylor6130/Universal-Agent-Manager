"""SVN branch creation (#520) against disposable file:// repositories only; never an installed app or saved SVN profile."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import urllib.parse

if shutil.which("svn") is None or shutil.which("svnadmin") is None:
    if os.environ.get("UAM_REQUIRE_SVN") == "1":
        print("FAIL: UAM_REQUIRE_SVN=1 but svn or svnadmin is not installed", file=sys.stderr)
        sys.exit(1)
    print("SKIP: svn or svnadmin is not installed")
    sys.exit(77)

binary = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="uam-svn-create-") as temporary:
    root = pathlib.Path(temporary).resolve()
    repository, working_copy, config = root / "repository", root / "copy @literal", root / "config"
    config.mkdir()
    subprocess.run(["svnadmin", "create", str(repository)], check=True, timeout=30)
    svn = ["svn", "--config-dir", str(config), "--no-auth-cache", "--non-interactive"]

    def run(*args):
        subprocess.run(svn + list(args), check=True, timeout=30, stdout=subprocess.DEVNULL)

    def read(*args):
        return subprocess.check_output(svn + list(args), text=True, timeout=30).strip()

    def create(mode, name):
        """Runs the service against the working copy; returns the switched URL and revision it reported."""
        output = subprocess.run([str(binary), "--integration", str(working_copy), str(config), mode, name], check=True, timeout=120, capture_output=True, text=True).stdout
        print(output, end="")
        created = [line.split() for line in output.splitlines() if line.startswith("CREATED ")]
        return (created[0][1], created[0][2]) if created else (None, None)

    url = repository.as_uri()
    run("mkdir", url + "/trunk", url + "/branches", url + "/tags", "-m", "standard layout")
    run("checkout", url + "/trunk", str(working_copy))
    (working_copy / "tracked.txt").write_text("trunk content\n")
    run("add", str(working_copy / "tracked.txt") + "@")
    run("commit", str(working_copy) + "@", "-m", "trunk content")
    run("update", str(working_copy) + "@")
    working_revision = read("info", "--show-item", "revision", str(working_copy) + "@")

    # Trunk moves on after the working copy was updated, so the two sources differ.
    other = root / "other"
    run("checkout", url + "/trunk", str(other))
    (other / "tracked.txt").write_text("newer trunk content\n")
    run("commit", str(other) + "@", "-m", "newer trunk")

    # From the working copy: ^/trunk at the working copy's revision, then the working copy is switched to it.
    switched_url, switched_revision = create("working-copy", "from-copy")
    assert switched_url == url + "/branches/from-copy", switched_url
    assert read("info", "--show-item", "url", str(working_copy) + "@") == url + "/branches/from-copy"
    assert read("info", "--show-item", "revision", str(working_copy) + "@") == switched_revision
    log = read("log", "--xml", "--verbose", "-r", switched_revision, url + "@" + switched_revision)
    assert 'copyfrom-path="/trunk"' in log and 'copyfrom-rev="%s"' % working_revision in log and ">/branches/from-copy</path>" in log, log
    assert "UAM integration: create from-copy" in log, "the confirmed message is the commit message"
    assert (working_copy / "tracked.txt").read_text() == "trunk content\n", "a working-copy branch keeps the working copy's revision"

    # From the main line: ^/trunk at the repository's newest revision, whichever branch the working copy is on.
    head = read("info", "--show-item", "revision", url + "@HEAD")
    switched_url, switched_revision = create("main-line", "from-trunk")
    assert switched_url == url + "/branches/from-trunk", switched_url
    log = read("log", "--xml", "--verbose", "-r", switched_revision, url + "@" + switched_revision)
    assert 'copyfrom-path="/trunk"' in log and 'copyfrom-rev="%s"' % head in log, log
    assert (working_copy / "tracked.txt").read_text() == "newer trunk content\n", "a main-line branch has trunk's newest content"

    # An existing name is refused without a commit.
    before = read("info", "--show-item", "revision", url + "@HEAD")
    create("refuse:already exists", "from-copy")
    assert read("info", "--show-item", "revision", url + "@HEAD") == before, "a refused creation commits nothing"
    assert read("info", "--show-item", "url", str(working_copy) + "@") == url + "/branches/from-trunk"

    # A project folder whose name svn leaves unescaped in URLs (+, @) and one it escapes (space): the branch goes beside the
    # project's trunk, and the switch is confirmed although svn's URL differs from UAM's encoding of it.
    project = url + "/" + urllib.parse.quote("shop+@ x", safe="")
    run("mkdir", "--parents", project + "/trunk", project + "/branches", "-m", "project layout")
    run("switch", "--ignore-ancestry", project + "/trunk@", str(working_copy))
    switched_url, switched_revision = create("working-copy", "in-project")
    assert switched_url is not None, "creation under a project folder must confirm its switch"
    relative = urllib.parse.unquote(read("info", "--show-item", "relative-url", str(working_copy) + "@"))
    assert relative == "^/shop+@ x/branches/in-project", relative

    # A deletion committed from the working copy is not in it any more, but its URL@revision still has the file: copying the
    # working copy is refused until it is updated, and nothing is committed.
    (working_copy / "doomed.txt").write_text("short-lived\n")
    run("add", str(working_copy / "doomed.txt") + "@")
    run("commit", str(working_copy) + "@", "-m", "add a file")
    run("update", str(working_copy) + "@")
    run("delete", str(working_copy / "doomed.txt") + "@")
    run("commit", str(working_copy) + "@", "-m", "delete it")
    before = read("info", "--show-item", "revision", url + "@HEAD")
    create("refuse:deleted files in a later commit", "after-delete")
    assert read("info", "--show-item", "revision", url + "@HEAD") == before, "a refused creation commits nothing"
    run("update", str(working_copy) + "@")
    switched_url, _ = create("working-copy", "after-update")
    assert switched_url is not None and not (working_copy / "doomed.txt").exists(), "after updating, the deletion stays deleted on the new branch"
