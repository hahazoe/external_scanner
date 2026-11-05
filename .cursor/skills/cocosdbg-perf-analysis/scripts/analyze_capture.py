# -*- coding: utf-8 -*-
"""Aggregate a .cocosdbg recording into a performance report.

Usage:
    python analyze_capture.py <capture.cocosdbg> [report.txt]

Produces: identity checks (cpu == script + c), frame cost distribution,
fps buckets, worst frames by lua_cpu, Lua/C method boards, top C++ methods,
hot lines (prefer line_ms on v6).
"""
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cocosdbg_reader import iter_frames

KIND_C = 2
FLAG_INTERVAL_LIMITED = 0x80
FLAG_METHOD_LIMITED = 0x02


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


def kind_label(kind):
    if kind == KIND_C:
        return "C"
    if kind == 3:
        return "main"
    return "Lua"


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
    proc_ws_kb = []
    proc_private_kb = []
    max_id_err = 0.0
    sum_id_err = 0.0
    n_id_bad = 0
    n_interval_limited = 0
    n_method_limited = 0
    max_kind_script_err = 0.0
    max_kind_c_err = 0.0

    for fr in iter_frames(path):
        frame_methods = []
        script_sum = 0.0
        c_sum = 0.0
        flags = fr["flags"]
        if flags & FLAG_INTERVAL_LIMITED:
            n_interval_limited += 1
        if flags & FLAG_METHOD_LIMITED:
            n_method_limited += 1
        id_err = abs(fr["cpu_ms"] - fr["script_ms"] - fr["c_ms"])
        if id_err > max_id_err:
            max_id_err = id_err
        sum_id_err += id_err
        if id_err > 0.05:
            n_id_bad += 1
        for m in fr["lua_methods"]:
            rec = methods.get(m["name"])
            if rec is None:
                rec = methods[m["name"]] = {
                    "name": m["name"], "kind": m["kind"],
                    "self_ms": 0.0, "incl_ms": 0.0,
                    "max_self": 0.0, "max_incl": 0.0, "started": 0,
                    "completed": 0, "frames": 0}
            rec["self_ms"] += m["self_ms"]
            rec["incl_ms"] += m["incl_ms"]
            rec["max_self"] = max(rec["max_self"], m["max_self_ms"])
            rec["max_incl"] = max(rec["max_incl"], m["max_incl_ms"])
            rec["started"] += m["started"]
            rec["completed"] += m["completed"]
            if m["kind"] == KIND_C:
                c_sum += m["self_ms"]
            else:
                script_sum += m["self_ms"]
            if m["self_ms"] > 0.02 or m["started"]:
                rec["frames"] += 1
            if m["self_ms"] > 0.02:
                frame_methods.append((m["self_ms"], m["incl_ms"], m["name"],
                                      m["kind"]))
        kind_script_err = abs(script_sum - fr["script_ms"])
        kind_c_err = abs(c_sum - fr["c_ms"])
        if kind_script_err > max_kind_script_err:
            max_kind_script_err = kind_script_err
        if kind_c_err > max_kind_c_err:
            max_kind_c_err = kind_c_err
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
            rec = lines.get(key)
            if rec is None:
                rec = lines[key] = [0, 0.0]
            rec[0] += ln["samples"]
            rec[1] += ln.get("line_ms", 0.0) or 0.0
        proc_ws_kb.append(fr.get("proc_ws_kb", 0.0) or 0.0)
        proc_private_kb.append(fr.get("proc_private_kb", 0.0) or 0.0)
        frames.append((fr["period_ms"], fr["wall_ms"], fr["cpu_ms"],
                       fr["script_ms"], fr["c_ms"], fr["mem_kb"],
                       fr["drawscene_ms"], fr["publish_sequence"],
                       frame_methods, fr["outside_ms"]))

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
    w("== identity (lua_cpu == script + c) ==")
    w("  max |cpu - script - c| = %.4f ms" % max_id_err)
    w("  mean |cpu - script - c| = %.4f ms" % (sum_id_err / n))
    w("  frames with |err| > 0.05 ms: %d / %d" % (n_id_bad, n))
    w("  max |sum(Lua/main self) - script| = %.4f ms" % max_kind_script_err)
    w("  max |sum(C self) - c|            = %.4f ms" % max_kind_c_err)
    w("  INTERVAL_LIMITED (0x80): %d / %d" % (n_interval_limited, n))
    w("  METHOD_LIMITED   (0x02): %d / %d" % (n_method_limited, n))
    if n_interval_limited > n * 0.2:
        w("  ! many INTERVAL_LIMITED frames: this is likely an OLD capture")
        w("    where lua_wall was truncated by hook density. Use lua_cpu,")
        w("    not lua_wall.")
    else:
        w("  lua_wall is a multithread busy-union; occupancy = lua_cpu")
        w("  (script + C). Single-thread new captures should have wall ~ cpu.")

    w("")
    w("== frame cost distribution (ms per frame) ==")
    w(stats_line("frame_period  ", periods))
    w(stats_line("drawscene_body", [x[6] for x in frames]))
    w(stats_line("lua_wall      ", [x[1] for x in frames]))
    w(stats_line("lua_cpu       ", [x[2] for x in frames]))
    w(stats_line("lua_script_self", [x[3] for x in frames]))
    w(stats_line("lua_c_self    ", [x[4] for x in frames]))
    w(stats_line("lua_outside   ", [x[9] for x in frames]))
    w(stats_line("lua_mem_kb    ", [x[5] for x in frames]))
    w(stats_line("proc_ws_kb    ", proc_ws_kb))
    w(stats_line("proc_private_kb", proc_private_kb))
    w("total recorded time: %.2f s" % (sum(periods) / 1000.0))
    w("sum lua_wall: %.2f s  sum lua_cpu: %.2f s  (script %.2f + C %.2f)" %
      (sum(x[1] for x in frames) / 1000.0, sum(x[2] for x in frames) / 1000.0,
       sum(x[3] for x in frames) / 1000.0, sum(x[4] for x in frames) / 1000.0))

    w("")
    w("== frame period buckets ==")
    buckets = [(0, 16.7, "<16.7 (60fps)"), (16.7, 20, "16.7-20"),
               (20, 33.3, "20-33.3"), (33.3, 50, "33.3-50"),
               (50, 100, "50-100"), (100, 1e9, ">100")]
    for lo, hi, label in buckets:
        c = sum(1 for p in periods if lo <= p < hi)
        w("  %-14s %6d frames (%.1f%%)" % (label, c, 100.0 * c / n))

    w("")
    w("== worst 15 frames by lua_cpu ==")
    for fr in sorted(frames, key=lambda x: -x[2])[:15]:
        top = sorted(fr[8], key=lambda t: -t[0])[:3]
        tops = "; ".join("%s %.2fms" % (t[2], t[0]) for t in top)
        w("  seq=%d period=%.1f cpu=%.2f script=%.2f c=%.2f wall=%.2f | top: %s" %
          (fr[7], fr[0], fr[2], fr[3], fr[4], fr[1], tops or "-"))

    lua_methods = [m for m in methods.values() if m["kind"] != KIND_C]
    c_methods = [m for m in methods.values() if m["kind"] == KIND_C]

    w("")
    w("== top 40 Lua/main methods by total self_ms ==")
    w("  %-6s %-8s %-8s %-8s %-9s %-9s %-6s %s" %
      ("kind", "self_ms", "incl_ms", "maxself", "started", "compl", "frames", "name"))
    for m in sorted(lua_methods, key=lambda m: -m["self_ms"])[:40]:
        w("  %-6s %-8.1f %-8.1f %-8.2f %-9d %-9d %-6d %s" %
          (kind_label(m["kind"]), m["self_ms"], m["incl_ms"], m["max_self"],
           m["started"], m["completed"], m["frames"], m["name"]))

    w("")
    w("== top 25 C methods by total self_ms ==")
    w("  %-8s %-8s %-8s %-9s %-6s %s" %
      ("self_ms", "incl_ms", "maxself", "started", "frames", "name"))
    for m in sorted(c_methods, key=lambda m: -m["self_ms"])[:25]:
        w("  %-8.1f %-8.1f %-8.2f %-9d %-6d %s" %
          (m["self_ms"], m["incl_ms"], m["max_self"], m["started"],
           m["frames"], m["name"]))

    w("")
    w("== top 25 lua methods by call count (call pressure) ==")
    for m in sorted(methods.values(), key=lambda m: -m["started"])[:25]:
        w("  started=%-9d self=%-9.1f avg_self_us=%-8.1f [%s] %s" %
          (m["started"], m["self_ms"],
           (m["self_ms"] * 1000.0 / m["started"]) if m["started"] else 0,
           kind_label(m["kind"]), m["name"]))

    w("")
    w("== top 25 lua methods by max_self (single-frame spike) ==")
    for m in sorted(methods.values(), key=lambda m: -m["max_self"])[:25]:
        w("  max_self=%-8.2f total_self=%-9.1f started=%-8d [%s] %s" %
          (m["max_self"], m["self_ms"], m["started"], kind_label(m["kind"]),
           m["name"]))

    w("")
    w("== top 25 cpp methods by total ms ==")
    for name, (total, mx, calls, fr_) in sorted(cpp.items(),
                                                key=lambda kv: -kv[1][0])[:25]:
        w("  total=%-9.1f max=%-7.2f calls=%-9d frames=%-6d %s" %
          (total, mx, calls, fr_, name))

    has_line_ms = any(rec[1] > 0.0 for rec in lines.values())
    w("")
    if has_line_ms:
        w("== top 60 hot lines by line_ms (v6 wall time; samples as aux) ==")
        ranked = sorted(lines.items(), key=lambda kv: -kv[1][1])[:60]
        for (src, fn, line), (samples, line_ms) in ranked:
            w("  line_ms=%-8.3f samples=%-8d %s:%d  in %s" %
              (line_ms, samples, src, line, fn or "?"))
        w("")
        w("== top 30 sources by line_ms ==")
        src_tot = {}
        for (src, fn, line), (samples, line_ms) in lines.items():
            prev = src_tot.get(src)
            if prev is None:
                src_tot[src] = [0, 0.0]
                prev = src_tot[src]
            prev[0] += samples
            prev[1] += line_ms
        for src, (samples, line_ms) in sorted(src_tot.items(),
                                              key=lambda kv: -kv[1][1])[:30]:
            w("  line_ms=%-8.3f samples=%-8d %s" % (line_ms, samples, src))
    else:
        w("== top 60 hot lines by samples (no line_ms in this capture) ==")
        ranked = sorted(lines.items(), key=lambda kv: -kv[1][0])[:60]
        for (src, fn, line), (samples, line_ms) in ranked:
            w("  samples=%-8d %s:%d  in %s" % (samples, src, line, fn or "?"))
        w("")
        w("== top 30 sources by line-sample total ==")
        src_tot = {}
        for (src, fn, line), (samples, line_ms) in lines.items():
            src_tot[src] = src_tot.get(src, 0) + samples
        for src, s in sorted(src_tot.items(), key=lambda kv: -kv[1])[:30]:
            w("  samples=%-8d %s" % (s, src))

    out.close()
    print("report written: %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
