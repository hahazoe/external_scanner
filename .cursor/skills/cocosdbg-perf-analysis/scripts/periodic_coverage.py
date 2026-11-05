# -*- coding: utf-8 -*-
"""Chase the periodic driver of a spike train — and find what fires *near* spikes.

Two complementary questions, both answered without a trace event stream:

1. Is there a beat?  Print the spike timestamps and the statistics of the gaps.
   A tight cluster of ~1.3 s gaps that matches NO Lua timer interval is strong
   evidence the driver lives outside Lua (C++ schedule, async load callback).
   Caveat: pairs of spikes a few frames apart are one burst, not two beats.

2. Which Lua methods fire within +-K frames of MOST spikes?
   This is a *coverage* query, not a rarity query.  caller_attribution.py's
   rarity filter silently returns "(none)" when the spikes are heterogeneous or
   when the trigger runs in many ordinary frames too; coverage still surfaces it.
   Always cross-check against the method's total active-frame count: a method
   active in 259/1893 frames that covers 26/27 spikes is a real signal, while a
   method active in ~all frames covers everything trivially.

Usage:
    python periodic_coverage.py <capture.cocosdbg> [out.txt] [thr_ms]
"""
import io
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cocosdbg_reader import iter_frames

K = 2


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: periodic_coverage.py <capture.cocosdbg> "
                         "[out.txt] [thr_ms]\n")
        return 64
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.splitext(path)[0] + "_periodic.txt"
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

    spikes = [f for f in frames if f["drawscene_ms"] >= thr]
    rd = [f for f in spikes if f["cpu_ms"] < 10.0 and
          cpp_ms(f, "cc.Scene:render") >= 0.35 * f["drawscene_ms"]]

    out = io.open(out_path, "w", encoding="utf-8")

    def w(s=""):
        out.write(s + "\n")

    w("capture: %s" % path)
    w("frames %d ; spikes(body>=%.0f) %d ; render-driven %d" %
      (n, thr, len(spikes), len(rd)))
    w()

    w("== A. spike timeline (render-driven) ==")
    ts = [f["begin_s"] - t0 for f in rd]
    w("  " + ", ".join("%.2f" % t for t in ts))
    gaps = [ts[i + 1] - ts[i] for i in range(len(ts) - 1)]
    if gaps:
        w("  gaps: " + ", ".join("%.2f" % g for g in gaps))
        big = sorted(g for g in gaps if g > 0.5)
        small = [g for g in gaps if g <= 0.5]
        if big:
            w("  gaps > 0.5 s : n=%d mean=%.3f min=%.3f max=%.3f median=%.3f" %
              (len(big), sum(big) / len(big), big[0], big[-1], big[len(big) // 2]))
        if small:
            w("  gaps <= 0.5 s: n=%d mean=%.3f  (burst pairs, not separate beats)" %
              (len(small), sum(small) / len(small)))
        avg_p = (sum(f["period_ms"] for f in frames) / n) / 1000.0
        if big:
            w("  avg frame period %.3f ms -> median gap = %.1f frames" %
              (avg_p * 1000, big[len(big) // 2] / avg_p))
    w()

    act = defaultdict(list)
    for i, f in enumerate(frames):
        for m in f["lua_methods"]:
            if m["incl_ms"] >= 0.15 or m["self_ms"] >= 0.1:
                act[m["name"]].append(i)

    avg_p = (sum(f["period_ms"] for f in frames) / n) / 1000.0
    w("== B. periodic Lua methods (>=8 activations, median gap 30..150 frames, CV<0.35) ==")
    w("  %-6s %-7s %-7s %-8s %s" % ("act", "med", "cv", "med_s", "name"))
    found = []
    for nm, idxs in act.items():
        if len(idxs) < 8:
            continue
        gs = sorted(g for g in (idxs[i + 1] - idxs[i] for i in range(len(idxs) - 1)) if g > 0)
        if len(gs) < 6:
            continue
        med = gs[len(gs) // 2]
        if not (30 <= med <= 150):
            continue
        mean = sum(gs) / float(len(gs))
        cv = (sum((g - mean) ** 2 for g in gs) / len(gs)) ** 0.5 / mean if mean else 9.0
        if cv < 0.35:
            found.append((cv, nm, med, len(idxs)))
    for cv, nm, med, na in sorted(found)[:40]:
        w("  %-6d %-7d %-7.3f %-8.2f %s" % (na, med, cv, med * avg_p, nm))
    if not found:
        w("  (none)")
    w("  -> compare these intervals against section A. No match = driver is outside Lua.")
    w()

    sp_idx = set(i for i, f in enumerate(frames)
                 if any(f["publish_sequence"] == g["publish_sequence"] for g in rd))
    tot = len(sp_idx) or 1

    w("== C. Lua methods firing within +-%d frames of a render spike ==" % K)
    cover = {}
    for nm, idxs in act.items():
        c = sum(1 for sp in sp_idx if any(abs(sp - j) <= K for j in idxs))
        if c:
            cover[nm] = c
    w("  %-6s %-7s %-8s %s" % ("cover", "pct", "active", "name"))
    for nm, c in sorted(cover.items(), key=lambda kv: -kv[1]):
        if c < 0.5 * tot:
            break
        w("  %-6d %-7s %-8d %s" %
          (c, "%.0f%%" % (100.0 * c / tot), len(act[nm]), nm))
    w()
    w("  -- interpretation --")
    w("  a high pct with a small 'active' count  = real co-trigger, investigate")
    w("  a high pct with 'active' ~ frame count  = generic per-frame infrastructure")
    w()

    w("== D. highest on-spike concentration (active in 5..200 frames) ==")
    conc = []
    for nm, idxs in act.items():
        if not (5 <= len(idxs) <= 200):
            continue
        c = sum(1 for j in idxs if any(abs(j - sp) <= K for sp in sp_idx))
        if c:
            conc.append((100.0 * c / len(idxs), c, len(idxs), nm))
    for pct, c, na, nm in sorted(conc, key=lambda r: (-r[0], -r[1]))[:30]:
        w("  on-spike %-5.0f%%  (%d/%d active frames)  %s" % (pct, c, na, nm))

    out.close()
    sys.stdout.write("report written: %s\n" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
