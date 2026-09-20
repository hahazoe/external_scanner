# -*- coding: utf-8 -*-
"""Trace-event call-stack analysis for .cocosdbg recordings.

Usage:
    python trace_analysis.py <capture.cocosdbg> [report.txt] [topN=15]

Rebuilds per-thread call stacks from trace events (requires the recording to
have been made with the event stream enabled). For the topN hottest Lua
methods by call count, reports caller distribution. Also disambiguates
[C]:<anonymous> by method_id.

If the recording contains zero trace events, prints guidance instead.
"""
import io
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cocosdbg_reader import iter_frames, EVENT_CALL, EVENT_RETURN, EVENT_TAIL_RETURN


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: trace_analysis.py <capture.cocosdbg> "
                         "[report.txt] [topN]\n")
        return 64
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(path)[0] + "_trace.txt"
    top_n = int(sys.argv[3]) if len(sys.argv) > 3 else 15

    call_counts = defaultdict(int)
    callers = defaultdict(lambda: defaultdict(int))
    anon_by_mid = defaultdict(int)
    anon_callers = defaultdict(lambda: defaultdict(int))
    stacks = {}
    total_events = 0
    frames_seen = 0

    def parent(st):
        return st[-2] if len(st) >= 2 else "<root>"

    for fr in iter_frames(path):
        frames_seen += 1
        for ev in fr["trace_events"]:
            total_events += 1
            key = (ev["os_thread_id"], ev["lua_thread_id"])
            st = stacks.setdefault(key, [])
            if ev["type"] == EVENT_CALL:
                st.append(ev["method_name"])
                call_counts[ev["method_name"]] += 1
                if ev["method_name"] == "[C]:<anonymous>":
                    anon_by_mid[ev["method_id"]] += 1
                    anon_callers[ev["method_id"]][parent(st)] += 1
                else:
                    callers[ev["method_name"]][parent(st)] += 1
            elif ev["type"] in (EVENT_RETURN, EVENT_TAIL_RETURN):
                if st:
                    st.pop()

    out = io.open(out_path, "w", encoding="utf-8")

    def w(s):
        out.write(s + "\n")

    w("capture: %s" % path)
    w("frames: %d  trace events: %d" % (frames_seen, total_events))
    if total_events == 0:
        w("")
        w("!! This recording has NO trace events.")
        w("!! The event stream was not enabled at capture time.")
        w("!! Re-record with the DebugView event stream enabled")
        w("!! (IPC feature EVENT_STREAM) to get caller attribution.")
        out.close()
        print("no trace events; guidance written: %s" % out_path)
        return 0

    w("")
    w("== top %d methods by trace call count: caller distribution ==" % top_n)
    for name, cnt in sorted(call_counts.items(), key=lambda kv: -kv[1])[:top_n]:
        w("  %s  calls=%d" % (name, cnt))
        for pname, pc in sorted(callers[name].items(), key=lambda kv: -kv[1])[:5]:
            w("      <- %6d  %s" % (pc, pname))

    w("")
    w("== [C]:<anonymous> split by method_id (top 15) ==")
    for mid, cnt in sorted(anon_by_mid.items(), key=lambda kv: -kv[1])[:15]:
        w("  method_id=%u calls=%d" % (mid, cnt))
        for pname, pc in sorted(anon_callers[mid].items(), key=lambda kv: -kv[1])[:3]:
            w("      <- %6d  %s" % (pc, pname))

    out.close()
    print("report written: %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
