"""rt2_bench.py run's kept replay: one replay service (halo_replay --serve) per (rig, recording, replay build) that
outlives the run that started it, so the light pass's checkpoint it keeps (rt_checkpoint, in the GPU's memory) serves
every later run: a try in a couple of seconds, not a play of the recording from its start.

This file runs ON THE RIG (python3 -c <this source> connect ...; rt2_bench.py sends it inline as base64, nothing
installed):

    connect <recording> <replay> <build> <replay script>
        joins the kept replay for (recording, replay build), starting it if there is none (detached, setsid), and
        bridges standard input and output to it: the replay's protocol, a JSON request a line, a reply a line. Its
        first line out is {"keep": "warm" | "cold", "why": ..., "state": {"checkpoint": F | null, "applied": {...}}}:
        what the kept replay holds (the frame its checkpoint was kept at, the settings it was last given).

The kept replay (serve, the forked daemon) takes the GPU lock as goat-rig's picture runs do (gpu.lock shared and one
of its 3 picture slots) only while a run is joined, so an idle one blocks no measuring run; it serves one run at a
time (the next waits), and ends, writing why in keep/<key>.ended, after IDLE seconds unjoined, when the machine's
owner starts playing, when its replay or recording file changes (a rebuild: another build, another service), or to
make room (at most MOST kept per rig, the least recently used idle one ends). Its socket, state and log are in
~/goat-rig/rt2-bench/keep/<key>.*, key = the recording's and the replay's paths hashed.
"""

import base64
import fcntl
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
KEEP = os.path.join(HOME, "goat-rig", "rt2-bench", "keep")
LOCKS = os.path.join(HOME, "goat-rig", "locks")
IDLE = 20 * 60
MOST = 3
OWNER_GAME = f"^{HOME}/Applications/OpenH1[^/ ]*/bin/openh1"


def identity(path):
    s = os.stat(os.path.join(HOME, path))
    return [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns]


def paths(key):
    base = os.path.join(KEEP, key)
    named = {k: f"{base}.{k}" for k in ("json", "ended", "log", "start", "busy")}
    # (the socket by its name in KEEP, the working folder: a socket's path is limited to about 100 bytes)
    return {**named, "sock": f"{key}.sock"}


def owner_playing():
    return subprocess.run(["pgrep", "-f", OWNER_GAME], capture_output=True).returncode == 0


def write_json(path, value):
    with open(path + ".part", "w") as f:
        json.dump(value, f)
    os.replace(path + ".part", path)


# ---------- the kept replay (the daemon)

class Kept:
    def __init__(self, key, recording, replay, build, script):
        self.key, self.recording, self.replay, self.build, self.script = key, recording, replay, build, script
        self.p = paths(key)
        self.ids = [identity(recording), identity(replay)]
        self.process = None
        self.state = {"checkpoint": None, "applied": {}}
        self.info = {"key": key, "recording": recording, "replay": replay, "build": build, "pid": os.getpid(),
                     "started": time.time(), "used": time.time(), "runs": 0}
        self.locks = []
        self.ending = None

    def save(self):
        write_json(self.p["json"], self.info)

    def lock(self):
        """goat-rig's picture-run lock: gpu.lock shared, then one of its 3 picture slots"""
        os.makedirs(LOCKS, exist_ok=True)
        gpu = open(os.path.join(LOCKS, "gpu.lock"), "a")
        fcntl.flock(gpu, fcntl.LOCK_SH)
        while True:
            for s in range(3):
                slot = open(os.path.join(LOCKS, f"gpu-shared-{s}.lock"), "a")
                try:
                    fcntl.flock(slot, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    self.locks = [gpu, slot]
                    return
                except OSError:
                    slot.close()
            time.sleep(0.5)

    def unlock(self):
        for f in self.locks:
            f.close()
        self.locks = []

    def start_replay(self):
        log = open(self.p["log"], "ab")
        self.process = subprocess.Popen(["bash", "-c", self.script], cwd=HOME, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1)

    def forward(self, line):
        """a request to the replay: its reply (the state noted from it); None if the replay stopped"""
        if self.process is None:
            self.start_replay()
        try:
            self.process.stdin.write(line if line.endswith("\n") else line + "\n")
            self.process.stdin.flush()
            reply = self.process.stdout.readline()
        except (BrokenPipeError, OSError):
            reply = ""
        if not reply:
            return None
        try:
            request, answer = json.loads(line), json.loads(reply)
        except ValueError:
            return reply
        if answer.get("ok"):
            cmd = request.get("cmd")
            if cmd == "rt_checkpoint":
                self.state["checkpoint"] = None if request.get("drop") else answer.get("n")
            elif cmd in ("rt_reload", "rt_set"):
                for word in (request.get("env") or "").split():
                    if word.startswith("-") and "=" not in word:
                        self.state["applied"].pop(word[1:], None)
                    elif "=" in word:
                        k, _, v = word.partition("=")
                        self.state["applied"][k] = v
        return reply

    def stop(self, why):
        self.ending = why

    def session(self, conn):
        self.lock()
        open(self.p["busy"], "w").close()
        try:
            stream = conn.makefile("rw", buffering=1, encoding="utf-8", newline="\n")
            for line in stream:
                if not line.strip():
                    continue
                request = json.loads(line)
                cmd = request.get("cmd")
                if cmd == "keep_hello":
                    match = request.get("ids") == self.ids
                    stream.write(json.dumps({"ok": True, "match": match, "state": self.state,
                                             "started": self.info["started"], "runs": self.info["runs"]}) + "\n")
                    stream.flush()
                    if not match:
                        self.stop("its replay or recording changed (rebuilt or recorded again)")
                        return
                    self.info["runs"] += 1
                    continue
                if cmd == "quit":
                    return
                reply = self.forward(line)
                if reply is None:
                    stream.write(json.dumps({"ok": False, "error": "the kept replay stopped"}) + "\n")
                    stream.flush()
                    self.stop("its replay stopped (see keep/" + self.key + ".log)")
                    return
                stream.write(reply if reply.endswith("\n") else reply + "\n")
                stream.flush()
        except (OSError, ValueError):
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass
            try:
                os.unlink(self.p["busy"])
            except OSError:
                pass
            self.unlock()
            self.info["used"] = time.time()
            self.save()

    def serve(self, listener):
        signal.signal(signal.SIGTERM,
                      lambda *_: self.stop("asked to end (to make room for another kept replay, or by hand)"))
        listener.settimeout(5)
        self.save()
        try:
            while self.ending is None:
                try:
                    conn, _ = listener.accept()
                except socket.timeout:
                    if time.time() - self.info["used"] > IDLE:
                        self.stop(f"{IDLE // 60} idle minutes ran out")
                    elif owner_playing():
                        self.stop("the machine's owner started playing")
                    else:
                        try:
                            if [identity(self.recording), identity(self.replay)] != self.ids:
                                self.stop("its replay or recording changed (rebuilt or recorded again)")
                        except OSError:
                            self.stop("its replay or recording is gone")
                    continue
                except InterruptedError:
                    continue
                conn.setblocking(True)
                self.session(conn)
        finally:
            for name in ("sock", "json", "busy"):
                try:
                    os.unlink(self.p[name])
                except OSError:
                    pass
            write_json(self.p["ended"], {"why": self.ending or "it ended", "at": time.time()})
            if self.process:
                try:
                    self.process.stdin.write('{"cmd":"quit"}\n')
                    self.process.stdin.close()
                except (BrokenPipeError, OSError, ValueError):
                    pass
                try:
                    self.process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    self.process.kill()


def make_room(keep_key):
    """at most MOST kept replays on the rig: the least recently used idle ones asked to end (SIGTERM)"""
    alive = []
    for name in os.listdir(KEEP):
        if not name.endswith(".json") or name[:-5] == keep_key:
            continue
        try:
            with open(os.path.join(KEEP, name)) as f:
                info = json.load(f)
            os.kill(info["pid"], 0)
        except (OSError, ValueError, KeyError):
            continue
        alive.append((info.get("used", 0), info["pid"], os.path.exists(os.path.join(KEEP, name[:-5] + ".busy"))))
    idle = sorted(a for a in alive if not a[2])
    for used, pid, _ in idle[:max(0, len(alive) + 1 - MOST)]:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


def spawn(key, recording, replay, build, script):
    """the kept replay started detached (double fork, its own session); its socket listening before this returns"""
    p = paths(key)
    try:
        os.unlink(p["sock"])
    except OSError:
        pass
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(p["sock"])
    listener.listen(8)
    if os.fork():
        listener.close()
        os.wait()
        return
    os.setsid()
    if os.fork():
        os._exit(0)
    try:
        devnull = os.open(os.devnull, os.O_RDWR)
        log = os.open(p["log"], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        os.dup2(devnull, 0)
        os.dup2(log, 1)
        os.dup2(log, 2)
        # (none of the run's files kept open: the start lock above all, which would block the next run)
        os.closerange(3, listener.fileno())
        os.closerange(listener.fileno() + 1, 4096)
        Kept(key, recording, replay, build, script).serve(listener)
    finally:
        os._exit(0)


# ---------- a run joining it (the bridge)

def connect(recording, replay, build, script):
    os.makedirs(KEEP, exist_ok=True)
    os.chdir(KEEP)
    key = hashlib.sha256(f"{recording}\0{replay}".encode()).hexdigest()[:16]
    p = paths(key)
    ids = [identity(recording), identity(replay)]
    why = None
    for _ in range(2):
        with open(p["start"], "a") as start:
            fcntl.flock(start, fcntl.LOCK_EX)
            conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                conn.connect(p["sock"])
            except OSError:
                conn.close()
                conn = None
                if why is None:
                    try:
                        with open(p["ended"]) as f:
                            ended = json.load(f)
                        why = f"the kept replay ended {(time.time() - ended['at']) / 60:.0f} min ago: {ended['why']}"
                    except (OSError, ValueError, KeyError):
                        why = "none kept for this recording and replay yet (the first run)"
                make_room(key)
                spawn(key, recording, replay, build, script)
                conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                conn.connect(p["sock"])
        stream = conn.makefile("rw", buffering=1, encoding="utf-8", newline="\n")
        # (the hello waits, in the kept replay, for the GPU lock and for any run before this one)
        stream.write(json.dumps({"cmd": "keep_hello", "ids": ids}) + "\n")
        stream.flush()
        hello = json.loads(stream.readline() or "{}")
        if hello.get("match"):
            break
        conn.close()
        why = "the kept replay's replay or recording changed (rebuilt or recorded again)"
        for _ in range(100):
            if not os.path.exists(p["sock"]):
                break
            time.sleep(0.1)
    else:
        print(json.dumps({"keep": "error", "why": "could not join a kept replay"}), flush=True)
        return 1
    print(json.dumps({"keep": "cold" if why else "warm", "why": why, "state": hello["state"],
                      "started": hello.get("started"), "runs": hello.get("runs")}), flush=True)
    for line in sys.stdin:
        if not line.strip():
            continue
        stream.write(line)
        stream.flush()
        if json.loads(line).get("cmd") == "quit":
            break
        reply = stream.readline()
        if not reply:
            break
        sys.stdout.write(reply)
        sys.stdout.flush()
    conn.close()
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 6 and sys.argv[1] == "connect":
        recording, replay, build, script = sys.argv[2:6]
        if not script.startswith(("set ", "#")):
            script = base64.b64decode(script).decode()  # (rt2_bench.py sends it as base64)
        sys.exit(connect(recording, replay, build, script))
    sys.exit("usage: rt2_bench_keep.py connect <recording> <replay> <build> <replay script>")
