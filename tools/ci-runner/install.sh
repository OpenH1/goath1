#!/bin/bash
# tools/ci-runner/install.sh <omarchy|5900x> [count]: GitHub Actions runners of OpenH1/goath1 on a test machine
# (<name>.fabric as $GOAT_RIG_USER, the machines' user), from a checkout with gh signed in as a repository admin. No root needed: each runner
# runs sandboxed in a systemd user service (tools/ci-runner/host.sh). Needs goat-rig (tools/rig/install.sh) for
# its compile cache, and omarchy's docker to build the jobs' file system. Run it again to update the kit or the
# image (when the machine is idle: its runners restart); count 0 removes the machine's runners.
# The workflow sends only trusted accounts' jobs here (build.yml, changes job's Pick the runner).
set -euo pipefail
host=${1:?usage: tools/ci-runner/install.sh <omarchy|5900x> [count]}
case $host in
  # (the 5900X is for games first: 20 of its 24 cores, idle priority, and its runners stop while a game plays)
  omarchy) count=${2:-1} nice=10 slice='CPUWeight=50
MemoryHigh=36G
MemoryMax=40G' ;;
  5900x) count=${2:-1} nice=19 slice='CPUWeight=idle
AllowedCPUs=4-23
IOWeight=10
MemoryHigh=12G
MemoryMax=16G' ;;
  *) echo "omarchy or 5900x" >&2; exit 2 ;;
esac
user=${GOAT_RIG_USER:?set GOAT_RIG_USER to the test machines\' user}
target=$user@$host.fabric
builder=$user@omarchy.fabric
repo=OpenH1/goath1
root=$(cd "$(dirname "$0")/../.." && pwd)
# (the runner's release: it updates itself afterwards)
runner_version=2.338.2
runner_sha256=baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40

# the kit (the 5900X's login shell is fish: plain commands only)
ssh "$target" 'mkdir -p goat-rig/ci/bin goat-rig/ci/runners .config/systemd/user'
COPYFILE_DISABLE=1 tar --no-xattrs -C "$root/tools/ci-runner" -cf - host.sh | ssh "$target" 'tar -C goat-rig/ci/bin -xf -'

# the jobs' file system: .github/ci-image's image, built on the builder
start=start
image=$(COPYFILE_DISABLE=1 tar --no-xattrs -C "$root/.github/ci-image" -cf - . |
  ssh "$builder" 'docker build -q --pull --build-arg ARCH=x86_64 -t goath1-ci-rootfs -')
if [ "$(ssh "$target" 'cat goat-rig/ci/rootfs/.image-id 2>/dev/null' || true)" != "$image" ]; then
  echo "unpacking $image on $host"
  start=restart
  ssh "$builder" "cid=\$(docker create $image) && docker export \$cid; docker rm \$cid > /dev/null" |
    ssh "$target" "goat-rig/ci/bin/host.sh rootfs $image"
fi
ssh "$target" "goat-rig/ci/bin/host.sh runner $runner_version $runner_sha256"

# the services
ssh "$target" 'cat > .config/systemd/user/goath1-ci.slice' <<EOF
[Unit]
Description=OpenH1/goath1's CI runners
[Slice]
$slice
EOF
ssh "$target" 'cat > .config/systemd/user/goath1-runner@.service' <<EOF
[Unit]
Description=GitHub Actions runner %i of $repo (tools/ci-runner)
After=network-online.target
[Service]
Slice=goath1-ci.slice
ExecStart=%h/goat-rig/ci/bin/host.sh run %i
Nice=$nice
IOSchedulingClass=idle
KillSignal=SIGINT
TimeoutStopSec=30
Restart=always
RestartSec=10
[Install]
WantedBy=default.target
EOF
if [ "$host" = 5900x ]; then
  ssh "$target" 'cat > .config/systemd/user/goath1-runner-guard.service' <<'EOF'
[Unit]
Description=goath1 runners stopped while a game plays
[Service]
Type=oneshot
ExecStart=%h/goat-rig/ci/bin/host.sh guard
EOF
  ssh "$target" 'cat > .config/systemd/user/goath1-runner-guard.timer' <<'EOF'
[Timer]
OnBootSec=30s
OnUnitActiveSec=20s
AccuracySec=2s
[Install]
WantedBy=timers.target
EOF
fi
ssh "$target" 'systemctl --user daemon-reload'

# the runners: <host>-1 to <host>-<count> (registration tokens last an hour, and go on stdin only)
for i in $(seq 1 "$count"); do
  name=$host-$i
  if ! ssh "$target" "test -f goat-rig/ci/runners/$name/.runner"; then
    gh api -X POST "repos/$repo/actions/runners/registration-token" --jq .token |
      ssh "$target" "goat-rig/ci/bin/host.sh add $name $runner_version"
  fi
  ssh "$target" "systemctl --user enable goath1-runner@$name.service && systemctl --user $start goath1-runner@$name.service"
done
for name in $(ssh "$target" 'goat-rig/ci/bin/host.sh list'); do
  [ "${name#"$host"-}" -le "$count" ] 2>/dev/null && continue
  echo "removing $name"
  gh api -X POST "repos/$repo/actions/runners/remove-token" --jq .token |
    ssh "$target" "goat-rig/ci/bin/host.sh remove $name"
done
[ "$host" != 5900x ] || ssh "$target" 'systemctl --user enable --now goath1-runner-guard.timer'
ssh "$target" 'goat-rig/ci/bin/host.sh tidy'
echo "$host: $count runner(s); gh api repos/$repo/actions/runners lists them"
