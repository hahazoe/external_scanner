# -*- coding: utf-8 -*-
"""Temporal correlation analysis for .cocosdbg recordings.

Usage:
    python correlation_analysis.py <capture.cocosdbg> [report.txt] [key=substring ...]

Without watch specs, auto-detects "suspect periodic logic": Lua methods that
are active in only a small fraction of frames but carry high self cost when
active (the signature of periodic debug logging / batch processing).

With watch specs like `dump_=functions.luac"]:61` (substring match on method
name), reports per-frame call rates, active-frame intervals, and pairwise
co-occurrence between watched keys.
"""
import io
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cocosdbg_reader import iter_frames

SUSPECT_MAX_ACTIVE_RATIO = 0.20   # active in <=20% of frames
SUSPECT_MIN_SELF_MS = 100.0       # and >=100ms total self


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: correlation_analysis.py <capture.cocosdbg> "
                         "[report.txt] [key=substring ...]\n")
        return 64
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(path)[0] + "_correlation.txt"
    watch = {}
    for spec in sys.argv[3:]:
        if "=" in spec:
            k, sub = spec.split("=", 1)
            watch[k] = sub

    # method -> stats
    stat = {}  # name -> [total_self, started_total, active_frames(list of seq)]
    watch_counts = defaultdict(list)  # key -> [(seq, started, self_ms)]

    n = 0
    for fr in iter_frames(path):
        n += 1
        seq = fr["publish_sequence"]
        for m in fr["lua_methods"]:
            rec = stat.get(m["name"])
            if rec is None:
                rec = stat[m["name"]] = [0.0, 0, []]
            rec[0] += m["self_ms"]
            rec[1] += m["started"]
            if m["started"] > 0 or m["self_ms"] > 0.02:
                rec[2].append(seq)
        for key, sub in watch.items():
            if sub in m["name"] and (m["started"] > 0 or m["self_ms"] > 0.02):
                watch_counts[key].append((seq, m["started"], m["self_ms"]))

    out = io.open(out_path, "w", encoding="utf-8")

    def w(s):
        out.write(s + "\n")

    w("capture: %s" % path)
    w("frames: %d" % n)
    if n == 0:
        out.close()
        return 1

    w("")
    w("== suspect periodic/burst logic (active in <=%.0f%% frames, self>=%.0fms) ==" %
      (SUSPECT_MAX_ACTIVE_RATIO * 100, SUSPECT_MIN_SELF_MS))
    suspects = []
    for name, (self_ms, started, seqs) in stat.items():
        if not seqs:
            continue
        ratio = len(seqs) / float(n)
        if ratio <= SUSPECT_MAX_ACTIVE_RATIO and self_ms >= SUSPECT_MIN_SELF_MS:
            suspects.append((self_ms, ratio, name, seqs))
    if not suspects:
        w("  (none)")
    for self_ms, ratio, name, seqs in sorted(suspects, key=lambda t: -t[0]):
        per_frame = self_ms / len(seqs)
        iv = Counter(b - a for a, b in zip(seqs, seqs[1:]))
        w("  self=%8.1fms  active=%4d frames (%.1f%%)  cost/active-frame=%6.2fms" %
          (self_ms, len(seqs), ratio * 100, per_frame))
        w("    %s" % name)
        w("    interval histogram (top): %s" % iv.most_common(5))

    if watch:
        w("")
        w("== watched methods ==")
        for key, rows in sorted(watch_counts.items()):
            total_calls = sum(r[1] for r in rows)
            total_self = sum(r[2] for r in rows)
            seqs = [r[0] for r in rows]
            iv = Counter(b - a for a, b in zip(seqs, seqs[1:]))
            w("  [%s] active_frames=%d  calls_total=%d (%.1f/frame avg)  self_total=%.1fms" %
              (key, len(rows), total_calls, total_calls / float(n), total_self))
            w("    interval histogram (top): %s" % iv.most_common(5))
        # pairwise co-occurrence
        keys = sorted(watch_counts.keys())
        if len(keys) > 1:
            w("")
            w("== co-occurrence (frames where both active) ==")
            for i in range(len(keys)):
                for j in range(i + 1, len(keys)):
                    a = set(r[0] for r in watch_counts[keys[i]])
                    b = set(r[0] for r in watch_counts[keys[j]])
                    w("  %-12s & %-12s : %d frames (of %d / %d)" %
                      (keys[i], keys[j], len(a & b), len(a), len(b)))

    out.close()
    print("report written: %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
