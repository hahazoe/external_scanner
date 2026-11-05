# -*- coding: utf-8 -*-
"""Render-side spike breakdown: is a slow frame Lua-bound, traversal-bound,
or not instrumented at all?

Motivation: on sealapp most >20 ms frames turned out to have small `lua_cpu`.
The Lua profiler alone cannot say where the time went, but the three C++ hooks
(cc.Director:drawScene / cc.Scene:render / cc.Renderer:render) let you split the
frame and decide in one pass whether to keep digging in Lua at all.

The decisive signal is the **traversal / submission ratio**:
    cc.Scene:render   == visit() recursion + one renderer->render()
    cc.Renderer:render == actual GL command submission
If a spike frame shows a huge cc.Scene:render but a *normal* cc.Renderer:render
(same call count as a baseline frame), then no extra drawing happened -- the
scene graph walk itself got slow. That points at per-node CPU work inside
visit() (Label::updateContent, character visit() overrides, sortAllChildren),
NOT at Lua and NOT at "more nodes on screen".

Usage:
    python spike_breakdown.py <capture.cocosdbg> [out.txt] [thr_ms]

Output:
  A. correlations of frame period against each frame component
  B. spike table: seq / t / period / body / lua_cpu / SceneR / RenderR / other
  C. classification: lua-driven / render-driven / neither
  D. full cpp + lua dump for notable frames
"""
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cocosdbg_reader import iter_frames


def pearson(a, b):
    n = len(a)
    if n < 3:
        return float("nan")
    ma = sum(a) / n
    mb = sum(b) / n
    va = sum((x - ma) ** 2 for x in a)
    vb = sum((y - mb) ** 2 for y in b)
    if va <= 0 or vb <= 0:
        return float("nan")
    return sum((a[i] - ma) * (b[i] - mb) for i in range(n)) / (va * vb) ** 0.5


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: spike_breakdown.py <capture.cocosdbg> "
                         "[out.txt] [thr_ms]\n")
        return 64
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.splitext(path)[0] + "_spikes.txt"
    thr = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0

    frames = list(iter_frames(path))
    n = len(frames)
    if not n:
        return 1
    t0 = frames[0]["begin_s"]

    def cpp_ms(f, key):
        tot = 0.0
        for m in f["cpp_methods"]:
            if key in m["name"]:
                tot += m["total_ms"]
        return tot

    out = io.open(out_path, "w", encoding="utf-8")

    def w(s=""):
        out.write(s + "\n")

    scene_r = [cpp_ms(f, "cc.Scene:render") for f in frames]
    rend_r = [cpp_ms(f, "cc.Renderer:render") for f in frames]
    body = [f["drawscene_ms"] for f in frames]
    lua = [f["cpu_ms"] for f in frames]
    period = [f["period_ms"] for f in frames]
    other = [max(0.0, body[i] - lua[i] - scene_r[i] - rend_r[i]) for i in range(n)]

    w("capture: %s" % path)
    w("frames: %d ; span %.2f s ; threshold body >= %.1f ms" %
      (n, frames[-1]["end_s"] - t0, thr))
    w()
    w("== A. which column does the frame time follow? ==")
    w("  r(period, drawscene_body)     = %+.3f" % pearson(period, body))
    w("  r(period, lua_cpu)            = %+.3f" % pearson(period, lua))
    w("  r(period, cc.Scene:render)    = %+.3f" % pearson(period, scene_r))
    w("  r(period, cc.Renderer:render) = %+.3f" % pearson(period, rend_r))
    w("  r(period, uninstrumented)     = %+.3f" % pearson(period, other))
    w("  r(drawscene_body, lua_cpu)    = %+.3f" % pearson(body, lua))
    w()
    w("  mean body %.3f = lua %.3f + Scene:render %.3f + Renderer:render %.3f + other %.3f"
      % (sum(body) / n, sum(lua) / n, sum(scene_r) / n, sum(rend_r) / n, sum(other) / n))
    w()

    spikes = [f for f in frames if f["drawscene_ms"] >= thr]
    w("== B. spikes: %d frames (%.2f%% of %d) ==" %
      (len(spikes), 100.0 * len(spikes) / n, n))
    w("  %-7s %-7s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-9s %s" %
      ("seq", "t(s)", "period", "body", "lua_cpu", "SceneR", "RenderR", "other",
       "S/R", "hook_ev", "flags"))
    for f in spikes:
        sr = cpp_ms(f, "cc.Scene:render")
        rr = cpp_ms(f, "cc.Renderer:render")
        ratio = "%.1fx" % (sr / rr) if rr > 0.01 else "-"
        w("  %-7d %-7.2f %-8.1f %-8.2f %-8.2f %-8.2f %-8.2f %-8.2f %-8s %-9d 0x%08X" %
          (f["publish_sequence"], f["begin_s"] - t0, f["period_ms"], f["drawscene_ms"],
           f["cpu_ms"], sr, rr,
           f["drawscene_ms"] - f["cpu_ms"] - sr - rr, ratio, f["hook_events"], f["flags"]))
    w("  (S/R = Scene:render / Renderer:render. Baseline ratio on sealapp is ~2.2x;")
    w("   a ratio of 25-60x means the graph WALK got slow, not the drawing.)")
    w()

    lua_d = [f for f in spikes if f["cpu_ms"] >= 10.0]
    render_d = [f for f in spikes if f["cpu_ms"] < 10.0 and
                cpp_ms(f, "cc.Scene:render") >= 0.35 * f["drawscene_ms"]]
    _tagged = set(f["publish_sequence"] for f in lua_d) | \
        set(f["publish_sequence"] for f in render_d)
    neither = [f for f in spikes if f["publish_sequence"] not in _tagged]
    w("== C. classification ==")
    w("  lua-driven    (lua_cpu >= 10 ms)                     : %d" % len(lua_d))
    for f in lua_d:
        w("      seq=%-7d t=%-6.2f body=%-7.2f lua_cpu=%-6.2f SceneR=%.2f" %
          (f["publish_sequence"], f["begin_s"] - t0, f["drawscene_ms"], f["cpu_ms"],
           cpp_ms(f, "cc.Scene:render")))
    w("  render-driven (SceneR >= 35%% body, lua_cpu < 10 ms)  : %d" % len(render_d))
    for f in render_d:
        w("      seq=%-7d t=%-6.2f body=%-7.2f SceneR=%.2f" %
          (f["publish_sequence"], f["begin_s"] - t0, f["drawscene_ms"],
           cpp_ms(f, "cc.Scene:render")))
    w("  neither                                              : %d" % len(neither))
    for f in neither:
        sr = cpp_ms(f, "cc.Scene:render")
        rr = cpp_ms(f, "cc.Renderer:render")
        w("      seq=%-7d t=%-6.2f body=%-7.2f lua=%-5.2f SceneR=%-6.2f RenderR=%-5.2f other=%.2f" %
          (f["publish_sequence"], f["begin_s"] - t0, f["drawscene_ms"], f["cpu_ms"],
           sr, rr, f["drawscene_ms"] - f["cpu_ms"] - sr - rr))
    w()

    notable = sorted(set(f["publish_sequence"] for f in spikes))
    by_seq = dict((f["publish_sequence"], f) for f in frames)
    w("== D. full method dump for spike frames ==")
    for seq in notable:
        f = by_seq.get(seq)
        if not f:
            continue
        sr = cpp_ms(f, "cc.Scene:render")
        rr = cpp_ms(f, "cc.Renderer:render")
        w("")
        w("--- seq=%d t=%.2fs period=%.1f body=%.2f lua_cpu=%.2f SceneR=%.2f RenderR=%.2f flags=0x%08X hook=%d mem=%.0fMB"
          % (seq, f["begin_s"] - t0, f["period_ms"], f["drawscene_ms"], f["cpu_ms"],
             sr, rr, f["flags"], f["hook_events"], f["mem_kb"] / 1024.0))
        w("    cpp :")
        for m in sorted(f["cpp_methods"], key=lambda m: -m["total_ms"]):
            w("        %-28s total=%-8.2f max=%-8.2f calls=%-6d" %
              (m["name"], m["total_ms"], m["max_ms"], m["calls"]))
        w("    lua (by incl):")
        for m in sorted([m for m in f["lua_methods"]
                         if m["incl_ms"] > 0.3 or m["self_ms"] > 0.3],
                        key=lambda m: -m["incl_ms"])[:22]:
            w("        incl=%-8.2f self=%-8.2f calls=%-7d %s" %
              (m["incl_ms"], m["self_ms"], m["started"], m["name"]))
        if f["lua_lines"]:
            w("    lines:")
            has_ms = any((r.get("line_ms") or 0.0) > 0.0 for r in f["lua_lines"])
            ranked = sorted(
                f["lua_lines"],
                key=(lambda r: -(r.get("line_ms") or 0.0)) if has_ms
                else (lambda r: -r["samples"]))
            for r in ranked[:6]:
                w("        line_ms=%-7.3f smp=%-5d %s:%d  (%s)" %
                  (r.get("line_ms") or 0.0, r["samples"],
                   r["source"], r["line"], r["function_name"]))
    out.close()
    sys.stdout.write("report written: %s\n" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
