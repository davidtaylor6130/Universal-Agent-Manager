"""Only disposable file:// repositories; never an installed app or saved SVN profile."""
import pathlib
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="uam-svn-composer-@") as temporary:
    root = pathlib.Path(temporary).resolve()
    repository, working_copy, config = root / "repository", root / "copy @literal", root / "config"
    config.mkdir()
    subprocess.run(["svnadmin", "create", str(repository)], check=True, timeout=30)
    svn = ["svn", "--config-dir", str(config), "--no-auth-cache", "--non-interactive"]

    def run(*args):
        subprocess.run(svn + list(args), check=True, timeout=30, stdout=subprocess.DEVNULL)

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
    subprocess.run([str(binary), "--integration", str(working_copy), str(config)], check=True, timeout=120)
