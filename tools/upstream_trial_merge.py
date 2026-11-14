#!/usr/bin/env python3
"""A trial merge of upstream (OpenCE's main) into dev, reported, never
committed (.github/workflows/upstream-trial-merge.yml runs it weekly and
puts the report in one tracking issue; docs/upstream_sync.md):

    git fetch opence main
    tools/upstream_trial_merge.py --base origin/dev --upstream opence/main --output report.md

The report: upstream's commits since the last sync (dev's merge base with
it), the files the merge would conflict in, by area, and what changes on the
wire (the Legacy network version, upstream's new messages, and any of their
numbers GOAT H1's own already use). The merge is tried in a worktree of its
own, removed after.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import netver  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MARKER = "<!-- upstream-trial-merge -->"
UPSTREAM_URL = "https://github.com/OpenCommunityEdition/OpenCE"
RULES = "docs/upstream_sync.md"
LEGACY_VERSION = re.compile(r"^#define HALO_PORT_NETWORK_VERSION (\d+)", re.M)
# areas, first match wins: (path prefix or regex, area)
AREAS = (
    (r"^port/assets/menus/", "Menus (generated: tools/menus_regen.sh --resolve)"),
    (r"^tools/(port_settings|ce_menus|menu_)", "Menus (generator)"),
    (r"^(source/networking/|port/linux/game/network|port/linux/include/halo_port_limits\.h|port/linux/NETCODE\.md"
     r"|port/linux/network_format\.txt|port/linux/(game|src)/(p2p|matchmaking|master|server_master|lobby))",
     "Network"),
    (r"^(docs/|website/)|\.md$", "Docs"),
    (r"^source/(sound|.*audio)|^port/linux/(game|src)/.*(sound|audio|dsound)", "Sound"),
    (r"^(port/linux/(src|host)/.*(vulkan|render|gpu|shader|xgpu|nv2a|d3d|gl\b|textures)|.*\.(metal|glsl|hlsl)$"
     r"|source/rasterizer/)", "Graphics"),
    (r"^source/", "Engine (source/)"),
    (r"^port/linux/game/", "Port game code (port/linux/game)"),
    (r"^port/linux/(src|include|host)/", "Port host code (port/linux/src, include)"),
    (r"^port/assets/", "Assets (port/assets)"),
    (r"^port/(android|ios|macos|windows|web)/", "Platforms"),
    (r"^port/", "Port (other)"),
    (r"^(\.github/|configure\.py$|tools/(ci_|.*_build))", "Build and CI"),
    (r"^tools/", "Tools and tests"),
)


def git(*args: str, cwd: Path = None, check: bool = True) -> str:
    result = subprocess.run(["git", "-C", str(cwd or ROOT), *args], text=True, capture_output=True)
    if check and result.returncode:
        raise SystemExit(f"git {' '.join(args)} failed:\n{result.stderr}")
    return result.stdout


def area_of(path: str) -> str:
    for pattern, area in AREAS:
        if re.search(pattern, path):
            return area
    return "Other"


def show(ref: str, path: str) -> str:
    return git("show", f"{ref}:{path}", check=False)


def legacy_version(ref: str):
    match = LEGACY_VERSION.search(show(ref, netver.LIMITS))
    return int(match.group(1)) if match else None


def messages(ref: str) -> dict[str, int]:
    text = show(ref, netver.KINDS[0].path)
    if not text:
        return {}
    try:
        return {entry.name: entry.value for entry in netver.enum_entries(netver.KINDS[0], text.splitlines())}
    except (netver.NetverError, StopIteration):
        return {}


def trial_merge(base: str, upstream: str) -> tuple[list[tuple[str, str]], str]:
    """the conflicted files (status, path) of merging upstream into base, and git's summary"""
    with tempfile.TemporaryDirectory(prefix="trial-merge-") as folder:
        tree = Path(folder) / "tree"
        git("worktree", "add", "-q", "--detach", str(tree), base)
        try:
            result = subprocess.run(["git", "-C", str(tree), "-c", "user.name=trial", "-c", "user.email=trial@invalid",
                                     "merge", "--no-commit", "--no-ff", upstream], text=True, capture_output=True)
            conflicted = []
            for line in git("status", "--porcelain", "--no-renames", cwd=tree).splitlines():
                status, path = line[:2], line[3:]
                if status in ("DD", "AU", "UD", "UA", "DU", "AA", "UU"):
                    conflicted.append((status, path))
            summary = "clean" if result.returncode == 0 else "conflicts"
        finally:
            git("merge", "--abort", cwd=tree, check=False)
            git("worktree", "remove", "--force", str(tree), check=False)
    return conflicted, summary


STATUS_WORDS = {"UU": "both changed", "AA": "both added", "DU": "we deleted", "UD": "they deleted",
                "AU": "we added", "UA": "they added", "DD": "both deleted"}


def report(base: str, upstream: str, repository: str = "OpenH1/goath1") -> str:
    base_sha = git("rev-parse", base).strip()
    upstream_sha = git("rev-parse", upstream).strip()
    since = git("merge-base", base_sha, upstream_sha).strip()
    commits = git("log", "--no-merges", "--format=%h%x09%ad%x09%s", "--date=short", f"{since}..{upstream_sha}")
    commits = [line.split("\t", 2) for line in commits.splitlines()]
    lines = [MARKER, f"Trial merge of OpenCE `main` ({upstream_sha[:12]}) into `dev` ({base_sha[:12]}). "
             "Report only: nothing was pushed. Merge by the rules in "
             f"[{RULES}](https://github.com/{repository}/blob/dev/{RULES}).", ""]
    lines.append(f"Last sync (merge base): {since[:12]} ({git('log', '-1', '--format=%ad', '--date=short', since).strip()})")
    lines.append("")

    lines.append(f"## New upstream commits ({len(commits)})")
    lines.append("")
    if not commits:
        lines.append("None: dev has everything on OpenCE's main.")
    for sha, date, subject in commits[:200]:
        lines.append(f"- [{sha}]({UPSTREAM_URL}/commit/{sha}) {date} {subject}")
    if len(commits) > 200:
        lines.append(f"- ... and {len(commits) - 200} more (`git log {since[:12]}..opence/main`)")
    lines.append("")

    conflicted, summary = trial_merge(base_sha, upstream_sha) if commits else ([], "clean")
    lines.append(f"## Conflicts ({len(conflicted)} files)")
    lines.append("")
    if not conflicted:
        lines.append("None: the merge is clean." if commits else "Nothing to merge.")
    by_area = defaultdict(list)
    for status, path in conflicted:
        by_area[area_of(path)].append((status, path))
    for area in sorted(by_area, key=lambda name: (-len(by_area[name]), name)):
        lines.append(f"### {area} ({len(by_area[area])})")
        lines.append("")
        for status, path in sorted(by_area[area], key=lambda item: item[1]):
            lines.append(f"- `{path}` ({STATUS_WORDS.get(status, status)})")
        lines.append("")

    lines.append("## Network")
    lines.append("")
    then, theirs, ours = legacy_version(since), legacy_version(upstream_sha), legacy_version(base_sha)
    if theirs != then:
        lines.append(f"- **Legacy network version (`HALO_PORT_NETWORK_VERSION`) changed upstream: {then} to {theirs}** "
                     f"(dev has {ours}). Take theirs; raise `HALO_PORT_STANDARD_NETWORK_VERSION` and its base with it "
                     "(port/linux/NETCODE.md, \"Numbers on parallel branches\").")
    else:
        lines.append(f"- Legacy network version unchanged upstream: {theirs} (dev has {ours}).")
    before, after, own = messages(since), messages(upstream_sha), messages(base_sha)
    added = {name: value for name, value in after.items() if before.get(name) != value}
    if added:
        lines.append("- Upstream messages new or renumbered: " + ", ".join(
            f"`{name}` = {value}" for name, value in sorted(added.items(), key=lambda item: item[1])))
    first = int(netver.KINDS[0].first)
    taken = {value: name for name, value in own.items() if value >= first}
    clashes = [f"`{name}` = {value} (ours: `{taken[value]}`)" for name, value in added.items()
               if value in taken and taken[value] != name]
    if clashes:
        lines.append("- **Upstream messages on numbers GOAT H1's own use:** " + ", ".join(clashes))
    lines.append("")
    lines.append(f"<sub>tools/upstream_trial_merge.py; merge {summary}.</sub>")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", default="origin/dev", help="the branch merged into (origin/dev)")
    parser.add_argument("--upstream", default="opence/main", help="upstream's branch (opence/main)")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", "OpenH1/goath1"),
                        help="the repository the rules' link points into")
    parser.add_argument("--output", type=Path, help="write the report here (else print it)")
    args = parser.parse_args()
    text = report(args.base, args.upstream, args.repository)
    if args.output:
        args.output.write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
