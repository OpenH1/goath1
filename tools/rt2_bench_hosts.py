"""The lighting workbench's hosts and shared recordings (tools/rt2_bench.py's host and recording layers).

Shared recordings. A recording holds what the GAME did: the command stream the game sent to the renderer, as the
render thread's decoder played it (port/android/host/host_render_record.h). Everything downstream of that point is
the replay's: the Vulkan backend and the light pass's C (port/linux/vulkan, xgpu_spirv.c), its shaders (lt/*.glsl:
sent by run, or the build's own), the replay itself (port/tools/replay), and, once a recording carries LT_INPUTS,
the light list (lt_lights_build.c, rebuilt in the replay by --unit). So a recording is valid for every build whose
game side is the same, and one recording serves every helper:

    ~/goat-rig/rt2-bench/shared/<moment>[-classic][-at<s>][-s<settings>]@<key>/   (meta.json, manifest.json, used)

<key> is game_key(): a hash of the build's commit tree, every file but GAME_SIDE_EXCLUDED's (the replay side above,
docs, the workbench's own tools), and the platform (linux, windows). The id's other parts are the recipe: the
moment, --setting-off, --at and record's --set (the moment's own staging, tools/rt2_clips.py's MOMENTS entry and
settings, is hashed into the key too, since rt2_clips.py itself is excluded from the tree's part).

A recording is STALE (a new key, recorded again) when any of these change: the game's C (the guest, the host up to
the decoder: lt_lights.c's gather, lt_emitters.c, world_rt.c, dynamic_lights.c, the stream's format xgpu_stream.h,
the recorder, lt_lights.h / lt_lights_inputs.h's record layouts), the game's assets and their tools, the moment's
staging, the recipe. NOT stale: the shaders, the backend's C, the replay, lt_lights_build.c (a recording with
LT_INPUTS: replayed with --unit, else the light list plays as recorded), the docs and the workbench's Python. Not in
the key (each host's own, so recordings stay per host): the maps and the MCC import's data on the host.

One limit of the rule: the backend names the guest memory it reads (host_render_record_note), so a backend change
that reads guest memory it did not read before needs a new recording (the replay then fails to find it). Pass
`--name` to keep a private recording as before, or record with a new --set to get a new id.

record: a valid shared recording is reused (0 s); else copied from another Linux rig that has it (rsync between the
rigs), else recorded (into a part folder renamed when done; a second helper recording the same id waits for the
first). Afterwards the host's shared recordings are collected to its budget (MIN_FREE_GB, MAX_SHARED_GB): the least
recently used first, never one used in the last hour.

--host auto (verify, run, view, reference, terms, triage's default): each host probed at once (one ssh each): the
5900X only when its owner is not playing (checked again before every replay: rt2_bench.owner_check), bosgame only
with no game in the VM; its replays now (drawing or queued for goat-rig's 3 picture slots), a measuring run's lock,
whether it has the build and the recording. The host taken is the one where the command should end soonest
(score(): an exact try's time alone on that GPU, longer for each replay sharing it as measured, times the command's
work, plus a copy or a build first).
"""

from __future__ import annotations

import concurrent.futures
import hashlib
import inspect
import json
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

import rt2_clips

SHARED = "goat-rig/rt2-bench/shared"  # (under the rig's $HOME)
HOSTS = ("5900x", "omarchy", "bosgame")
AUTO_DEFAULT = ("verify", "run", "view", "reference", "terms", "triage")
# replays drawing at once per GPU: goat-rig's 3 picture slots (rt2_scorecard.remote_script); more wait for a slot
CAPS = {"5900x": 3, "omarchy": 3, "bosgame": 3}
MIN_FREE_GB = {"5900x": 60, "omarchy": 60, "bosgame": 15}
MAX_SHARED_GB = {"5900x": 40, "omarchy": 40, "bosgame": 12}
RECENT_SECONDS = 3600  # (a recording used within this is never collected)

# the files whose change leaves a recording valid: downstream of the recorder (the replay side), or no code
GAME_SIDE_EXCLUDED = (
    "port/linux/vulkan/*",  # the backend, the light pass's C and GLSL, the denoiser, DLSS (but see the next)
    "port/tools/replay/*",
    "port/linux/src/xgpu_spirv.c", "port/linux/src/xgpu_spirv.h",  # (the backend's shader translation)
    "port/linux/game/lt_lights_build.c",  # (the light-list unit: rebuilt by the replay from LT_INPUTS)
    "docs/*", "*.md", ".github/*",
    "tools/rt2_*", "tools/test_rt2_*", "tools/rig/*", "tools/scorecard/*", "tools/rt_truth/*",
)
GAME_SIDE_KEPT = (  # (in port/linux/vulkan, yet the game's: the window's guest and host halves, the imports)
    "port/linux/vulkan/guest_vulkan.c", "port/linux/vulkan/host_vulkan.c", "port/linux/vulkan/host_imports.list",
)


def say(*words):
    print(*words, file=sys.stderr, flush=True)


# ---------- the recording's key

def game_key(tree, platform, excluded, kept):
    """the game side's hash: `git ls-tree -r` lines ("<mode> <type> <object>\\t<path>"), those whose path is
    excluded (and not kept) left out, and the platform; 12 hex digits (its source runs on the rigs: KEY_SCRIPT)"""
    import fnmatch as _fnmatch
    import hashlib as _hashlib
    digest = _hashlib.sha256(f"platform {platform}\n".encode())
    for line in sorted(line for line in tree.splitlines() if "\t" in line):
        path = line.split("\t", 1)[1]
        if any(_fnmatch.fnmatchcase(path, p) for p in excluded) and path not in kept:
            continue
        digest.update(line.encode() + b"\n")
    return digest.hexdigest()[:12]


def recipe_hash(moment, clips=rt2_clips):
    """the moment's staging (rt2_clips.py's entry and its settings, both sides): in the key, as rt2_clips.py is
    excluded from the tree's part"""
    if moment not in clips.MOMENTS:
        raise SystemExit(f"rt2_bench: no moment {moment} in this checkout's tools/rt2_clips.py")
    entry = clips.MOMENTS[moment]
    text = json.dumps([repr(entry), clips.settings(moment, True), clips.settings(moment, False),
                       clips.SIZE, clips.FPS], sort_keys=True)
    return hashlib.sha256(text.encode()).hexdigest()[:8]


def shared_id(moment, key, setting_off=False, at=None, sets=()):
    """the shared recording's folder name: the moment, the recipe's parts that differ from the default, @key"""
    end = rt2_clips.MOMENTS[moment][2] if moment in rt2_clips.MOMENTS else None
    parts = [moment]
    if setting_off:
        parts.append("classic")
    if at == ANY_AT:
        parts.append("at" + ANY_AT)
    elif at is not None and (end is None or float(at) != float(end)):
        parts.append(f"at{float(at):g}")
    if sets:
        parts.append("s" + hashlib.sha256("\n".join(sorted(sets)).encode()).hexdigest()[:8])
    return "-".join(parts) + "@" + key


def platform_of(host):
    return "windows" if rt2_clips.windows(host) else "linux"


def builder_of(host):
    return rt2_clips.WINDOWS_BUILDER if rt2_clips.windows(host) else host


def tree_of(host, ref):
    # (goat-rig's worktree name, rt2_scorecard.build_folder: not imported, it needs numpy)
    return ref.replace("/", "-") + (".win" if rt2_clips.windows(host) else "")


KEY_SCRIPT = """
import json, sys
tree = sys.stdin.read()
{source}
print(game_key(tree, {platform!r}, {excluded!r}, {kept!r}))
"""


def key_command(host, ref, mirror=False):
    """a bash line printing '<sha> <key>' for the ref's build worktree (on the host's builder), or nothing; mirror:
    the ref's commit in the host's mirror instead (a guess for a ref not built there: goat-rig build fetches
    first)"""
    source = inspect.getsource(game_key)
    script = KEY_SCRIPT.format(source=source, platform=platform_of(host), excluded=GAME_SIDE_EXCLUDED,
                               kept=GAME_SIDE_KEPT)
    if mirror:
        git = "git -C ~/goat-rig/repo.git"
        names = " ".join(shlex.quote(n) for n in (f"refs/heads/{ref}", f"refs/remotes/origin/{ref}", ref))
        return (f"c=; for n in {names}; do c=$({git} rev-parse --verify -q \"$n^{{commit}}\") && break; done; "
                f"if [ -n \"$c\" ]; then echo \"${{c:0:9}} $({git} ls-tree -r $c | "
                f"python3 -c {shlex.quote(script)})\"; fi")
    tree = f"~/goat-rig/builds/{tree_of(host, ref)}"
    return (f"if git -C {tree} rev-parse HEAD >/dev/null 2>&1; then "
            f"echo \"$(git -C {tree} rev-parse --short HEAD) $(git -C {tree} ls-tree -r HEAD | "
            f"python3 -c {shlex.quote(script)})\"; fi")


def key_of(host, ref, recipe):
    """(sha, key) of the ref as built for the host; the key the recording's game side and the moment's staging"""
    from rt2_bench import ssh
    builder = builder_of(host)
    line = (ssh(builder, key_command(host, ref)) if not rt2_clips.windows(builder) else "").strip()
    if not line:
        raise SystemExit(f"rt2_bench: {ref} is not built for {host} (no worktree on {builder})")
    sha, key = line.split()
    return sha, f"{key}{recipe}"


# ---------- the shared folder on a host

def manifest_list(host):
    """the host's shared recordings: {id: {"manifest": ..., "mb": size, "age": seconds since last used}}"""
    from rt2_bench import ssh
    script = (f"mkdir -p ~/{SHARED}; cd ~/{SHARED}; now=$(date +%s); for d in */; do d=${{d%/}}; "
              "[ -f \"$d/meta.json\" ] || continue; u=$(stat -c %Y \"$d/used\" 2>/dev/null || stat -c %Y \"$d\"); "
              "echo \"R $d $(du -sm \"$d\" | cut -f1) $((now-u))\"; "
              "echo \"M $(tr -d '\\n\\r' < \"$d/manifest.json\" 2>/dev/null)\"; done; "
              "echo \"F $(df -Pk ~ | tail -1 | awk '{print $4}')\"")
    found, free_kb = {}, 0
    last = None
    for line in ssh(host, script).splitlines():
        if line.startswith("R "):
            _, name, mb, age = line.split()
            last = found[name] = {"mb": int(mb), "age": int(age), "manifest": {}}
        elif line.startswith("M ") and last is not None and line[2:].strip():
            try:
                last["manifest"] = json.loads(line[2:])
            except json.JSONDecodeError:
                pass
        elif line.startswith("F "):
            free_kb = int(line.split()[1] or 0)
    return found, free_kb / 1024 / 1024


def touch(host, folder):
    from rt2_bench import ssh
    ssh(host, f"touch ~/{folder}/used", check=False)


def remove_folder(host, folder):
    """a recording's folder removed (in the VM its data folder's junctions first: rm -rf would follow them)"""
    from rt2_bench import ssh
    unlink = rt2_clips.windows_unlink(f"{folder}/data") + "; " if rt2_clips.windows(host) else ""
    ssh(host, f"{unlink}rm -rf ~/{folder}", check=False)


def collect(host, keep=()):
    """the host's shared recordings collected to its budget: least recently used first, never one used within
    RECENT_SECONDS nor one in keep; stale part folders and verify folders over a day old too"""
    from rt2_bench import ssh
    ssh(host, f"cd ~/{SHARED} 2>/dev/null && find . -maxdepth 1 -name '.*.part-*' -mmin +1440 -print0 | "
              "xargs -0r rm -rf; find . -mindepth 2 -maxdepth 2 -name 'verify-*' -mmin +1440 -print0 | "
              "xargs -0r rm -rf; true", check=False)
    found, free_gb = manifest_list(host)
    total = sum(r["mb"] for r in found.values()) / 1024
    removed = []
    for name, r in sorted(found.items(), key=lambda kv: -kv[1]["age"]):
        if free_gb >= MIN_FREE_GB[host] and total <= MAX_SHARED_GB[host]:
            break
        if r["age"] < RECENT_SECONDS or name in keep:
            continue
        remove_folder(host, f"{SHARED}/{name}")
        free_gb += r["mb"] / 1024
        total -= r["mb"] / 1024
        removed.append(name)
    if removed:
        say(f"rt2_bench: {host}: collected {len(removed)} old shared recordings ({', '.join(removed)}); "
            f"{free_gb:.0f} GB free, {total:.1f} GB shared")
    return removed


# ---------- record: reuse, copy or record

def wanted_id(args, key):
    return shared_id(args.moment, key, args.setting_off, args.at, args.set)


REGISTRY = Path.home() / "dev" / "rt2-recordings.md"  # (the hand-kept list on the Mac: a row added per recording)


ANY_AT = "*"  # (shared_id's at for a query: any end second)


def parse_id(name):
    """a shared folder's name: (moment, classic, at or None, the settings' hash or None, key, generation), or None"""
    m = re.fullmatch(r"([A-Za-z0-9_]+)((?:-[a-z]+[^-@~]*)*)@([0-9A-Za-z]+)(?:~(\d+))?", name)
    if not m:
        return None
    moment, flags, key, gen = m.group(1), m.group(2), m.group(3), int(m.group(4) or 1)
    classic, at, sets = False, None, None
    for flag in [f for f in flags.split("-") if f]:
        if flag == "classic":
            classic = True
        elif flag.startswith("at"):
            at = flag[2:]
        elif flag.startswith("s"):
            sets = flag[1:]
    return moment, classic, at, sets, key, gen


def generation(name, rid):
    """name's generation when it is a recording rid asks for (rid's at "*": any end second), else 0"""
    a, b = parse_id(name), parse_id(rid)
    if not a or not b or a[:2] != b[:2] or a[3:5] != b[3:5]:
        return 0
    if b[2] != ANY_AT and a[2] != b[2]:
        return 0
    return a[5]


def newest(names, rid):
    """the recording among names that rid asks for: the moment's own end before another (any end second asked),
    then the newest generation (a --force-new recording supersedes the one before), or None"""
    found = [((parse_id(n)[2] is None, generation(n, rid)), n) for n in names if generation(n, rid)]
    return max(found)[1] if found else None


def everywhere(platform, hosts=HOSTS):
    """each host's shared recordings, probed at once: {host: (found, free_gb)} (an unreachable host left out)"""
    def one(host):
        try:
            return host, manifest_list(host)
        except SystemExit:
            return host, None
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        return {h: r for h, r in pool.map(one, [h for h in hosts if platform_of(h) == platform]) if r}


def record(args, record_fn, build_fn):
    """rt2_bench record without --name: REFUSED when a valid shared recording of the moment exists on any host of
    the platform (it says which: run/view/reference/terms/triage find it by --moment, copying it to the host they
    choose); else recorded once, into the shared folder, and listed (the host's records.log, rt2-recordings.md).
    --force-new REASON records a new generation anyway (rid~N, which supersedes the one before), the reason logged"""
    from rt2_bench import ssh, owner_check
    if args.host in (None, "auto"):
        args.host = "omarchy"
    started = time.monotonic()
    owner_check(args.host)
    build_fn(args.host, args.build)
    sha, key = key_of(args.host, args.build, recipe_hash(args.moment))
    rid = wanted_id(args, key)
    found = everywhere(platform_of(args.host))
    existing = [(h, newest(f, rid), f) for h, (f, _) in found.items() if newest(f, rid)]
    if not existing and args.at is None:
        # (no --at: a recording of the moment to another second counts too; --at S records to S)
        loose = shared_id(args.moment, key, args.setting_off, ANY_AT, args.set)
        existing = [(h, newest(f, loose), f) for h, (f, _) in found.items() if newest(f, loose)]
    if existing and not args.force_new:
        # (refused: the recording this would make is there already)
        for host, name, f in existing:
            m = f[name]["manifest"]
            say(f"rt2_bench: NOT RECORDED: the moment is recorded for this game side already: {host}:~/{SHARED}/"
                f"{name} (recorded {m.get('created', '?')} by {m.get('build', '?')} {m.get('sha', '?')})")
        here = [n for h, n, _ in existing if h == args.host]
        if args.name and here:
            alias(args.host, args.name, newest(here, rid))
        say(f"rt2_bench: use it: rt2_bench.py run|view|reference|terms|triage --moment {args.moment} --build "
            f"{args.build} (found by --moment, copied to the host chosen). Only a change to the game's side makes "
            "a new one (tools/rt2_bench_hosts.py's doc); else --force-new REASON")
        return 0
    if existing:
        top = max(parse_id(name)[5] for _, name, _ in existing if generation(name, rid)) if any(
            generation(name, rid) for _, name, _ in existing) else 0
        rid_new = f"{rid}~{top + 1}"
        say(f"rt2_bench: --force-new ({args.force_new}): recording {rid_new}, superseding "
            + ", ".join(f"{h}:{n}" for h, n, _ in existing))
    else:
        rid_new = rid
    folder = f"{SHARED}/{rid_new}"
    lock = f"{SHARED}/.{rid}.lock"
    if ssh(args.host, f"mkdir -p ~/{SHARED}; find ~/{lock} -maxdepth 0 -mmin +30 -exec rm -rf {{}} + 2>/dev/null; "
                      f"mkdir ~/{lock} 2>/dev/null && echo mine || echo taken").strip() != "mine":
        say(f"rt2_bench: {rid} is being recorded on {args.host} by another helper now; NOT RECORDED again. Wait "
            f"for it, then run with --moment {args.moment}")
        return 0
    part = f".{rid}.part-{int(time.time())}"
    name = args.name
    try:
        args.name = f"shared/{part}"
        code = record_fn(args)
        if code:
            return code
        manifest = {"id": rid_new, "key": key, "platform": platform_of(args.host), "moment": args.moment,
                    "setting_off": bool(args.setting_off), "at": args.at, "set": list(args.set),
                    "build": args.build, "sha": sha, "made_on": args.host,
                    "created": time.strftime("%Y-%m-%d %H:%M"), "reason": args.force_new or "first for its key"}
        ssh(args.host, f"cat > ~/{SHARED}/{part}/manifest.json <<'EOF'\n{json.dumps(manifest)}\nEOF\n"
                       f"touch ~/{SHARED}/{part}/used && mv ~/{SHARED}/{part} ~/{folder} && "
                       f"echo {shlex.quote(json.dumps(manifest))} >> ~/{SHARED}/records.log")
        say(f"rt2_bench: the shared recording {args.host}:~/{folder} (every helper's on this game side)")
        listed(args.host, manifest)
        if name:
            alias(args.host, name, rid_new)
    finally:
        args.name = name
        ssh(args.host, f"rm -rf ~/{lock}", check=False)
        if ssh(args.host, f"test -d ~/{SHARED}/{part} && echo left || true", check=False).strip() == "left":
            remove_folder(args.host, f"{SHARED}/{part}")
    collect(args.host, keep=(rid_new,))
    say(f"rt2_bench: {time.monotonic() - started:.0f} s")
    return 0


def alias(host, name, rid):
    """--name kept working: ~/goat-rig/rt2-bench/<name> a link to the shared recording (a Linux rig's; in the VM
    the shared folder's own name is used), unless a folder of that name is there"""
    from rt2_bench import ssh
    if rt2_clips.windows(host):
        say(f"rt2_bench: in the VM, --name {name}: use --moment (the shared recording shared/{rid})")
        return
    left = ssh(host, f"cd ~/goat-rig/rt2-bench && if [ -e {shlex.quote(name)} ] && [ ! -L {shlex.quote(name)} ]; "
                     f"then echo taken; else ln -sfn shared/{shlex.quote(rid)} {shlex.quote(name)}; fi").strip()
    if left == "taken":
        say(f"rt2_bench: ~/goat-rig/rt2-bench/{name} is another folder; use --moment (shared/{rid})")
    else:
        say(f"rt2_bench: --name {name} is the shared recording shared/{rid} on {host}")


def listed(host, m):
    """a row for the new recording in the hand-kept list on the Mac (rt2-recordings.md), when it is there"""
    try:
        if REGISTRY.exists():
            with REGISTRY.open("a") as f:
                f.write(f"| {m['moment']} ({'classic' if m['setting_off'] else 'on'}) | {host} | shared/{m['id']} | "
                        f"{m['build']} {m['sha']} | shared, by rt2_bench record {m['created']}; "
                        f"{m['reason']} |\n")
    except OSError:
        pass


def copy(source, dest, rid):
    """a shared recording copied between two Linux rigs (rsync from the destination: the rigs reach each other),
    into a part folder renamed when whole; the copy's manifest says where it came from"""
    from rt2_bench import ssh
    if rt2_clips.windows(source) or rt2_clips.windows(dest):
        raise SystemExit("rt2_bench: recordings are copied between the Linux rigs only (the VM records its own)")
    part = f".{rid}.part-{int(time.time())}"
    target = rt2_clips.rig_target(source)
    ssh(dest, f"set -e; mkdir -p ~/{SHARED}; rsync -a --exclude verify-* --exclude cache "
              f"{shlex.quote(target)}:{SHARED}/{shlex.quote(rid)}/ ~/{SHARED}/{part}/; "
              f"cd ~/{SHARED}/{part} && python3 -c \"import json; m = json.load(open('manifest.json')); "
              f"m['copied_from'] = '{source}'; json.dump(m, open('manifest.json', 'w'))\" && touch used && "
              f"cd .. && (mv -T {part} {shlex.quote(rid)} 2>/dev/null || rm -rf {part})")
    collect(dest, keep=(rid,))


# ---------- the hosts: probed, chosen

def probe_script(host, refs):
    """one bash script: the host's replays, GPU load and lock, owner, builds and shared recordings"""
    lines = [f"mkdir -p ~/{SHARED}"]
    if rt2_clips.windows(host):
        lines += ["echo \"replays $(tasklist //NH //FO CSV | grep -ic '^\\\"halo_replay')\"",
                  f"echo \"game $({rt2_clips.WINDOWS_GAME_RUNNING} | head -1 | tr -d '\\r\\n')\"",
                  "echo \"util $(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits "
                  "2>/dev/null | head -1 | tr -d '\\r ')\""]
        for ref in refs:
            lines.append(f"test -f ~/{rt2_clips_replay(host, ref)} && echo \"built {ref}\"")
    else:
        # (the replays drawing (argv0 halo_replay) or queued for one of the GPU's 3 picture slots: each replay's
        # outermost script once, its wrappers not counted again; the larger of the two counts)
        lines += ["echo \"replays $(ps -eo pid=,ppid=,args= | awk '{a=$0; sub(/^ *[0-9]+ +[0-9]+ +/, \"\", a)} "
                  "a ~ /^[^ ]*halo_replay( |$)/ {n++; next} a ~ /halo_replay/ && a ~ /gpu-shared/ {w[$1]=$2} "
                  "END {for (p in w) if (!(w[p] in w)) o++; print (o > n ? o : n) + 0}')\"",
                  f"echo \"game $(pgrep -fa \"{rt2_clips.OWNER_GAME}\" | head -1)\"",
                  "u=$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null | head -1);"
                  " [ -n \"$u\" ] || u=$(cat /sys/class/drm/card*/device/gpu_busy_percent 2>/dev/null | sort -n | "
                  "tail -1); echo \"util $u\"",
                  "flock -n -s ~/goat-rig/locks/gpu.lock true 2>/dev/null && echo 'lock free' || echo 'lock held'"]
        for ref in refs:
            lines.append(f"if test -x ~/{rt2_clips_replay(host, ref)}; then echo \"built {ref}\"; "
                         f"echo \"key {ref} $({key_command(host, ref)})\"; "
                         f"else echo \"key {ref} $({key_command(host, ref, mirror=True)})\"; fi")
    lines.append(f"cd ~/{SHARED} && for d in *@*/; do [ -f \"$d/manifest.json\" ] && echo \"have ${{d%/}}\"; "
                 "done; cd ~/goat-rig/rt2-bench && for d in */; do [ -f \"$d/meta.json\" ] && echo \"named ${d%/}\";"
                 " done; true")
    return "\n".join(lines) + "\n"


def rt2_clips_replay(host, ref):
    from rt2_bench import replay_path
    return replay_path(host, ref)


def probe(host, refs):
    """the host's state for choose(): replays, util, game, lock, built refs, keys, have (shared ids), named"""
    from rt2_bench import ssh
    state = {"host": host, "replays": 0, "util": 0.0, "game": "", "lock": "free", "built": set(), "keys": {},
             "have": set(), "named": set(), "error": None}
    try:
        text = ssh(host, probe_script(host, refs))
        if rt2_clips.windows(host):
            # (the Windows builds' keys: their worktrees are on the builder)
            keys = ssh(builder_of(host), "\n".join(f"echo \"key {ref} $({key_command(host, ref)})\"" for ref in refs))
            text += "\n" + keys
    except (SystemExit, OSError) as error:
        state["error"] = str(error)[-200:]
        return state
    for line in text.splitlines():
        word, _, rest = line.strip().partition(" ")
        if word == "replays":
            state["replays"] = int(rest or 0)
        elif word == "util":
            try:
                state["util"] = float(rest)
            except ValueError:
                pass
        elif word == "game":
            state["game"] = rest.strip()
        elif word == "lock":
            state["lock"] = rest.strip()
        elif word == "built":
            state["built"].add(rest.strip())
        elif word == "key":
            ref, _, value = rest.partition(" ")
            if value.split():
                state["keys"][ref] = tuple(value.split()[:2])
        elif word == "have":
            state["have"].add(rest.strip())
        elif word == "named":
            state["named"].add(rest.strip())
    return state


def score(state, needs, work=1):
    """the seconds the command is expected to take there (lower is better): an exact try's time on the host's GPU
    alone and its start (ALONE, OVERHEAD), longer by SLOWER for each replay sharing the GPU (measured), times the
    command's work in tries (WORK), plus what must be done
    first (its replay built, BUILD_SECONDS; each recording copied from another rig, COPY_SECONDS) and a measuring
    run's lock (LOCK_SECONDS). None: not eligible (its owner playing, a game in the VM, unreachable, or a
    recording it lacks and cannot be given)"""
    host = state["host"]
    if state["error"] or state["game"]:
        return None
    s = (ALONE[host] + OVERHEAD[host]) * (1 + SLOWER[host] * state["replays"]) * work
    if state["lock"] == "held":
        s += LOCK_SECONDS
    for need in needs:
        if need in state["ready"]:
            continue
        if need[0] == "built":
            s += BUILD_SECONDS
        elif need[0] == "shared" and need[1] != "?" and state["copyable"].get(need):
            s += COPY_SECONDS
        else:
            return None  # (a recording it cannot be given, or no guess of its key: not sent there on spec)
    return s


# measured 2026-11-28 on a50_shields (1300-1380 frames, every frame whole): one exact try alone on each GPU (its
# replay's seconds), the hops' and start's seconds besides, and how much longer a try takes for each replay beside it
# (omarchy: 3 at once 118 s each, 6 at once 106 s then 204 s, the 3 picture slots: its throughput stays one try's;
# the 4090: 6 tries 4 s apart 14-24 s each; the VM: 2 at once 33 s each)
ALONE = {"5900x": 9.3, "omarchy": 35.0, "bosgame": 19.7}
OVERHEAD = {"5900x": 2.0, "omarchy": 3.0, "bosgame": 8.0}
SLOWER = {"5900x": 0.4, "omarchy": 1.2, "bosgame": 0.7}
COPY_SECONDS = 50.0  # (1.8 GB rig to rig, rsync: 50 s measured 5900X to omarchy)
BUILD_SECONDS = 30.0  # (the replay built warm)
LOCK_SECONDS = 60.0
# a command's work in exact tries (run --watch: a session of tries)
WORK = {"run": 1, "view": 1, "verify": 4, "reference": 6, "terms": 12, "triage": 15}


def why(state):
    if state["error"]:
        return "unreachable: " + state["error"][-80:]
    if state["game"]:
        return ("a game in the VM: " if rt2_clips.windows(state["host"]) else "its owner is playing: ") + \
            state["game"][:60]
    return "lacks " + ", ".join(n[1] for n in state["needs"] if n not in state["ready"] and n[0] != "built") + \
        " and no rig can copy it"


def choose(hosts, needs_of, describe=True, work=1):
    """the least-busy eligible host: (host, its state, the states of all); needs_of(state) gives what the command
    needs on it and which of those it has (sets state['ready'], state['copyable'])"""
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        states = list(pool.map(lambda h: probe(h, needs_of(None)), hosts))
    for state in states:
        state["needs"], state["ready"] = needs_of(state)
    scored = []
    for state in states:
        state["copyable"] = {need: True for need in state["needs"] if need[0] == "shared" and not
                             rt2_clips.windows(state["host"]) and
                             any(o["host"] != state["host"] and not rt2_clips.windows(o["host"]) and
                                 newest(o["have"], need[1]) for o in states)}
        state["score"] = score(state, state["needs"], work)
        if state["score"] is not None:
            scored.append(state)
    if describe:
        say("rt2_bench: hosts: " + "; ".join(
            f"{s['host']} " + (f"not eligible ({why(s)})"
                               if s["score"] is None else
                               f"{s['replays']}/{CAPS[s['host']]} replays, GPU {s['util']:.0f}%"
                               f"{', lock held' if s['lock'] == 'held' else ''}"
                               f"{'' if all(n in s['ready'] for n in s['needs']) else ', ' + ', '.join(n[0] + ' missing' for n in s['needs'] if n not in s['ready'])}"
                               f" (about {s['score']:.0f} s)") for s in states))
    if not scored:
        raise SystemExit("rt2_bench: no host is eligible now: " + "; ".join(f"{s['host']} {why(s)}" for s in
                                                                         states) +
                         ". A recording no rig has: record it once (rt2_bench.py record --moment ... --build ...)")
    best = min(scored, key=lambda s: (s["score"], HOSTS.index(s["host"])))
    return best["host"], best, states


# ---------- resolving a command's host and recordings

def needs_for(args):
    """the function choose() asks: the refs to probe (state None), else (needs, ready) for the host's state. A need
    is ("built", ref), ("named", name) or ("shared", base id: any generation)"""

    def needs_of(state):
        if state is None:
            return [args.build]
        needs, ready = [("built", args.build)], set()
        if args.build in state["built"]:
            ready.add(("built", args.build))
        names = [args.name] if args.name else []
        if args.what == "triage" and args.classic and args.classic not in ("auto", "shared"):
            names.append(args.classic)
        for name in names:
            need = ("named", name)
            needs.append(need)
            if name in state["named"]:
                ready.add(need)
        key = state["keys"].get(args.build)
        if args.name is None and key:
            for rid in wanted_ids(args, key[1]):
                need = ("shared", rid)
                needs.append(need)
                if newest(state["have"], rid):
                    ready.add(need)
        elif args.name is None:
            needs.append(("shared", "?"))  # (not built there: its key unknown until it is)
        return needs, ready

    return needs_of


def wanted_ids(args, tree_key):
    """the shared recordings a command asks for: the moment's (to --at's second, else any), and triage --classic
    auto's classic one"""
    recipe = recipe_hash(args.moment)
    at = ANY_AT if args.at is None else args.at
    ids = [shared_id(args.moment, tree_key + recipe, at=at)]
    if args.what == "triage" and args.classic in ("auto", "shared"):
        ids.append(shared_id(args.moment, tree_key + recipe, setting_off=True, at=at))
    return ids


def resolve(args, build_fn):
    """args.host chosen (auto) and args.name / args.classic pointed at the recordings to use, every one present on
    the host (its replay built, the recordings copied there first); never records"""
    if args.what == "record":
        return
    if args.host in (None, "auto"):
        hosts = list(HOSTS) if args.what in AUTO_DEFAULT or args.host == "auto" else ["omarchy"]
    else:
        hosts = [args.host]
    needs_of = needs_for(args)
    work = WORK.get(args.what, 1) * (5 if getattr(args, "watch", False) else 1)
    host, state, states = choose(hosts, needs_of, describe=len(hosts) > 1, work=work)
    tried = []
    while True:
        try:
            provide(args, host, state, states, needs_of, build_fn)
            return
        except SystemExit as error:
            # (its key guessed wrong, a copy failed: the next host, if any)
            tried.append(host)
            rest = sorted((s for s in states if s["score"] is not None and s["host"] not in tried),
                          key=lambda s: s["score"])
            if not rest or len(hosts) == 1:
                raise
            say(f"rt2_bench: {host}: {str(error)[:200]}; trying {rest[0]['host']}")
            host, state = rest[0]["host"], rest[0]


def provide(args, host, state, states, needs_of, build_fn):
    args.host = host
    if ("built", args.build) not in state["ready"]:
        say(f"rt2_bench: building {args.build}'s replay on {host}")
        build_fn(host, args.build, game=False)
        state = probe(host, [args.build])
        state["needs"], state["ready"] = needs_of(state)
    if args.name is None:
        key = state["keys"].get(args.build)
        if not key:
            raise SystemExit(f"rt2_bench: {args.build} has no worktree on {builder_of(host)}")
        ids = wanted_ids(args, key[1])
        got = []
        for rid in ids:
            name = newest(state["have"], rid)
            if not name:
                name = bring(host, rid, [s for s in states if s["host"] != host])
            got.append(name)
            touch(host, f"{SHARED}/{name}")
        args.name = f"shared/{got[0]}"
        if len(got) > 1:
            args.classic = f"shared/{got[1]}"
        say(f"rt2_bench: {args.what} on {host}, the recording {args.name}"
            + (f", classic {args.classic}" if args.what == "triage" and args.classic else ""))
    else:
        missing = [n for n in state["needs"] if n[0] == "named" and n not in state["ready"]]
        if missing:
            raise SystemExit(f"rt2_bench: {host} has no recording {', '.join(n[1] for n in missing)} "
                             "(record without --name for a shared one)")


def bring(host, rid, others):
    """the newest generation of rid copied to host from a Linux rig with it; its name. None anywhere: refused,
    with the record command (a run never records)"""
    if not rt2_clips.windows(host):
        sources = [(newest(s["have"], rid), s["host"]) for s in others
                   if not rt2_clips.windows(s["host"]) and newest(s["have"], rid)]
        if sources:
            name, source = max(sources, key=lambda x: generation(x[0], rid))
            started = time.monotonic()
            say(f"rt2_bench: copying the shared recording {name} from {source} to {host}")
            copy(source, host, name)
            say(f"rt2_bench: copied in {time.monotonic() - started:.0f} s")
            return name
    raise SystemExit(f"rt2_bench: no shared recording {rid} on {host}" +
                     ("" if rt2_clips.windows(host) else " or another Linux rig") +
                     ": this game side has none yet. Record it once: rt2_bench.py record --moment ... --build ... "
                     f"--host {host}" + (" --setting-off" if "-classic" in rid else ""))


# ---------- files back to the Mac (bosgame: through the hops; no scp in the VM's path)

def fetch(host, folder, names, out):
    """files of a rig folder (under its home) copied into the Mac's out folder"""
    out.mkdir(parents=True, exist_ok=True)
    if not names:
        return
    if rt2_clips.windows(host):
        listed = " ".join(shlex.quote(n) for n in names)
        source = subprocess.Popen(rt2_clips.remote_argv(host, f"cd ~/{folder} && tar -cf - {listed}"),
                                  stdout=subprocess.PIPE)
        subprocess.run(["tar", "-xf", "-", "-C", str(out)], stdin=source.stdout, check=False)
        source.stdout.close()
        source.wait()
    else:
        subprocess.run(["scp", "-q", *[f"{rt2_clips.rig_target(host)}:{folder}/{n}" for n in names], str(out)],
                       check=False)


# ---------- migration of existing recordings (once; helpers that have finished only)

def recipe_at(builder, sha, moment):
    """recipe_hash() by the commit's own tools/rt2_clips.py (a recording made before: its staging then)"""
    import importlib.util
    import tempfile
    from rt2_bench import ssh
    text = ssh(builder, f"git -C ~/goat-rig/repo.git show {shlex.quote(sha)}:tools/rt2_clips.py")
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "rt2_clips_then.py"
        path.write_text(text)
        spec = importlib.util.spec_from_file_location(f"rt2_clips_{sha}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    return recipe_hash(moment, module)


def migrate(host, name, dry=False):
    """a helper's recording ~/goat-rig/rt2-bench/<name> MOVED into the shared folder under its key (its build's
    commit must be in the builder's mirror): (its id, whether that id was there already: then left alone)"""
    from rt2_bench import ssh, meta_of
    meta = meta_of(host, f"goat-rig/rt2-bench/{name}")
    mirror = "~/goat-rig/repo.git"
    builder = builder_of(host)
    platform = platform_of(host)
    source = inspect.getsource(game_key)
    script = KEY_SCRIPT.format(source=source, platform=platform, excluded=GAME_SIDE_EXCLUDED, kept=GAME_SIDE_KEPT)
    key = ssh(builder, f"git -C {mirror} ls-tree -r {shlex.quote(meta['sha'])} | python3 -c {shlex.quote(script)}"
              ).strip()
    if not re.fullmatch(r"[0-9a-f]{12}", key):
        raise SystemExit(f"rt2_bench: {name}: its commit {meta['sha']} is not in {builder}'s mirror")
    off = meta.get("lighting") == "classic"
    recipe = recipe_at(builder, meta["sha"], meta["moment"])
    rid = shared_id(meta["moment"], key + recipe, off, meta.get("at"))
    have, _ = manifest_list(host)
    if dry or rid in have:
        return rid, rid in have
    manifest = {"id": rid, "key": key + recipe, "platform": platform,
                "moment": meta["moment"], "setting_off": off, "at": meta.get("at"), "set": [],
                "build": meta["build"], "sha": meta["sha"], "made_on": host, "created": meta.get("recorded"),
                "migrated_from": name}
    q, r = shlex.quote(name), shlex.quote(rid)
    # (moved, not copied; on a Linux rig the old name left as a link to it, so its owner's --name still works)
    ssh(host, f"set -e; cd ~/goat-rig/rt2-bench; mkdir -p shared; cat > {q}/manifest.json <<'EOF'\n"
              f"{json.dumps(manifest)}\nEOF\ntouch {q}/used; mv {q} shared/{r}" +
        (f"; cmd //c mklink //J \"$(cygpath -w ~/goat-rig/rt2-bench)\\\\{name}\" "
         f"\"$(cygpath -w ~/goat-rig/rt2-bench/shared/{rid})\" >/dev/null" if rt2_clips.windows(host) else
         f"; ln -sfn shared/{r} {q}"))
    return rid, False
