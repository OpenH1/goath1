# Syncing with upstream (OpenCE)

GOAT H1 is a fork of [OpenCE](https://github.com/OpenCommunityEdition/OpenCE).
Its `main` is merged into `dev` from time to time. Every Monday the
[Upstream trial merge](../.github/workflows/upstream-trial-merge.yml) workflow
tries that merge in a runner (`tools/upstream_trial_merge.py`) and updates one
tracking issue, *Weekly OpenCE trial merge*: upstream's new commits since the
last sync, the files that would conflict, grouped by area, and any change of
the network version. It pushes nothing. Run it by hand from the Actions tab,
or locally:

```sh
git remote add opence https://github.com/OpenCommunityEdition/OpenCE.git  # once
git fetch opence main
tools/upstream_trial_merge.py --base origin/dev --upstream opence/main
```

## What wins

- **Classic (Legacy) online play** (the OpenCE-compatible protocol,
  `HALO_PORT_NETWORK_VERSION`, message formats, `port/linux/network_format.txt`,
  the master server protocol): **theirs**, exactly. The goal is to play with
  OpenCE's current release. The classic numbers (messages below 120, the
  bits of `cooperative_flags`) come only from upstream.
- **GOAT H1's own protocol** (`HALO_PORT_STANDARD_NETWORK_VERSION`, our
  messages from 120, the game record's `standard_flags`), our lobby, co-op
  flow, parties and friends, Spartan Link, host controls: **ours**, kept
  alongside theirs. When upstream raises `HALO_PORT_NETWORK_VERSION`, raise
  the Standard version and its `HALO_PORT_STANDARD_BASE_VERSION` with it
  (`tools/netver.py change`, then `tools/netver.py bump`, and set the base:
  port/linux/NETCODE.md, "Numbers on parallel branches"). If upstream takes a
  number of ours, ours moves (the trial merge's report lists such clashes).
- **Gameplay and engine behavior** (`source/`, game rules, map support,
  dedicated server changes): **theirs**, unless it undoes one of our
  documented fixes (keep our fix and say so in the pull request). Anything
  that changes the simulation is gated so that Classic play matches
  (e.g. `network_standard_enhanced_animations()`).
- **Graphics** (renderer, Vulkan, Metal, VR, the 64-bit hosts): **ours**.
- **Sound**: **ours**. Upstream's ports of our own sound work are not taken
  again; their bug fixes are, adapted to our code, when our code has the bug.
- **Loading screen**: **ours**.
- **Menus**: **ours** (our redesigned and generated menus). Upstream's new
  options must be reachable from our menus: add them through the generator
  (`tools/port_settings.py`, then `tools/menus_regen.sh`), never by editing
  generated files. A conflict in a generated file is resolved by
  `tools/menus_regen.sh --resolve` once the sources are resolved.
- **Never take**: upstream's site-only pieces (its analytics, its web
  build's hosting).
- Upstream commits keep their own history and trailers. Our commits credit
  no AI tool and name no one.

## How to work

- Merge `opence/main` into a branch made from `dev`
  (`merge-opence-<year>-<month>`). A big sync can be committed once with
  its conflict markers, then split into lanes (engine, network, menus,
  platform, sound and graphics), each owning a list of files and editing
  only those; a change needed outside the list goes in the lane's report.
- Understand each conflict before choosing:
  `git log -p <merge base>..opence/main -- <file>` and
  `git log -p <merge base>..origin/dev -- <file>`. Our side may already
  hold a port of their change under another commit (many OpenCE pull
  requests were ported by hand): keep one copy, not both.
- Binary files: take one side whole
  (`git show opence/main:<path> > <path>` or `git show origin/dev:<path> > <path>`).
- Resolve every marker:
  `grep -n '^<<<<<<<\|^=======$\|^>>>>>>>' <files>`.
- Then build, run the tests, and check that classic play still matches
  (`tools/scorecard/identity.py --repeat`).
- Report for each notable file what was taken from each side, anything
  uncertain, and what a later build or test must check.
