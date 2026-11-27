#!/usr/bin/env python3
"""The maps' preparations checked against play in each window context (port/linux/game/render_enumerate.c's list
of contexts): each map of a list is played for a while on a test machine with the shared kit (tools/rig/README.md,
goat-rig run --shared), every instance recording its draws (debug.render_manifest), and each recording is read as
tools/render_manifest.py check reads it: the draws of play the map's preparation did not make, per map, context and
instance, grouped by the functions they were drawn in, beside the backend's own count of them in its log
("pipeline missed map preparation", "a pipeline the map's preparation did not make").

    python3 tools/prepared_coverage.py --build <ref> [--context solo|split2|split3|split4|link ...]
        [--maps b30,c40,bloodgulch] [--seconds 90] [--jobs 3] [--shipped] [--exact] [--keep]
    python3 tools/prepared_coverage.py --rig omarchy --build <ref> ...   (from a Mac checkout: run there over ssh)

Run on the machine (or with --rig), in any checkout of the tools; --build is a ref goat-rig build has built there.
The contexts:
  solo    one player straight into the map (init.txt's map_name), the scripted player (bot);
  splitN  N local players in split screen (2 to 4): a campaign level as SPLIT-SCREEN CO-OP starts it
          (network test "campaign:"), a multiplayer map as team slayer ("local:"), so that each window's Spartan
          link has teammates; every player scripted, the cinematics skipped, the link opened;
  link    a host and a client over the run's loopback (goat-rig run --pair): a campaign level in co-op, a
          multiplayer map in team slayer, the link opened on both, so each draws the other's feed.
Each map is walked as it loads (HALO_SHIPPED_RECORDS=0: the walk of this build is what is checked), or with
--shipped played from the build's shipped records, as a player's first load is. The lines also count the Spartan
link feeds drawn (debug.spartan_link_log) and name each map's preparation. Draws are compared as a backend compiles
them, or whole with --exact (fields the Vulkan key does not hold then differ: tools/render_manifest.py). The run
folders (~/goat-rig/runs/pcov-*) are removed after the report unless --keep. It exits 1 on a pipeline the backend
missed (its log's count, which the Vulkan backend keys as it compiles) or a run that did not play; with --strict on
the recording's misses too, which also count context fields a backend may not compile apart (the HDR tag's
category, a stage's texture sampled from a texture or a target).
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from render_enumerate_sweep import STOCK_MAPS  # noqa: E402
from render_manifest import Recording, symbolize  # noqa: E402

RIG = Path.home() / "goat-rig"
GOAT_RIG = RIG / "bin" / "goat-rig"
PLAYABLE = {scenario.rsplit("\\", 1)[-1]: scenario for scenario in STOCK_MAPS if scenario != "ui"}
CONTEXTS = ("solo", "split2", "split3", "split4", "link")
# (debug.spartan_link_log: a line for each feed drawn, spartan_link.c's spartan_link_feed_draw)
FEED = re.compile(r"spartan link: \d+ \d+ \d+ [01] ")


def settings(context, scenario, arguments):
    """goat-rig run's options and each instance's settings (lists after its --)"""
    campaign = not scenario.startswith("levels\\test\\")
    common = [f"HALO_TEST_INPUT=bot:{arguments.seed}", "HALO_SPARTAN_LINK_OPEN=1", "HALO_SPARTAN_LINK_LOG=1",
              "HALO_SKIP_CINEMATICS=1"]
    if not arguments.shipped:
        common.append("HALO_SHIPPED_RECORDS=0")
    if context == "solo":
        return ["--map", scenario], [common + ["HALO_RENDER_MANIFEST={run:game}/manifest.txt"]]
    if context.startswith("split"):
        players = int(context[5:])
        test = f"campaign:{scenario}" if campaign else f"local:{scenario.rsplit(chr(92), 1)[-1]}:team_slayer"
        return [], [common + [f"HALO_NETWORK_TEST={test}", f"HALO_NETWORK_TEST_PLAYERS={players}",
                              "HALO_NETWORK_TEST_START=2", "HALO_RENDER_MANIFEST={run:game}/manifest.txt"]]
    variant = "coop" if campaign else "team_slayer"
    host = f"host:{scenario}:{variant}" if campaign else f"host:{scenario.rsplit(chr(92), 1)[-1]}:{variant}"
    return ["--pair", f"host:{arguments.build}", f"client:{arguments.build}"], [
        common + [f"HALO_NETWORK_TEST={host}", "HALO_NETWORK_TEST_START=15", "HALO_NETWORK_TEST_NAME=Host",
                  "HALO_RENDER_MANIFEST={run:host}/manifest.txt"],
        common[1:] + [f"HALO_TEST_INPUT=bot:{arguments.seed + 1}", "HALO_NETWORK_TEST=join",
                      "HALO_NETWORK_TEST_NAME=Client", "HALO_RENDER_MANIFEST={run:client}/manifest.txt"]]


def run(job, arguments):
    """one map in one context played; its label and the run's output"""
    name, context = job
    label = f"pcov-{name}-{context}"
    options, groups = settings(context, PLAYABLE[name], arguments)
    command = [str(GOAT_RIG), "run", label, "--build", arguments.build, "--shared",
               "--seconds", str(arguments.seconds + (30 if context == "link" else 0))] + options
    for group in groups:
        command += ["--"] + group
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return label, result.returncode, result.stdout


def report(name, context, label, status, output, image, arguments):
    """the run's lines; whether it missed or did not play"""
    folder = RIG / "runs" / label
    instances = sorted(path for path in folder.iterdir() if path.is_dir()) if folder.is_dir() else []
    lines, failed = [], status != 0
    if status != 0:
        lines.append(f"{name} {context}: goat-rig run exit {status}: " +
                     " | ".join(output.strip().splitlines()[-3:]))
    for instance in instances:
        log = (instance / "halo.log").read_text(errors="replace") if (instance / "halo.log").exists() else ""
        vulkan = log.count("pipeline missed map preparation")
        unmade = log.count("a pipeline the map's preparation did not make")
        feeds = len(FEED.findall(log))
        how = "walked" if "the shipped records left out" in log else "shipped record" \
            if "made from its record" in log else "?"
        recording_path = instance / "manifest.txt"
        if not recording_path.exists():
            lines.append(f"{name} {context} {instance.name}: FAIL no recording (vulkan {vulkan} missed)")
            failed = True
            continue
        recording = Recording(str(recording_path), arguments.exact)
        # (the recorder's own misses, each after its "# missed:" stack; a draw written again unmarked is one the
        # preparation made, or one marked before, recorded in another context: xgpu_manifest_record_draw)
        recording.misses = [miss for miss in recording.misses if miss[1]]
        missed = len(recording.misses)
        failed = failed or vulkan > 0 or unmade > 0 or recording.enumerations == 0 or (arguments.strict and missed > 0)
        lines.append(f"{name} {context} {instance.name}: {missed} missed of {len(recording.enumerated)} draws "
                     f"prepared ({recording.enumerations} preparations, {how}); vulkan {vulkan} missed, "
                     f"{unmade} not made; {feeds} link feeds drawn")
        if recording.misses:
            names = symbolize(image, {a for _, stack in recording.misses for a in stack}) if image else {}
            sites = {}
            for _, stack in recording.misses:
                functions = [names[a][0] if a in names else a for a in stack]
                while functions and functions[0].startswith(("xgpu_", "prepare_draw", "draw_", "D3DDevice_",
                                                             "record_")):
                    functions.pop(0)
                site = " < ".join(functions[:arguments.depth])
                sites[site] = sites.get(site, 0) + 1
            for site, count in sorted(sites.items(), key=lambda item: -item[1]):
                lines.append(f"    {count:4} {site}")
    if not instances:
        lines.append(f"{name} {context}: FAIL no instance ran")
        failed = True
    if not arguments.keep and folder.is_dir():
        shutil.rmtree(folder)
    return lines, failed


def remote(arguments, argv):
    """the same command on a test machine, in its build's own tree (its tools: this commit's or the build's)"""
    host = f"{arguments.rig}.fabric"
    hosts = Path.home() / ".config/goat-rig/hosts"
    if hosts.exists():
        for line in hosts.read_text().splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == arguments.rig:
                host = parts[1]
    rest, skip = [], False
    for item in argv:
        if skip:
            skip = False
        elif item == "--rig":
            skip = True
        elif not item.startswith("--rig="):
            rest.append(item)
    script = (f"tree=$(dirname $(dirname $(dirname $(~/goat-rig/bin/goat-rig path {shlex.quote(arguments.build)})))) "
              f"&& cd \"$tree\" && python3 tools/prepared_coverage.py {' '.join(shlex.quote(a) for a in rest)}")
    return subprocess.run(["ssh", host, f"bash -c {shlex.quote(script)}"]).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", required=True, help="a ref goat-rig build built on the machine")
    parser.add_argument("--context", action="append", choices=CONTEXTS, help="solo (default), split2..split4, link")
    parser.add_argument("--maps", help="comma-separated stock names (b30,bloodgulch); default every playable map")
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--jobs", type=int, default=3, help="runs at once (goat-rig's --shared GPU slots)")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--shipped", action="store_true", help="the shipped records played, not the walk")
    parser.add_argument("--strict", action="store_true", help="the recording's misses fail the run too")
    parser.add_argument("--exact", action="store_true", help="whole draws compared, not as a backend compiles them")
    parser.add_argument("--depth", type=int, default=4, help="the functions named per miss")
    parser.add_argument("--keep", action="store_true", help="the run folders kept")
    parser.add_argument("--rig", help="run on this test machine over ssh (omarchy, 5900x)")
    arguments = parser.parse_args()
    if arguments.rig:
        return remote(arguments, sys.argv[1:])
    if not GOAT_RIG.exists():
        raise SystemExit(f"no {GOAT_RIG}: run on a test machine, or with --rig")
    names = arguments.maps.split(",") if arguments.maps else list(PLAYABLE)
    unknown = [name for name in names if name not in PLAYABLE]
    if unknown:
        raise SystemExit(f"not a playable stock map: {', '.join(unknown)} (of {', '.join(PLAYABLE)})")
    contexts = arguments.context or ["solo"]
    binary = subprocess.run([str(GOAT_RIG), "path", arguments.build], stdout=subprocess.PIPE, text=True,
                            check=True).stdout.strip().splitlines()[-1]
    image = Path(binary).parent / "halo_guest.elf"
    image = str(image) if image.exists() else None

    jobs = [(name, context) for context in contexts for name in names]
    failed = False
    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
        futures = {pool.submit(run, job, arguments): job for job in jobs}
        for future in concurrent.futures.as_completed(futures):
            name, context = futures[future]
            label, status, output = future.result()
            lines, problem = report(name, context, label, status, output, image, arguments)
            print("\n".join(lines), flush=True)
            failed = failed or problem
    print("prepared coverage: " + ("FAIL (pipelines the backend missed, or runs that did not play)" if failed else
                                   f"no pipeline missed in {len(jobs)} runs"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
