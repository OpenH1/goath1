"""tools/upstream_trial_merge.py on a small repository: an upstream that
raised the Legacy network version, added a message on a number of ours, and
changed a file both sides changed"""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import upstream_trial_merge  # noqa: E402

LIMITS = "port/linux/include/halo_port_limits.h"
MESSAGES = "port/linux/game/network_distributed.h"


def run(repo, *args):
    subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@invalid", *args],
                   check=True, capture_output=True)


def write(repo, path, text):
    (repo / path).parent.mkdir(parents=True, exist_ok=True)
    (repo / path).write_text(text)


def messages(*entries):
    body = "".join(f"\t_distributed_message_{name} = {value},\n" for name, value in entries)
    return f"enum\n{{\n{body}\tNUMBER_OF_DISTRIBUTED_MESSAGES\n}};\n"


def test_report(tmp_path, monkeypatch):
    repo = tmp_path / "repo"
    repo.mkdir()
    run(repo, "init", "-q", "-b", "dev")
    write(repo, LIMITS, "#define HALO_PORT_NETWORK_VERSION 41\n")
    write(repo, MESSAGES, messages(("ping", 1)))
    write(repo, "port/linux/game/network_join.c", "int join = 1;\n")
    write(repo, "README.md", "readme\n")
    run(repo, "add", "-A")
    run(repo, "commit", "-q", "-m", "base")
    run(repo, "branch", "upstream")

    write(repo, MESSAGES, messages(("ping", 1), ("ours", 120)))
    write(repo, "port/linux/game/network_join.c", "int join = 2; /* ours */\n")
    run(repo, "commit", "-q", "-am", "ours")

    run(repo, "checkout", "-q", "upstream")
    write(repo, LIMITS, "#define HALO_PORT_NETWORK_VERSION 42\n")
    write(repo, MESSAGES, messages(("ping", 1), ("theirs", 120)))
    write(repo, "port/linux/game/network_join.c", "int join = 3; /* theirs */\n")
    write(repo, "README.md", "readme, theirs\n")
    run(repo, "commit", "-q", "-am", "Raise the network version")
    run(repo, "checkout", "-q", "dev")

    monkeypatch.setattr(upstream_trial_merge, "ROOT", repo)
    text = upstream_trial_merge.report("dev", "upstream", "example/repo")

    assert text.startswith(upstream_trial_merge.MARKER)
    assert "## New upstream commits (1)" in text and "Raise the network version" in text
    assert "### Network (2)" in text
    assert f"`{MESSAGES}` (both changed)" in text and "`port/linux/game/network_join.c` (both changed)" in text
    assert "README.md" not in text.split("## Conflicts")[1]  # (merged cleanly)
    assert "changed upstream: 41 to 42" in text
    assert "`_distributed_message_theirs` = 120 (ours: `_distributed_message_ours`)" in text
    assert "https://github.com/example/repo/blob/dev/docs/upstream_sync.md" in text
    # (report only: dev is where it was, and no worktree is left behind)
    status = subprocess.run(["git", "-C", str(repo), "status", "--porcelain"], capture_output=True, text=True)
    assert status.stdout == ""
    worktrees = subprocess.run(["git", "-C", str(repo), "worktree", "list"], capture_output=True, text=True)
    assert len(worktrees.stdout.splitlines()) == 1
