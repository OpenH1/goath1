#!/usr/bin/env python3
"""A network test's latency by hop, from the machines' debug.network_latency_trace files (one computer's clock:
port/linux/game/network_latency_trace.c). Each control change is followed from the machine of its player (sampled,
sent) through the host (arrived, read, applied to its copy, sent on) to each other client (arrived, read, applied,
first frame presented); each hop's median, 95th percentile and worst in milliseconds, and what the host and the
clients hold: the ticks a client's input comes ahead of the host's tick that runs it, and the host's latest a
client's tick has. A machine that draws gives its frame presented after the change: the owner's own view of it, the
host's, each other client's.

Machines on computers of their own (--align) are put on the host's clock: each client's offset from the fastest
datagrams each way in each window of 15 s (the host's to it, its to the host; their transit taken alike), a line
fitted through the windows' offsets for the clocks' drift.

    tools/net_latency.py p1/latency.txt p2/latency.txt p3/latency.txt [--align] [--json out.json]
"""
import argparse
import json
import sys
from pathlib import Path


def parse(path):
    trace = {"name": Path(path).parent.name or Path(path).stem, "role": None, "rate": None, "S": {}, "T": {}, "V": {},
             "A": {}, "P": [], "B": {}, "K": [], "C": None}
    for line in Path(path).read_text(errors="replace").splitlines():
        part = line.split()
        if not part:
            continue
        if part[0] == "#":
            if "rate" in part:
                trace["rate"] = int(part[part.index("rate") + 1])
                trace["role"] = part[part.index("rate") + 2]
                for name in ("vsync", "max_fps"):
                    if name in part:
                        trace[name] = part[part.index(name) + 1]
            continue
        kind = part[0]
        try:
            if kind in "STVA" and len(part) >= 5:
                key = (part[1], int(part[2]), int(part[3]), int(part[4]))
                values = [int(x) for x in part[5:]]
                if kind == "T":
                    trace["T"].setdefault(key, []).append(values)
                elif key not in trace[kind]:
                    trace[kind][key] = values
            elif kind == "P":
                trace["P"].append(int(part[1]))
            elif kind == "B":
                trace["B"][int(part[1])] = int(part[2])
            elif kind == "K":
                trace["K"].append((int(part[1]), int(part[2]), int(part[3])))
            elif kind == "C":
                trace["C"] = [int(x) for x in part[1:5]]
        except ValueError:
            continue
    trace["P"].sort()
    return trace


def shift(trace, offset):
    """The trace's times less offset(t) (its clock's lead over the host's at its time t)."""
    at = lambda t: t - offset(t) if t else t
    for kind, fields in (("S", (0, 1)), ("V", (0, 1, 2)), ("A", (0,))):
        for key, values in trace[kind].items():
            trace[kind][key] = [at(v) if i in fields else v for i, v in enumerate(values)]
    trace["T"] = {key: [[v[0], at(v[1])] for v in values] for key, values in trace["T"].items()}
    trace["P"] = sorted(at(t) for t in trace["P"])
    trace["B"] = {tick: at(t) for tick, t in trace["B"].items()}
    trace["K"] = [(tick, heard, at(t)) for tick, heard, t in trace["K"]]


def align(traces, window=15000000):
    """Each client's clock put on the host's: per window, the fastest datagram each way (its to the host: its sent to
    the host's arrival; the host's to it: the host's first sending of a change to its arrival there); the offset is
    half their difference, the transit taken alike each way, and a line fitted through the windows' offsets."""
    host = next((t for t in traces if t["role"] == "host"), None)
    report = {}
    for client in traces:
        if client is host:
            continue
        up, down = {}, {}
        for key, values in client["T"].items():
            sent = min((v[1] for v in values if v[0] == -1), default=None)
            received = host["V"].get(key)
            if sent and received:
                b = sent // window
                up[b] = min(up.get(b, 1 << 62), (received[0] or received[1]) - sent)
        for key, received in client["V"].items():
            forwards = host["T"].get(key)
            if not forwards:
                continue
            arrival = received[0] or received[1]
            b = arrival // window
            down[b] = min(down.get(b, 1 << 62), arrival - min(v[1] for v in forwards))
        points = [((b + 0.5) * window, (down[b] - up[b]) / 2.0, (down[b] + up[b]) / 2.0)
                  for b in sorted(set(up) & set(down))]
        if not points:
            report[client["name"]] = None
            continue
        n = len(points)
        mx = sum(p[0] for p in points) / n
        my = sum(p[1] for p in points) / n
        sxx = sum((p[0] - mx) ** 2 for p in points)
        slope = sum((p[0] - mx) * (p[1] - my) for p in points) / sxx if sxx else 0.0
        shift(client, lambda t, mx=mx, my=my, slope=slope: int(my + slope * (t - mx)))
        report[client["name"]] = {"offset_ms": my / 1000.0, "drift_ppm": slope * 1e6, "windows": n,
                                  "one_way_fastest_ms": min(p[2] for p in points) / 1000.0}
    return report


def stats(values):
    values = sorted(values)
    if not values:
        return None
    pick = lambda q: values[min(len(values) - 1, int(q * (len(values) - 1) + 0.5))]
    return {"n": len(values), "median": pick(0.5), "p95": pick(0.95), "worst": values[-1]}


def first_after(times, moment):
    import bisect
    index = bisect.bisect_left(times, moment)
    return times[index] if index < len(times) else None


def hops(traces):
    host = next((t for t in traces if t["role"] == "host"), None)
    if not host:
        sys.exit("no host trace")
    result = {}

    def add(name, value):
        if value is not None and value >= 0:
            result.setdefault(name, []).append(value / 1000.0)

    for owner in traces:
        for key, (sampled, step) in owner["S"].items():
            kind = key[0]
            if (kind == "H") != (owner is host):
                continue
            sent = owner["T"].get(key)
            own_sent = min((v[1] for v in sent if v[0] == -1), default=None) if sent and owner is not host else None
            path = "client" if owner is not host else "host"
            add(f"{kind} {path}: input read to step", step - sampled if sampled else None)
            own_shown = first_after(owner["P"], step) if owner["P"] and sampled else None
            if own_shown:
                add(f"{kind} {path}: step to own presented", own_shown - step)
                add(f"{kind} = {path}: own input to own presented", own_shown - sampled)
            if owner is not host:
                if own_sent is None:
                    continue
                add(f"{kind} a. owner: step to sent", own_sent - step)
                received = host["V"].get(key)
                if not received:
                    continue
                kernel, read, handled = received[0], received[1], received[2]
                if kernel:
                    add(f"{kind} b1. sent to host's kernel", kernel - own_sent)
                    add(f"{kind} b2. host: kernel to read", read - kernel)
                add(f"{kind} b3. host: read to handled", handled - read)
                applied = host["A"].get(key)
                if applied:
                    add(f"{kind} b4. host: handled to applied", applied[0] - handled)
                    add(f"{kind} = owner's input to host's applied", applied[0] - sampled)
                    shown = first_after(host["P"], applied[0]) if host["P"] else None
                    if shown:
                        add(f"{kind} b6. host: applied to presented", shown - applied[0])
                        add(f"{kind} = owner's input to host's presented", shown - sampled)
                forwards = sorted(v[1] for v in host["T"].get(key, []))
                if forwards:
                    add(f"{kind} b5. host: handled to sent on", forwards[0] - handled)
            else:
                forwards = sorted(v[1] for v in host["T"].get(key, []))
                if forwards:
                    add(f"{kind} a. host: step to sent", forwards[0] - step)
                handled = None
            for other in traces:
                if other is host or other is owner:
                    continue
                received = other["V"].get(key)
                applied = other["A"].get(key)
                if not received or not applied or not forwards:
                    continue
                kernel, read, handled_there = received[0], received[1], received[2]
                arrival = kernel or read
                went = max((f for f in forwards if f <= arrival), default=None)
                label = f"{kind} c. {other['name']}"
                if went is not None and kernel:
                    add(f"{label}: sent on to kernel", kernel - went)
                if kernel:
                    add(f"{label}: kernel to read", read - kernel)
                add(f"{label}: read to applied", applied[0] - handled_there)
                add(f"{label} = owner's input to applied", applied[0] - sampled)
                if other["P"]:
                    shown = first_after(other["P"], applied[0])
                    if shown:
                        add(f"{label}: applied to presented", shown - applied[0])
                        add(f"{label} = owner's input to presented", shown - sampled)
    return {name: stats(values) for name, values in sorted(result.items())}


def leads(traces):
    host = next(t for t in traces if t["role"] == "host")
    out = {}
    early = []
    ticks = []
    for key, received in host["V"].items():
        if key[0] != "I":
            continue
        began = host["B"].get(key[2])
        arrival = received[0] or received[1]
        if began:
            early.append((began - arrival) / 1000.0)
        ticks.append(key[2] - received[3])
    out["host: a client's input came before the host's tick of its number began (ms)"] = stats(early)
    out["host: a client's input's tick less the host's as it came (ticks)"] = stats(ticks)
    for trace in traces:
        if trace["K"]:
            out[f"{trace['name']}: its tick less the host's latest it had (ticks)"] = stats(
                [tick - heard for tick, heard, _ in trace["K"] if heard >= 0])
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("traces", nargs="+")
    parser.add_argument("--align", action="store_true", help="the clients' clocks put on the host's (machines on "
                        "computers of their own)")
    parser.add_argument("--json")
    args = parser.parse_args()
    traces = [parse(path) for path in args.traces]
    aligned = align(traces) if args.align else {}
    for name, figure in aligned.items():
        print(f"{name}: " + (f"clock {figure['offset_ms']:+.3f} ms on the host's, drift {figure['drift_ppm']:+.1f} ppm, "
                             f"fastest one way {figure['one_way_fastest_ms']:.3f} ms ({figure['windows']} windows)"
                             if figure else "no datagrams each way to align by"))
    rate = next((t["rate"] for t in traces if t["rate"]), None)
    table = hops(traces)
    held = leads(traces)
    close = {t["name"]: t["C"] for t in traces if t["C"]}
    print(f"rate {rate}: " + ", ".join(f"{t['name']} {t['role']} (vsync {t.get('vsync', '-')}, max_fps "
                                        f"{t.get('max_fps', '-')}, {'draws' if t['P'] else 'no frames'})"
                                        for t in traces))
    print(f"{'hop':64} {'n':>6} {'median':>8} {'p95':>8} {'worst':>8}")
    for name, figure in list(table.items()) + list(held.items()):
        if figure:
            print(f"{name:64} {figure['n']:6} {figure['median']:8.2f} {figure['p95']:8.2f} {figure['worst']:8.2f}")
    for name, figure in close.items():
        print(f"{name}: {figure[0]} datagrams, {figure[1]} within 1 ms of the one before to the same peer; "
              f"{figure[2]} stream writes, {figure[3]} within 1 ms of a datagram to the same peer")
    if args.json:
        Path(args.json).write_text(json.dumps({"rate": rate, "hops": table, "held": held, "close": close,
                                                    "aligned": aligned}, indent=1))


if __name__ == "__main__":
    main()
