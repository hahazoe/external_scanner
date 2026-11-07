# -*- coding: utf-8 -*-
"""Shared streaming reader for .cocosdbg recordings (format versions 5 through 9).

Format source of truth: debugview/recording_io.cpp (kMagic CCDBREC1).
All integers little-endian. See references/format.md for the full spec.

v6 adds two fields over v5 (both default to 0 when reading a v5 file):
  - frame["outside_ms"]  Lua wall time in the gap before this frame's window.
                         NOT part of cpu_ms and not attributed to any method.
  - line["line_ms"]      Wall time on that line, including time spent inside
                         C calls made from it.  Prefer this over "samples":
                         samples counts VM instruction batches (1 per 4000
                         instructions) and is blind to C-call time.
v7 adds, after flags and before the section counts (0 on older files):
  - frame["actor_x"], frame["actor_y"], frame["actor_valid"]
                         Legacy actor-position fields; new captures are always 0.
v8 adds, after actor_valid and before the section counts (0 on older files):
  - frame["proc_ws_kb"]       Private working set, KB. Same number as Task Manager's Memory column.
  - frame["proc_private_kb"]  Process private commit, KB.
v9 adds, after CppMethod entries and before line samples (empty on older files):
  - frame["file_io"]     Per-path FileUtilsWin32:getData aggregates for the frame.

Usage:
    from cocosdbg_reader import iter_frames, MAGIC, VERSION
    for frame in iter_frames(path):
        ...  # frame is a dict
"""
import struct

MAGIC = b"CCDBREC1"
VERSION = 9
MIN_SUPPORTED_VERSION = 5

EVENT_CALL = 1
EVENT_RETURN = 2
EVENT_TAIL_RETURN = 3
EVENT_RESUME = 4
EVENT_SUSPEND = 5
EVENT_FRAME_BEGIN = 6
EVENT_FRAME_END = 7

# publish_sequence, begin_t, end_t, lua_frame_index, render_frame_sequence,
# frame_period_ms, drawscene_body_ms, lua_wall_ms, lua_cpu_ms,
# lua_script_self_ms, lua_c_self_ms, lua_mem_kb, hook_events, flags
_FRAME_HEADER_V5 = struct.Struct("<QddII7fII")
# v6 inserts lua_outside_ms right after lua_c_self_ms.
_FRAME_HEADER_V6 = struct.Struct("<QddII8fII")


class CocosDbgFormatError(Exception):
    pass


def _open_and_check_header(path):
    f, version, count = _open_and_check_header_versioned(path)
    return f, count


def _open_and_check_header_versioned(path):
    """Returns (file, version, frame_count).  Callers that need to know the
    on-disk version (v5 vs v6 change the frame layout) must use this."""
    f = open(path, "rb")

    def rd(n):
        b = f.read(n)
        if len(b) != n:
            raise CocosDbgFormatError("short read (%d bytes)" % n)
        return b

    def u32():
        return struct.unpack("<I", rd(4))[0]

    def u64():
        return struct.unpack("<Q", rd(8))[0]

    if rd(8) != MAGIC:
        f.close()
        raise CocosDbgFormatError("bad magic (not a .cocosdbg file)")
    version = u32()
    if version < MIN_SUPPORTED_VERSION or version > VERSION:
        f.close()
        raise CocosDbgFormatError(
            "unsupported version %d (supported %d..%d)"
            % (version, MIN_SUPPORTED_VERSION, VERSION))
    count = u64()
    if count > 50000000:
        f.close()
        raise CocosDbgFormatError("implausible frame count %d" % count)
    return f, version, count


def iter_frames(path):
    """Yield per-frame dicts:

    { publish_sequence, begin_s, end_s, lua_frame_index, render_frame_sequence,
      period_ms, drawscene_ms, wall_ms, cpu_ms, script_ms, c_ms,
      outside_ms, mem_kb, hook_events, flags, actor_x, actor_y, actor_valid,
      proc_ws_kb, proc_private_kb,
      lua_methods: [ {name, kind, flags, self_ms, incl_ms, max_self_ms,
                      max_incl_ms, started, completed, active} ],
      cpp_methods: [ {name, total_ms, max_ms, last_ms, calls, call_sequence} ],
      file_io:     [ {path, total_ms, max_ms, last_ms, calls} ],  # v9+
      lua_lines:   [ {source, function_name, line, samples, line_ms} ],
      trace_events:[ {event_sequence, qpc, time_seconds, frame_sequence,
                      lua_thread_id, os_thread_id, method_id, method_name,
                      depth, type, flags} ] }
    """
    f, version, count = _open_and_check_header_versioned(path)
    header = _FRAME_HEADER_V6 if version >= 6 else _FRAME_HEADER_V5
    try:
        for _ in range(count):
            values = header.unpack(f.read(header.size))
            if version >= 6:
                (pub, begin_s, end_s, lfi, rfs, period, drawscene, wall, cpu,
                 script, cself, outside_ms, mem_kb, hook_events, flags) = values
            else:
                (pub, begin_s, end_s, lfi, rfs, period, drawscene, wall, cpu,
                 script, cself, mem_kb, hook_events, flags) = values
                outside_ms = 0.0
            actor_x = 0.0
            actor_y = 0.0
            actor_valid = 0
            if version >= 7:
                actor_x, actor_y = struct.unpack("<ff", f.read(8))
                actor_valid = _u32(f)
            proc_ws_kb = 0.0
            proc_private_kb = 0.0
            if version >= 8:
                proc_ws_kb, proc_private_kb = struct.unpack("<ff", f.read(8))
            frame = {
                "publish_sequence": pub,
                "begin_s": begin_s,
                "end_s": end_s,
                "lua_frame_index": lfi,
                "render_frame_sequence": rfs,
                "period_ms": period,
                "drawscene_ms": drawscene,
                "wall_ms": wall,
                "cpu_ms": cpu,
                "script_ms": script,
                "c_ms": cself,
                "outside_ms": outside_ms,
                "mem_kb": mem_kb,
                "hook_events": hook_events,
                "flags": flags,
                "actor_x": actor_x,
                "actor_y": actor_y,
                "actor_valid": actor_valid,
                "proc_ws_kb": proc_ws_kb,
                "proc_private_kb": proc_private_kb,
                "lua_methods": [],
                "cpp_methods": [],
                "file_io": [],
                "lua_lines": [],
                "trace_events": [],
            }
            lc = _u32(f)
            cc = _u32(f)
            lnc = _u32(f)
            ec = _u32(f)
            if lc > 1000000 or cc > 100000 or lnc > 1000000 or ec > 10000000:
                raise CocosDbgFormatError("implausible section counts")
            for _ in range(lc):
                frame["lua_methods"].append({
                    "name": _rstr(f),
                    "kind": _u32(f),
                    "flags": _u32(f),
                    "self_ms": _f32(f),
                    "incl_ms": _f32(f),
                    "max_self_ms": _f32(f),
                    "max_incl_ms": _f32(f),
                    "started": _u32(f),
                    "completed": _u32(f),
                    "active": _u32(f),
                })
            for _ in range(cc):
                frame["cpp_methods"].append({
                    "name": _rstr(f),
                    "total_ms": _f32(f),
                    "max_ms": _f32(f),
                    "last_ms": _f32(f),
                    "calls": _u32(f),
                    "call_sequence": _u32(f),
                })
            if version >= 9:
                fic = _u32(f)
                if fic > 100000:
                    raise CocosDbgFormatError("implausible file_io count")
                for _ in range(fic):
                    frame["file_io"].append({
                        "path": _rstr(f),
                        "total_ms": _f32(f),
                        "max_ms": _f32(f),
                        "last_ms": _f32(f),
                        "calls": _u32(f),
                    })
            for _ in range(lnc):
                source = _rstr(f)
                function_name = _rstr(f)
                line = _u32(f)
                samples = _u32(f)
                frame["lua_lines"].append({
                    "source": source,
                    "function_name": function_name,
                    "line": line,
                    "samples": samples,
                    "line_ms": _f32(f) if version >= 6 else 0.0,
                })
            for _ in range(ec):
                frame["trace_events"].append({
                    "event_sequence": _u32(f),
                    "qpc": _u64(f),
                    "time_seconds": _f64(f),
                    "frame_sequence": _u64(f),
                    "lua_thread_id": _u32(f),
                    "os_thread_id": _u32(f),
                    "method_id": _u32(f),
                    "method_name": _rstr(f),
                    "depth": _u32(f),
                    "type": _u32(f),
                    "flags": _u32(f),
                })
            yield frame
    finally:
        f.close()


def _u32(f):
    b = f.read(4)
    if len(b) != 4:
        raise CocosDbgFormatError("short read")
    return struct.unpack("<I", b)[0]


def _u64(f):
    b = f.read(8)
    if len(b) != 8:
        raise CocosDbgFormatError("short read")
    return struct.unpack("<Q", b)[0]


def _f32(f):
    b = f.read(4)
    if len(b) != 4:
        raise CocosDbgFormatError("short read")
    return struct.unpack("<f", b)[0]


def _f64(f):
    b = f.read(8)
    if len(b) != 8:
        raise CocosDbgFormatError("short read")
    return struct.unpack("<d", b)[0]


def _rstr(f):
    n = _u32(f)
    if n == 0:
        return ""
    if n > 0x100000:
        raise CocosDbgFormatError("implausible string length %d" % n)
    b = f.read(n)
    if len(b) != n:
        raise CocosDbgFormatError("short read in string")
    return b.decode("utf-8", "replace")
