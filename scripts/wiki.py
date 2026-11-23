import base64
import os
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory


def main() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    token = os.environ.get("WIKI_CONTENTS_TOKEN")
    if not token:
        raise SystemExit("WIKI_CONTENTS_TOKEN is required to publish the Wiki.")

    repository = os.environ["GITHUB_REPOSITORY"]
    actor = os.environ["GITHUB_ACTOR"]
    credential = base64.b64encode(f"x-access-token:{token}".encode()).decode("ascii")
    git_auth = os.environ | {
        "GIT_CONFIG_COUNT": "1",
        "GIT_CONFIG_KEY_0": "http.https://github.com/.extraheader",
        "GIT_CONFIG_VALUE_0": f"AUTHORIZATION: basic {credential}",
        "GIT_TERMINAL_PROMPT": "0",
    }

    tracked = subprocess.check_output(
        ["git", "ls-files", "-z", "--", ":(glob)docs/*.md"], cwd=repo_root
    ).decode("utf-8").split("\0")
    pages = [Path(path) for path in tracked if path]
    if not pages:
        raise SystemExit("No tracked top-level Wiki pages were found in docs/.")

    with TemporaryDirectory(prefix="magpie-wiki-") as temp:
        wiki = Path(temp) / "wiki"
        subprocess.run(
            ["git", "clone", f"https://github.com/{repository}.wiki.git", str(wiki)],
            env=git_auth,
            check=True,
        )
        for page in pages:
            shutil.copyfile(repo_root / page, wiki / page.name)

        subprocess.run(["git", "add", "--", *(page.name for page in pages)], cwd=wiki, check=True)
        if not subprocess.check_output(["git", "diff", "--cached", "--name-only"], cwd=wiki):
            print("Wiki pages are up to date.")
            return

        subprocess.run(["git", "config", "user.name", actor], cwd=wiki, check=True)
        subprocess.run(
            ["git", "config", "user.email", f"{actor}@users.noreply.github.com"], cwd=wiki, check=True
        )
        subprocess.run(["git", "commit", "-m", "Update Wiki pages"], cwd=wiki, check=True)
        subprocess.run(["git", "push", "origin", "HEAD"], cwd=wiki, env=git_auth, check=True)


if __name__ == "__main__":
    main()
