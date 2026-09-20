# -*- coding: utf-8 -*-
"""Aggregate a .cocosdbg recording into a performance report.

Usage:
    python analyze_capture.py <capture.cocosdbg> [report.txt]

Produces: frame cost distribution, fps buckets, worst frames, top Lua methods
by self/calls/spike, top C++ methods, hot lines, per-source totals.
"""
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cocosdbg_reader import iter_frames


def pct(arr, p):
    a = sorted(arr)
    if not a:
        return 0.0
    return a[min(len(a) - 1, int(len(a) * p))]


def stats_line(label, arr):
    if not arr:
        return "%s: n/a" % label
    return ("%s: avg=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f" %
            (label, sum(arr) / len(arr), pct(arr, 0.50), pct(arr, 0.95),
             pct(arr, 0.99), max(arr)))


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: analyze_capture.py <capture.cocosdbg> [report.txt]\n")
        return 64
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(path)[0] + "_report.txt"

    methods = {}
    lines = {}
    cpp = {}
    frames = []

    for fr in iter_frames(path):
        frame_methods = []
        for m in fr["lua_methods"]:
            rec = methods.get(m["name"])
            if rec is None:
                rec = methods[m["name"]] = {
                    "name": m["name"], "self_ms": 0.0, "incl_ms": 0.0,
                    "max_self": 0.0, "max_incl": 0.0, "started": 0,
                    "completed": 0, "frames": 0}
            rec["self_ms"] += m["self_ms"]
            rec["incl_ms"] += m["incl_ms"]
            rec["max_self"] = max(rec["max_self"], m["max_self_ms"])
            rec["max_incl"] = max(rec["max_incl"], m["max_incl_ms"])
            rec["started"] += m["started"]
            rec["completed"] += m["completed"]
            if m["self_ms"] > 0.02 or m["started"]:
                rec["frames"] += 1
            if m["self_ms"] > 0.02:
                frame_methods.append((m["self_ms"], m["incl_ms"], m["name"]))
        for m in fr["cpp_methods"]:
            rec = cpp.get(m["name"])
            if rec is None:
                rec = cpp[m["name"]] = [0.0, 0.0, 0, 0]
            rec[0] += m["total_ms"]
            rec[1] = max(rec[1], m["max_ms"])
            rec[2] += m["calls"]
            if m["total_ms"] > 0.02:
                rec[3] += 1
        for ln in fr["lua_lines"]:
            key = (ln["source"], ln["function_name"], ln["line"])
            lines[key] = lines.get(key, 0) + ln["samples"]
        frames.append((fr["period_ms"], fr["wall_ms"], fr["cpu_ms"],
                       fr["script_ms"], fr["c_ms"], fr["mem_kb"],
                       fr["drawscene_ms"], fr["publish_sequence"],
                       frame_methods))

    out = io.open(out_path, "w", encoding="utf-8")

    def w(s):
        out.write(s + "\n")

    n = len(frames)
    w("capture: %s" % path)
    w("parsed frames: %d" % n)
    if n == 0:
        out.close()
        return 1

    periods = [x[0] for x in frames]
    w("")
    w("== frame cost distribution (ms per frame) ==")
    w(stats_line("frame_period  ", periods))
    w(stats_line("drawscene_body", [x[6] for x in frames]))
    w(stats_line("lua_wall      ", [x[1] for x in frames]))
    w(stats_line("lua_cpu       ", [x[2] for x in frames]))
    w(stats_line("lua_script_self", [x[3] for x in frames]))
    w(stats_line("lua_c_self    ", [x[4] for x in frames]))
    w(stats_line("lua_mem_kb    ", [x[5] for x in frames]))
    w("total recorded time: %.2f s" % (sum(periods) / 1000.0))
    w("sum lua_wall: %.2f s  sum lua_cpu: %.2f s" %
      (sum(x[1] for x in frames) / 1000.0, sum(x[2] for x in frames) / 1000.0))

    w("")
    w("== frame period buckets ==")
    buckets = [(0, 16.7, "<16.7 (60fps)"), (16.7, 20, "16.7-20"),
               (20, 33.3, "20-33.3"), (33.3, 50, "33.3-50"),
               (50, 100, "50-100"), (100, 1e9, ">100")]
    for lo, hi, label in buckets:
        c = sum(1 for p in periods if lo <= p < hi)
        w("  %-14s %6d frames (%.1f%%)" % (label, c, 100.0 * c / n))

    w("")
    w("== worst 15 frames by lua_wall ==")
    for fr in sorted(frames, key=lambda x: -x[1])[:15]:
        top = sorted(fr[8], key=lambda t: -t[0])[:3]
        tops = "; ".join("%s %.2fms" % (t[2], t[0]) for t in top)
        w("  seq=%d period=%.1f wall=%.2f script=%.2f c=%.2f | top: %s" %
          (fr[7], fr[0], fr[1], fr[3], fr[4], tops or "-"))

    w("")
    w("== top 40 lua methods by total self_ms ==")
    w("  %-8s %-8s %-8s %-9s %-9s %-6s %s" %
      ("self_ms", "incl_ms", "maxself", "started", "compl", "frames", "name"))
    for m in sorted(methods.values(), key=lambda m: -m["self_ms"])[:40]:
        w("  %-8.1f %-8.1f %-8.2f %-9d %-9d %-6d %s" %
          (m["self_ms"], m["incl_ms"], m["max_self"], m["started"],
           m["completed"], m["frames"], m["name"]))

    w("")
    w("== top 25 lua methods by call count (call pressure) ==")
    for m in sorted(methods.values(), key=lambda m: -m["started"])[:25]:
        w("  started=%-9d self=%-9.1f avg_self_us=%-8.1f %s" %
          (m["started"], m["self_ms"],
           (m["self_ms"] * 1000.0 / m["started"]) if m["started"] else 0,
           m["name"]))

    w("")
    w("== top 25 lua methods by max_self (single-frame spike) ==")
    for m in sorted(methods.values(), key=lambda m: -m["max_self"])[:25]:
        w("  max_self=%-8.2f total_self=%-9.1f started=%-8d %s" %
          (m["max_self"], m["self_ms"], m["started"], m["name"]))

    w("")
    w("== top 25 cpp methods by total ms ==")
    for name, (total, mx, calls, fr_) in sorted(cpp.items(),
                                                key=lambda kv: -kv[1][0])[:25]:
        w("  total=%-9.1f max=%-7.2f calls=%-9d frames=%-6d %s" %
          (total, mx, calls, fr_, name))

    w("")
    w("== top 60 hot lines by samples ==")
    for (src, fn, line), s in sorted(lines.items(), key=lambda kv: -kv[1])[:60]:
        w("  samples=%-8d %s:%d  in %s" % (s, src, line, fn or "?"))

    w("")
    w("== top 30 sources by line-sample total ==")
    src_tot = {}
    for (src, fn, line), s in lines.items():
        src_tot[src] = src_tot.get(src, 0) + s
    for src, s in sorted(src_tot.items(), key=lambda kv: -kv[1])[:30]:
        w("  samples=%-8d %s" % (s, src))

    out.close()
    print("report written: %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
