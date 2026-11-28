"""The workbench's shared recordings and host choice (tools/rt2_bench_hosts.py): what makes a recording stale, and
which host a try goes to. No rig: the key over made-up trees, the choice over made-up probes."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rt2_bench_hosts as hosts  # noqa: E402

TREE = {
    "port/linux/game/lt_lights.c": "a1",
    "port/linux/game/lt_lights.h": "a2",
    "port/linux/game/lt_lights_inputs.h": "a3",
    "port/linux/game/lt_lights_build.c": "a4",
    "port/linux/src/xgpu_stream.h": "a5",
    "port/linux/src/xgpu_spirv.c": "a6",
    "port/linux/vulkan/lt/e1.frag": "a7",
    "port/linux/vulkan/xgpu_vulkan_lt.c": "a8",
    "port/linux/vulkan/guest_vulkan.c": "a9",
    "port/tools/replay/host_render_replay.c": "b1",
    "port/android/host/host_render_record.c": "b2",
    "docs/replay_protocol.md": "b3",
    "tools/rt2_bench.py": "b4",
    "tools/port_settings.py": "b5",
}


def tree(**changed):
    files = dict(TREE)
    for path, blob in changed.items():
        files[path.replace("__", "/").replace("_DOT_", ".")] = blob
    return "\n".join(f"100644 blob {blob}\t{path}" for path, blob in files.items())


def gk(text, platform):
    return hosts.game_key(text, platform, hosts.GAME_SIDE_EXCLUDED, hosts.GAME_SIDE_KEPT)


def key(**changed):
    return hosts.game_key(tree(**changed), "linux", hosts.GAME_SIDE_EXCLUDED, hosts.GAME_SIDE_KEPT)


def changed(path):
    files = dict(TREE)
    files[path] = "zz"
    return gk("\n".join(f"100644 blob {b}\t{p}" for p, b in files.items()), "linux")


class RecordingKey(unittest.TestCase):
    def test_stale_when_the_game_side_changes(self):
        base = key()
        for path in ("port/linux/game/lt_lights.c", "port/linux/game/lt_lights.h",
                     "port/linux/game/lt_lights_inputs.h", "port/linux/src/xgpu_stream.h",
                     "port/android/host/host_render_record.c", "port/linux/vulkan/guest_vulkan.c",
                     "tools/port_settings.py"):
            self.assertNotEqual(changed(path), base, path)

    def test_valid_when_only_the_replay_side_changes(self):
        base = key()
        for path in ("port/linux/vulkan/lt/e1.frag", "port/linux/vulkan/xgpu_vulkan_lt.c",
                     "port/linux/src/xgpu_spirv.c", "port/tools/replay/host_render_replay.c",
                     "port/linux/game/lt_lights_build.c", "docs/replay_protocol.md", "tools/rt2_bench.py"):
            self.assertEqual(changed(path), base, path)

    def test_a_new_game_file_is_stale(self):
        self.assertNotEqual(gk(tree() + "\n100644 blob c1\tport/linux/game/new.c", "linux"), key())

    def test_platform_and_order(self):
        self.assertNotEqual(gk(tree(), "windows"), key())
        lines = tree().splitlines()
        self.assertEqual(gk("\n".join(reversed(lines)), "linux"), key())

    def test_the_rigs_compute_the_same_key(self):
        # (the rig runs game_key's own source: KEY_SCRIPT)
        import subprocess
        script = hosts.KEY_SCRIPT.format(source=__import__("inspect").getsource(hosts.game_key), platform="linux",
                                         excluded=hosts.GAME_SIDE_EXCLUDED, kept=hosts.GAME_SIDE_KEPT)
        out = subprocess.run([sys.executable, "-c", script], input=tree(), text=True, capture_output=True,
                             check=True).stdout.strip()
        self.assertEqual(out, key())

    def test_ids(self):
        self.assertEqual(hosts.shared_id("a50_shields", "k"), "a50_shields@k")
        self.assertEqual(hosts.shared_id("a50_shields", "k", setting_off=True), "a50_shields-classic@k")
        end = hosts.rt2_clips.MOMENTS["a50_shields"][2]
        self.assertEqual(hosts.shared_id("a50_shields", "k", at=end), "a50_shields@k")
        self.assertNotEqual(hosts.shared_id("a50_shields", "k", at=end - 1), "a50_shields@k")
        self.assertNotEqual(hosts.shared_id("a50_shields", "k", sets=["HALO_X=1"]), "a50_shields@k")

    def test_any_end_second(self):
        names = {"a50_shields-at46@k", "a50_shields-at40@k~2", "a50_shields-classic-at46@k", "a50_shields@kk"}
        self.assertEqual(hosts.newest(names, "a50_shields-at*@k"), "a50_shields-at40@k~2")
        self.assertEqual(hosts.newest(names | {"a50_shields@k"}, "a50_shields-at*@k"), "a50_shields@k")
        self.assertEqual(hosts.newest(names, "a50_shields-at46@k"), "a50_shields-at46@k")
        self.assertEqual(hosts.newest(names, "a50_shields-classic-at*@k"), "a50_shields-classic-at46@k")
        self.assertEqual(hosts.newest(names, "a50_shields-s12345678-at*@k"), None)

    def test_generations(self):
        names = {"a@k", "a@k~3", "a@k~2", "a-classic@k", "a@kk"}
        self.assertEqual(hosts.newest(names, "a@k"), "a@k~3")
        self.assertEqual(hosts.newest({"a@kk"}, "a@k"), None)
        self.assertEqual(hosts.newest({"a-classic@k"}, "a-classic@k"), "a-classic@k")


def state(host, replays=0, util=0, game="", lock="free", have=(), built=True):
    return {"host": host, "replays": replays, "util": util, "game": game, "lock": lock, "error": None,
            "built": {"b"} if built else set(), "keys": {"b": ("sha", "k")}, "have": set(have), "named": set()}


class HostChoice(unittest.TestCase):
    def pick(self, states, what="run"):
        import argparse
        args = argparse.Namespace(what=what, build="b", name=None, moment="a50_shields", classic=None, at=None)
        # (what a helper recorded: the moment to 46 s; run asks for it by --moment alone)
        rid = hosts.shared_id("a50_shields", "k" + hosts.recipe_hash("a50_shields"), at=46)
        by_host = {s["host"]: s for s in states}
        for s in states:
            s["have"] = {rid if h == "R" else h for h in s["have"]}
        original = hosts.probe
        hosts.probe = lambda host, refs: by_host[host]
        try:
            return hosts.choose([s["host"] for s in states], hosts.needs_for(args), describe=False,
                                work=hosts.WORK[what])[0]
        finally:
            hosts.probe = original

    def test_least_busy_with_the_recording(self):
        self.assertEqual(self.pick([state("5900x", replays=2, have=["R"]), state("omarchy", replays=0, have=["R"]),
                                    state("bosgame", replays=0)]), "5900x")
        self.assertEqual(self.pick([state("5900x", replays=8, have=["R"]), state("omarchy", replays=0, have=["R"]),
                                    state("bosgame", replays=0)]), "omarchy")

    def test_owner_playing_never_chosen(self):
        self.assertEqual(self.pick([state("5900x", have=["R"], game="openh1"), state("omarchy", replays=2,
                                                                                    have=["R"])]), "omarchy")

    def test_copy_when_the_holder_is_full(self):
        # (16 replays on the 5900X: for a triage, copying to an idle omarchy beats waiting; for one try it does not)
        self.assertEqual(self.pick([state("5900x", replays=16, have=["R"]), state("omarchy")], "triage"), "omarchy")
        self.assertEqual(self.pick([state("5900x", replays=16, have=["R"]), state("omarchy")]), "5900x")

    def test_no_copy_when_the_holder_has_room(self):
        self.assertEqual(self.pick([state("5900x", replays=1, util=40, have=["R"]), state("omarchy")]), "5900x")

    def test_the_vm_cannot_be_given_a_linux_recording(self):
        self.assertEqual(self.pick([state("5900x", replays=16, util=100, have=["R"]), state("bosgame")]), "5900x")

    def test_none_eligible(self):
        with self.assertRaises(SystemExit):
            self.pick([state("5900x", game="openh1", have=["R"]), state("omarchy", game="x")])


if __name__ == "__main__":
    unittest.main()
