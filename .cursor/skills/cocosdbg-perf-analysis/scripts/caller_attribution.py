# -*- coding: utf-8 -*-
"""Caller attribution WITHOUT a trace event stream.

Uses two signals that are always present in .cocosdbg v5 recordings:

  1. per-method `incl_ms` is derived from the profiler's shadow stack, so
     parent/child nesting is real.  A spike's true caller has a large
     inclusive time in the spike frames and contains the suspicious callee.
  2. "rarity": count in how many of ALL frames a method runs at all
     (`base`).  A method that runs in *only* the spike frames (base == number
     of spike frames) is co-triggered with the spike; a method present in
     ~every frame (base ~= frame count) is just a generic ancestor
     (dispatcher/update loop) and must not be mistaken for the caller.

Usage:
    python caller_attribution.py <capture.cocosdbg> [report.txt] [selector]

    selector: how to pick spike frames.
      body=25        -> frames with drawscene_body_ms >= 25   (default)
      self=<substr>,<ms>
                     -> frames where method matching <substr> has self_ms >= ms
                        (e.g. 'self=:61:Lua:dump_,1.0')

Output: the inclusive call chain (parents first), method-by-method, plus the
"rare co-triggered" shortlist, for each spike frame and aggregated.
"""
import io
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cocosdbg_reader import iter_frames

FLAG_INTERVAL_LIMITED = 0x80


def pearson(a, b):
    n = len(a)
    if n < 3:
        return float("nan")
    ma = sum(a) / n
    mb = sum(b) / n
    va = sum((x - ma) ** 2 for x in a)
    vb = sum((x - mb) ** 2 for x in b)
    if va <= 0 or vb <= 0:
        return float("nan")
    return sum((a[i] - ma) * (b[i] - mb) for i in range(n)) / (va * vb) ** 0.5


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: caller_attribution.py <capture.cocosdbg> "
                         "[report.txt] [body=25 | self=substr,ms]\n")
        return 64
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.splitext(path)[0] + "_callers.txt"
    sel = sys.argv[3] if len(sys.argv) > 3 else "body=25"

    frames = list(iter_frames(path))
    n = len(frames)
    if not n:
        return 1
    t0 = frames[0]["begin_s"]

    # ---------------- pick spike frames
    if sel.startswith("body="):
        thr = float(sel.split("=", 1)[1])
        spikes = [f for f in frames if f["drawscene_ms"] >= thr]
        desc = "drawscene_body >= %g ms" % thr
    elif sel.startswith("self="):
        sub, ms = sel.split("=", 1)[1].rsplit(",", 1)
        thr = float(ms)
        spikes = []
        for f in frames:
            for m in f["lua_methods"]:
                if sub in m["name"] and m["self_ms"] >= thr:
                    spikes.append(f)
                    break
        desc = "self_ms('%s') >= %g ms" % (sub, thr)
    else:
        sys.stderr.write("bad selector: %s\n" % sel)
        return 64

    out = io.open(out_path, "w", encoding="utf-8")
    def w(s=""):
        out.write(s + "\n")

    # ---------------- global method frequency
    freq = defaultdict(int)
    for f in frames:
        for m in f["lua_methods"]:
            if m["started"] > 0 or m["self_ms"] > 0.02:
                freq[m["name"]] += 1

    w("capture: %s" % path)
    w("frames: %d ; spike frames: %d (%s)" % (n, len(spikes), desc))
    if not spikes:
        w("(no spike frames matched)")
        out.close()
        print("report written: %s" % out_path)
        return 0

    # ---------------- aggregate incl/self over spike frames
    agg = defaultdict(lambda: [0.0, 0.0, 0])   # name -> [sum_incl, sum_self, hits]
    for f in spikes:
        for m in f["lua_methods"]:
            r = agg[m["name"]]
            r[0] += m["incl_ms"]
            r[1] += m["self_ms"]
            r[2] += 1

    w("")
    w("== A. aggregate over spike frames, ranked by inclusive time ==")
    w("  (base = in how many of the %d frames the method runs at all)" % n)
    w("  %-9s %-9s %-6s %-7s %-7s  %s" %
      ("sum_incl", "sum_self", "hit", "hit/n", "base", "name"))
    for name, (inc, slf, hits) in sorted(agg.items(), key=lambda kv: -kv[1][0])[:45]:
        w("  %-9.2f %-9.2f %-6d %-7.2f %-7d  %s" %
          (inc, slf, hits, hits / float(len(spikes)), freq[name], name))

    w("")
    w("== B. rare co-triggered methods (candidate spike-only work) ==")
    w("  filter: hits >= half the spike frames AND base <= 3x spike frames")
    cand = [(nm, v) for nm, v in agg.items()
            if v[2] >= max(2, len(spikes) // 2) and freq[nm] <= 3 * len(spikes) + 2]
    for name, (inc, slf, hits) in sorted(cand, key=lambda kv: -kv[1][0])[:45]:
        w("  hit=%-4d/%d  base=%-5d  sum_incl=%-8.2f  sum_self=%-8.2f  %s" %
          (hits, len(spikes), freq[name], inc, slf, name))
    if not cand:
        w("  (none - the spike work is spread over always-present methods)")

    w("")
    w("== C. reconstructed inclusive call chain, worst spike frame ==")
    worst = max(spikes, key=lambda f: f["drawscene_ms"])
    w("  worst frame: seq=%d t=%.2fs body=%.2f ms lua_cpu=%.2f ms "
      "script=%.2f c=%.2f flags=0x%08X" %
      (worst["publish_sequence"], worst["begin_s"] - t0, worst["drawscene_ms"],
       worst["cpu_ms"], worst["script_ms"], worst["c_ms"], worst["flags"]))
    w("  inclusive nesting (indent = containment, deepest = actual hot leaf):")
    rows = sorted([m for m in worst["lua_methods"] if m["incl_ms"] > 0.05],
                  key=lambda m: -m["incl_ms"])
    w("  %-9s %-9s %-8s %-6s  %s" % ("incl", "self", "self%", "calls", "name"))
    for m in rows[:30]:
        pct = 100.0 * m["self_ms"] / m["incl_ms"] if m["incl_ms"] > 0 else 0.0
        w("  %-9.3f %-9.3f %-8.1f %-6d  %s" %
          (m["incl_ms"], m["self_ms"], pct, m["started"], m["name"]))

    w("")
    w("== D. per-frame summary ==")
    w("  %-8s %-8s %-8s %-10s %-10s %s" %
      ("seq", "t(s)", "body", "lua_cpu", "flags", "top inclusive (excluding generic ancestors)"))
    for f in spikes:
        tops = sorted([m for m in f["lua_methods"]
                       if m["incl_ms"] > 0.5 and freq[m["name"]] <= 3 * len(spikes) + 2],
                      key=lambda m: -m["incl_ms"])[:3]
        w("  %-8d %-8.2f %-8.2f %-10.2f 0x%08X %s" %
          (f["publish_sequence"], f["begin_s"] - t0, f["drawscene_ms"], f["cpu_ms"], f["flags"],
           "; ".join("%s %.2fms" % (m["name"][-46:], m["incl_ms"]) for m in tops)))

    w("")
    w("== E. lua_cpu identity and wall trust ==")
    id_errs = [abs(f["cpu_ms"] - f["script_ms"] - f["c_ms"]) for f in spikes]
    w("  spike frames: max |cpu - script - c| = %.4f ms" %
      (max(id_errs) if id_errs else 0.0))
    w("  occupancy metric is lua_cpu (= script + C). lua_wall is a busy-union.")
    lim = sum(1 for f in spikes if f["flags"] & FLAG_INTERVAL_LIMITED)
    w("  spike frames with INTERVAL_LIMITED(0x80): %d / %d" % (lim, len(spikes)))
    if lim > max(2, len(spikes) // 5):
        exc = [f["cpu_ms"] - f["wall_ms"] for f in spikes]
        w("  r(lua_cpu - lua_wall, hook_events) = %+.3f" %
          pearson(exc, [float(f["hook_events"]) for f in spikes]))
        w("  ! dense 0x80: this looks like an OLD capture where busy intervals")
        w("    were sliced per hook and truncated at 4096. Use lua_cpu / self_ms,")
        w("    NOT lua_wall.")
    else:
        w("  0x80 is sparse; do not treat wall truncation as the default story.")
        w("  Single-thread new captures should have wall ~ cpu.")
    out.close()
    print("report written: %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
