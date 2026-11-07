#!/usr/bin/env python3
"""把 dump.log 里的 C++ 崩溃栈用 Breakpad .sym 符号化。

每个参数都有脚本顶部的默认值，也可以用命令行覆盖。
"""

from __future__ import annotations

import argparse
import bisect
import json
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

# 直接改这些常量即可运行；命令行传入同名参数时覆盖它们。
DUMP_LOG = r"E:\20260929R1_win\1.1\99.99.0.0_full\dump.log"
SYM_ZIP = r"E:\20260929R1_win\1.1\99.99.0.0_full\breakpad_syms.zip"
ZIP_PASSWORD = "SealBreakpadSym"
SYM_DIR = ""
OUTPUT = ""

EXCEPTION_NAMES = {
    0xC0000005: "访问违例",
    0xC0000409: "栈缓冲区溢出",
    0xC0000094: "整数除零",
    0xC0000096: "特权指令",
    0xC000001D: "非法指令",
    0xC00000FD: "栈溢出",
    0x80000003: "断点",
    0x40000015: "致命退出",
}

ZIP_AES = 99


@dataclass
class DumpModule:
    name: str
    base: int
    size: int
    timestamp: int


@dataclass
class StackFrame:
    index: int
    pc: int
    module: str
    base: int
    rva: int


@dataclass
class DumpLog:
    when: str
    exception_code: int | None
    exception_address: int | None
    exception_module: str
    exception_rva: int | None
    access_address: int | None
    modules: dict[str, DumpModule]
    frames: list[StackFrame]
    lua_callback: str


@dataclass
class FuncSym:
    addr: int
    size: int
    name: str
    line_addrs: list[int] = field(default_factory=list)
    line_nos: list[int] = field(default_factory=list)
    line_files: list[int] = field(default_factory=list)


@dataclass
class ModuleSym:
    name: str
    code_id: str
    timestamp: int | None
    image_size: int | None
    files: dict[int, str]
    funcs: list[FuncSym]
    func_addrs: list[int]
    publics: list[tuple[int, str]]
    public_addrs: list[int]


@dataclass
class ResolvedFrame:
    function: str
    offset: int
    source: str
    line: int | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="用 Breakpad .sym 符号化 dump.log 的 C++ 崩溃堆栈。"
    )
    parser.add_argument("--dump", default=DUMP_LOG, help="dump.log 路径")
    parser.add_argument("--sym-zip", default=SYM_ZIP, help="breakpad_syms.zip 路径")
    parser.add_argument("--password", default=ZIP_PASSWORD, help="符号 zip 密码")
    parser.add_argument(
        "--sym-dir",
        default=SYM_DIR,
        help="已解压的 .sym 目录。非空时不再读取 zip",
    )
    parser.add_argument(
        "--output",
        default=OUTPUT,
        help="报告输出文件。空则打印到标准输出",
    )
    parser.add_argument(
        "--format",
        default="text",
        choices=("text", "json"),
        help="报告格式。默认 text，json 供界面读取",
    )
    return parser.parse_args()


def parse_int(text: str) -> int | None:
    text = text.strip()
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        return None


def parse_dump(path: Path) -> DumpLog:
    raw = path.read_bytes()
    text = raw.decode("utf-8-sig", errors="replace")
    lines = text.splitlines()

    when = ""
    exception_code = None
    exception_address = None
    exception_module = ""
    exception_rva = None
    access_address = None
    modules: dict[str, DumpModule] = {}
    frames: list[StackFrame] = []
    lua_start = None

    for index, line in enumerate(lines):
        stripped = line.strip()
        if not when and stripped:
            when = stripped
        if stripped.startswith("Exception Code "):
            exception_code = parse_int(stripped.split()[-1])
        elif stripped.startswith("Exception Address "):
            parts = stripped.split()
            if len(parts) >= 3:
                exception_address = parse_int(parts[2])
                exception_module = parts[3]
            for part in parts:
                if part.startswith("rva="):
                    exception_rva = parse_int(part.split("=", 1)[1])
        elif stripped.lower().startswith("address attemp"):
            access_address = parse_int(stripped.split()[-1])
        elif stripped == "LuaCallBack:":
            lua_start = index
            break

        frame = _parse_frame_line(stripped)
        if frame is not None:
            frames.append(
                StackFrame(len(frames), frame[0], frame[1], frame[2], frame[3])
            )
            continue

        module = _parse_module_line(stripped)
        if module is not None:
            modules[module.name.lower()] = module

    lua_callback = ""
    if lua_start is not None:
        lua_callback = "\n".join(lines[lua_start:]).strip()

    return DumpLog(
        when=when,
        exception_code=exception_code,
        exception_address=exception_address,
        exception_module=exception_module,
        exception_rva=exception_rva,
        access_address=access_address,
        modules=modules,
        frames=frames,
        lua_callback=lua_callback,
    )


def _parse_frame_line(line: str) -> tuple[int, str, int, int] | None:
    parts = line.split()
    if len(parts) < 4 or not parts[0].startswith("0x"):
        return None
    if not parts[2].startswith("base=") or not parts[3].startswith("rva="):
        return None
    pc = parse_int(parts[0])
    base = parse_int(parts[2].split("=", 1)[1])
    rva = parse_int(parts[3].split("=", 1)[1])
    if pc is None or base is None or rva is None:
        return None
    return pc, parts[1], base, rva


def _parse_module_line(line: str) -> DumpModule | None:
    parts = line.split()
    if len(parts) != 4:
        return None
    if not parts[1].startswith("base="):
        return None
    if not parts[2].startswith("size=") or not parts[3].startswith("ts="):
        return None
    base = parse_int(parts[1].split("=", 1)[1])
    size = parse_int(parts[2].split("=", 1)[1])
    timestamp = parse_int(parts[3].split("=", 1)[1])
    if base is None or size is None or timestamp is None:
        return None
    return DumpModule(parts[0], base, size, timestamp)


def clean_path(path: str) -> str:
    if path.startswith("\\\\?\\"):
        return path[4:]
    return path


def split_code_id(code_id: str) -> tuple[int | None, int | None]:
    # Windows CODE_ID 是 8 位时间戳十六进制，后面紧跟映像大小（去掉前导 0）。
    if len(code_id) <= 8:
        return None, None
    try:
        return int(code_id[:8], 16), int(code_id[8:], 16)
    except ValueError:
        return None, None


def parse_sym(text: str) -> ModuleSym | None:
    files: dict[int, str] = {}
    funcs: list[FuncSym] = []
    publics: list[tuple[int, str]] = []
    current: FuncSym | None = None
    module_name = ""
    code_id = ""

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            current = None
            continue
        if line.startswith("MODULE "):
            module_parts = line.split()
            if len(module_parts) >= 5 and not module_name:
                module_name = module_parts[-1]
            current = None
            continue
        if line.startswith("INFO CODE_ID "):
            rest = line[len("INFO CODE_ID ") :]
            code_part, _, filename = rest.partition(" ")
            code_id = code_part.strip()
            if filename.strip():
                module_name = filename.strip()
            current = None
            continue
        if line.startswith("FILE "):
            parts = line.split(" ", 2)
            if len(parts) >= 3:
                try:
                    files[int(parts[1])] = parts[2]
                except ValueError:
                    pass
            current = None
            continue
        if line.startswith("FUNC "):
            parsed = _parse_func(line)
            current = parsed
            if parsed is not None:
                funcs.append(parsed)
            continue
        if line.startswith("PUBLIC "):
            parsed_public = _parse_public(line)
            if parsed_public is not None:
                publics.append(parsed_public)
            current = None
            continue
        if current is not None and line[0] in "0123456789abcdefABCDEF":
            parts = line.split()
            if len(parts) >= 4:
                try:
                    current.line_addrs.append(int(parts[0], 16))
                    current.line_nos.append(int(parts[2]))
                    current.line_files.append(int(parts[3]))
                except ValueError:
                    pass
            continue
        current = None

    if not module_name:
        return None

    for func in funcs:
        if len(func.line_addrs) > 1:
            order = sorted(range(len(func.line_addrs)), key=func.line_addrs.__getitem__)
            func.line_addrs = [func.line_addrs[i] for i in order]
            func.line_nos = [func.line_nos[i] for i in order]
            func.line_files = [func.line_files[i] for i in order]

    funcs.sort(key=lambda item: item.addr)
    publics.sort(key=lambda item: item[0])
    timestamp, image_size = split_code_id(code_id)
    return ModuleSym(
        name=module_name,
        code_id=code_id,
        timestamp=timestamp,
        image_size=image_size,
        files=files,
        funcs=funcs,
        func_addrs=[item.addr for item in funcs],
        publics=publics,
        public_addrs=[item[0] for item in publics],
    )


def _skip_multiple_flag(parts: list[str]) -> int:
    index = 1
    if index < len(parts) and parts[index] == "m":
        index += 1
    return index


def _parse_func(line: str) -> FuncSym | None:
    parts = line.split(" ")
    index = _skip_multiple_flag(parts)
    if len(parts) < index + 3:
        return None
    try:
        addr = int(parts[index], 16)
        size = int(parts[index + 1], 16)
    except ValueError:
        return None
    name = " ".join(parts[index + 3 :]).strip()
    return FuncSym(addr, size, name or "<unknown>")


def _parse_public(line: str) -> tuple[int, str] | None:
    parts = line.split(" ")
    index = _skip_multiple_flag(parts)
    if len(parts) < index + 2:
        return None
    try:
        addr = int(parts[index], 16)
    except ValueError:
        return None
    name = " ".join(parts[index + 2 :]).strip()
    return addr, name or "<unknown>"


def find_7z() -> str | None:
    found = shutil.which("7z")
    if found:
        return found
    candidates = [
        r"C:\Program Files\7-Zip\7z.exe",
        r"C:\Program Files (x86)\7-Zip\7z.exe",
    ]
    for candidate in candidates:
        if Path(candidate).is_file():
            return candidate
    return None


def _sym_names_in_zip(zip_path: Path) -> tuple[list[str], bool]:
    with zipfile.ZipFile(zip_path) as archive:
        names = [
            info.filename
            for info in archive.infolist()
            if info.filename.lower().endswith(".sym") and not info.is_dir()
        ]
        aes = any(info.compress_type == ZIP_AES for info in archive.infolist())
    return names, aes


def _read_pyzipper(zip_path: Path, password: str, names: list[str]) -> list[tuple[str, str]]:
    import pyzipper

    blobs: list[tuple[str, str]] = []
    with pyzipper.AESZipFile(zip_path) as archive:
        archive.pwd = password.encode("utf-8") if password else None
        for name in names:
            blobs.append((name, archive.read(name).decode("utf-8-sig", errors="replace")))
    return blobs


def _read_zipfile(zip_path: Path, password: str, names: list[str]) -> list[tuple[str, str]]:
    pwd = password.encode("utf-8") if password else None
    blobs: list[tuple[str, str]] = []
    with zipfile.ZipFile(zip_path) as archive:
        for name in names:
            info = archive.getinfo(name)
            if info.compress_type == ZIP_AES:
                raise RuntimeError("标准库 zipfile 不能解 WinZip AES（压缩方法 99）")
            blobs.append(
                (name, archive.read(name, pwd=pwd).decode("utf-8-sig", errors="replace"))
            )
    return blobs


def _read_7z(zip_path: Path, password: str, names: list[str]) -> list[tuple[str, str]]:
    seven = find_7z()
    if not seven:
        raise RuntimeError("未找到 7z.exe")
    blobs: list[tuple[str, str]] = []
    password_arg = "-p" + password
    for name in names:
        completed = subprocess.run(
            [seven, "e", "-so", "-y", password_arg, str(zip_path), name],
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0 or not completed.stdout:
            detail = completed.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(f"7-Zip 解压 {name} 失败: {detail or completed.returncode}")
        blobs.append((name, completed.stdout.decode("utf-8-sig", errors="replace")))
    return blobs


def _read_bssym_tree(zip_path: Path) -> list[tuple[str, str]] | None:
    """Read .sym members, including those nested in breakpad_syms.zip.

    BSSYM1 blobs are XOR'd zlib. Plaintext .sym members pass through.
    Returns None when this is not a standard zip the walker can open
    (for example a WinZip AES package), so the caller can fall back.
    """
    from decrypt_bssym import decrypt_bssym, iter_sym_blobs

    try:
        found = list(iter_sym_blobs(zip_path))
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile):
        return None
    if not found:
        return None
    blobs: list[tuple[str, str]] = []
    for name, raw in found:
        blobs.append((name, decrypt_bssym(raw).decode("utf-8-sig", errors="replace")))
    return blobs


def read_zip_syms(zip_path: Path, password: str) -> list[tuple[str, str]]:
    if not zip_path.is_file():
        raise FileNotFoundError(f"找不到符号包: {zip_path}")
    tree = _read_bssym_tree(zip_path)
    if tree:
        return tree
    names, aes = _sym_names_in_zip(zip_path)
    if not names:
        raise RuntimeError(f"符号包里没有 .sym 文件: {zip_path}")

    errors: list[str] = []
    if aes:
        try:
            import pyzipper  # noqa: F401
        except ImportError:
            errors.append("未安装 pyzipper，跳过 AES 直接读取")
        else:
            try:
                return _read_pyzipper(zip_path, password, names)
            except Exception as exc:
                errors.append(f"pyzipper: {exc}")
        try:
            return _read_7z(zip_path, password, names)
        except Exception as exc:
            errors.append(f"7z: {exc}")
        errors.append("标准库 zipfile 不能解 WinZip AES")
        raise RuntimeError("无法读取加密符号包:\n" + "\n".join(errors))

    try:
        return _read_zipfile(zip_path, password, names)
    except Exception as exc:
        errors.append(f"zipfile: {exc}")
    try:
        import pyzipper  # noqa: F401
    except ImportError:
        errors.append("未安装 pyzipper")
    else:
        try:
            return _read_pyzipper(zip_path, password, names)
        except Exception as exc:
            errors.append(f"pyzipper: {exc}")
    try:
        return _read_7z(zip_path, password, names)
    except Exception as exc:
        errors.append(f"7z: {exc}")
    raise RuntimeError("无法读取符号包:\n" + "\n".join(errors))


def read_dir_syms(sym_dir: Path) -> list[tuple[str, str]]:
    if not sym_dir.is_dir():
        raise FileNotFoundError(f"找不到符号目录: {sym_dir}")
    paths = sorted(sym_dir.rglob("*.sym"))
    if not paths:
        raise RuntimeError(f"目录里没有 .sym 文件: {sym_dir}")
    blobs: list[tuple[str, str]] = []
    for path in paths:
        blobs.append((str(path), path.read_text(encoding="utf-8-sig", errors="replace")))
    return blobs


def load_symbols(sym_zip: str, password: str, sym_dir: str) -> dict[str, list[ModuleSym]]:
    if sym_dir:
        blobs = read_dir_syms(Path(sym_dir))
    else:
        if not sym_zip:
            raise RuntimeError("未指定 --sym-zip 或 --sym-dir")
        blobs = read_zip_syms(Path(sym_zip), password)

    grouped: dict[str, list[ModuleSym]] = {}
    for _name, text in blobs:
        parsed = parse_sym(text)
        if parsed is None:
            continue
        grouped.setdefault(parsed.name.lower(), []).append(parsed)
    if not grouped:
        raise RuntimeError("没有解析出任何模块符号")
    return grouped


def select_module(
    candidates: list[ModuleSym], dump_module: DumpModule | None
) -> tuple[ModuleSym | None, str]:
    if not candidates:
        return None, ""
    if dump_module is not None:
        matched = [
            item
            for item in candidates
            if item.timestamp == dump_module.timestamp
            and item.image_size == dump_module.size
        ]
        if len(matched) == 1:
            return matched[0], "与模块时间戳和大小匹配"
        if len(matched) > 1:
            return matched[0], "多份符号的 CODE_ID 都匹配，使用第一份"
    if len(candidates) == 1:
        only = candidates[0]
        if (
            dump_module is not None
            and only.timestamp is not None
            and (
                only.timestamp != dump_module.timestamp
                or only.image_size != dump_module.size
            )
        ):
            return only, (
                "与 dump 不一致 "
                f"(符号 ts=0x{only.timestamp:x} size=0x{only.image_size or 0:x}, "
                f"dump ts=0x{dump_module.timestamp:x} size=0x{dump_module.size:x})"
            )
        if dump_module is None:
            return only, "dump 未提供该模块时间戳"
        return only, "与模块时间戳和大小匹配"
    return None, "有多份符号但 CODE_ID 都不匹配，已跳过"


def lookup(module: ModuleSym, rva: int, adjust: bool) -> ResolvedFrame | None:
    query = rva - 1 if adjust and rva > 0 else rva
    index = bisect.bisect_right(module.func_addrs, query) - 1
    if index >= 0:
        func = module.funcs[index]
        if func.size > 0 and func.addr <= query < func.addr + func.size:
            source = ""
            line_no = None
            line_index = bisect.bisect_right(func.line_addrs, query) - 1
            if line_index >= 0:
                line_no = func.line_nos[line_index]
                source = clean_path(module.files.get(func.line_files[line_index], ""))
            return ResolvedFrame(func.name, query - func.addr, source, line_no)

    public_index = bisect.bisect_right(module.public_addrs, query) - 1
    if public_index >= 0:
        addr, name = module.publics[public_index]
        return ResolvedFrame(name, query - addr, "", None)
    return None


def is_return_address(frame: StackFrame, dump: DumpLog) -> bool:
    if dump.exception_address is None:
        return frame.index != 0
    return frame.pc != dump.exception_address


def is_crash_frame(frame: StackFrame, dump: DumpLog) -> bool:
    if dump.exception_address is None:
        return frame.index == 0
    return frame.pc == dump.exception_address


def symbol_matches(status: str) -> bool:
    return "匹配" in status and "不匹配" not in status


def hex_word(value: int | None) -> str:
    if value is None:
        return "?"
    return f"0x{value:x}"


def build_report(dump: DumpLog, symbols: dict[str, list[ModuleSym]]) -> dict:
    if dump.exception_code is None:
        code_text = None
        code_name = ""
    else:
        code_text = f"0x{dump.exception_code:08x}"
        code_name = EXCEPTION_NAMES.get(dump.exception_code, "")

    chosen: dict[str, tuple[ModuleSym | None, str]] = {}
    symbol_rows: list[dict] = []
    used_names = {frame.module.lower() for frame in dump.frames}
    if dump.exception_module:
        used_names.add(dump.exception_module.lower())
    for name in sorted(used_names):
        candidates = symbols.get(name, [])
        if not candidates:
            continue
        module, status = select_module(candidates, dump.modules.get(name))
        chosen[name] = (module, status)
        symbol_rows.append(
            {
                "module": name,
                "code_id": module.code_id if module is not None else "?",
                "status": status,
                "matched": symbol_matches(status),
            }
        )

    frame_rows: list[dict] = []
    for frame in dump.frames:
        row = {
            "index": frame.index,
            "module": frame.module,
            "rva": hex_word(frame.rva),
            "function": "",
            "offset": None,
            "source": "",
            "line": None,
            "resolved": False,
            "crash": is_crash_frame(frame, dump),
            "note": "",
        }
        selected = chosen.get(frame.module.lower())
        module = selected[0] if selected else None
        if module is None:
            row["note"] = "无符号"
        else:
            resolved = lookup(module, frame.rva, is_return_address(frame, dump))
            if resolved is None:
                row["note"] = "地址不在符号范围内"
            else:
                row["resolved"] = True
                row["function"] = resolved.function
                row["offset"] = hex_word(resolved.offset)
                row["source"] = resolved.source
                row["line"] = resolved.line
        frame_rows.append(row)

    return {
        "time": dump.when or "?",
        "exception_code": code_text,
        "exception_name": code_name,
        "exception_address": hex_word(dump.exception_address),
        "exception_module": dump.exception_module or "?",
        "exception_rva": hex_word(dump.exception_rva),
        "access_address": (
            None if dump.access_address is None else hex_word(dump.access_address)
        ),
        "symbols": symbol_rows,
        "frames": frame_rows,
        "lua_callback": dump.lua_callback,
    }


def render_text_report(report: dict) -> str:
    lines: list[str] = [f"时间: {report['time']}"]
    if report["exception_code"] is None:
        lines.append("异常码: ?")
    else:
        suffix = f" {report['exception_name']}" if report["exception_name"] else ""
        lines.append(f"异常码: {report['exception_code']}{suffix}")
    lines.append(
        "异常地址: "
        f"{report['exception_address']}  {report['exception_module']}  "
        f"rva={report['exception_rva']}"
    )
    if report["access_address"] is not None:
        lines.append(f"访问地址: {report['access_address']}")
    for item in report["symbols"]:
        lines.append(
            f"符号: {item['module']}  CODE_ID {item['code_id']}  {item['status']}"
        )

    lines.append("")
    lines.append("C++ 调用栈:")
    if not report["frames"]:
        lines.append("（没有解析到调用栈）")
    for frame in report["frames"]:
        lines.append(
            f"#{frame['index']:02d}  {frame['module']}  rva={frame['rva']}"
        )
        if frame["note"] == "无符号":
            lines.append("     （无符号）")
            continue
        if frame["note"] == "地址不在符号范围内":
            lines.append("     （地址不在符号范围内）")
            continue
        lines.append(f"     {frame['function']} + {frame['offset']}")
        if frame["source"] and frame["line"] is not None:
            lines.append(f"     {frame['source']}:{frame['line']}")

    if report["lua_callback"]:
        lines.append("")
        lines.append(report["lua_callback"])
    lines.append("")
    return "\n".join(lines)


def render_json_report(report: dict) -> str:
    return json.dumps(report, ensure_ascii=False, indent=2) + "\n"


def render_report(report: dict, report_format: str) -> str:
    if report_format == "json":
        return render_json_report(report)
    return render_text_report(report)


def emit(text: str, output_path: str) -> None:
    if output_path:
        destination = Path(output_path)
        destination.write_text(text, encoding="utf-8")
        print(f"已写入 {destination}", file=sys.stderr)
        return
    encoding = getattr(sys.stdout, "encoding", None) or "utf-8"
    sys.stdout.buffer.write(text.encode(encoding, errors="replace"))


def main() -> int:
    args = parse_args()
    dump_path = Path(args.dump)
    if not dump_path.is_file():
        print(f"找不到 dump 文件: {dump_path}", file=sys.stderr)
        return 1
    try:
        dump = parse_dump(dump_path)
        symbols = load_symbols(args.sym_zip, args.password, args.sym_dir)
        report = render_report(build_report(dump, symbols), args.format)
    except (OSError, RuntimeError, zipfile.BadZipFile) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    emit(report, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
