"""Decrypt Breakpad .sym blobs that use the BSSYM1 wrapper.

Layout (little-endian):
    offset 0..5   magic "BSSYM1"
    offset 6..9   plainLen  uint32, decompressed length (0 = skip check)
    offset 10..   payload   XOR'd zlib stream

Decrypt order is fixed: XOR, then zlib inflate.
XOR is symmetric with the encrypt side:
    key = b"SealBreakpadSymXorKey!v1"
    payload[i] ^= key[i % len(key)] ^ (i & 0xFF)
Encrypt was XOR(zlib.compress(plain, level=9)).

The outer zip is not encrypted. Only .sym members (including those inside
a nested breakpad_syms.zip) use this wrapper. A member that does not start
with BSSYM1 is already plaintext and is copied through.
"""

from __future__ import annotations

import argparse
import io
import sys
import zipfile
import zlib
from pathlib import Path

MAGIC = b"BSSYM1"
KEY = b"SealBreakpadSymXorKey!v1"


def decrypt_bssym(blob: bytes) -> bytes:
    """Return Breakpad plaintext. Non-BSSYM1 input is returned unchanged."""
    if not blob.startswith(MAGIC):
        return blob
    if len(blob) < 10:
        raise ValueError("BSSYM1 header is truncated")

    plain_len = int.from_bytes(blob[6:10], "little")
    payload = bytearray(blob[10:])
    key_len = len(KEY)
    for i in range(len(payload)):
        payload[i] ^= KEY[i % key_len] ^ (i & 0xFF)

    try:
        plain = zlib.decompress(payload)
    except zlib.error as exc:
        raise ValueError(f"zlib inflate failed: {exc}") from exc

    if plain_len != 0 and len(plain) != plain_len:
        raise ValueError(
            f"plain length mismatch: header {plain_len}, inflated {len(plain)}"
        )
    return plain


def _is_zip(data: bytes) -> bool:
    return data.startswith(b"PK\x03\x04") or data.startswith(b"PK\x05\x06")


def iter_sym_blobs(source: Path):
    """Yield (relative_name, raw_bytes) for every .sym under a file or zip."""
    if source.is_dir():
        for path in sorted(source.rglob("*")):
            if path.is_file() and path.suffix.lower() == ".sym":
                yield path.relative_to(source).as_posix(), path.read_bytes()
            elif path.is_file() and path.suffix.lower() == ".zip":
                yield from _iter_zip_syms(path.read_bytes(), path.name + "/")
        return

    data = source.read_bytes()
    if source.suffix.lower() == ".sym":
        yield source.name, data
        return
    if source.suffix.lower() == ".zip" or _is_zip(data):
        yield from _iter_zip_syms(data, "")
        return
    raise ValueError(f"unsupported input: {source}")


def _iter_zip_syms(data: bytes, prefix: str):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/")
            raw = archive.read(info)
            lower = name.lower()
            if lower.endswith(".sym"):
                yield prefix + name, raw
            elif lower.endswith(".zip"):
                yield from _iter_zip_syms(raw, prefix + name + "/")


def decrypt_tree(source: Path, output_dir: Path) -> list[tuple[str, int, int, str]]:
    """Decrypt every .sym and write plaintext. Returns a short report row per file."""
    output_dir.mkdir(parents=True, exist_ok=True)
    rows: list[tuple[str, int, int, str]] = []
    found = False
    for rel, raw in iter_sym_blobs(source):
        found = True
        plain = decrypt_bssym(raw)
        dest = output_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(plain)
        head = plain.split(b"\n", 1)[0].decode("utf-8", "replace")
        rows.append((rel, len(raw), len(plain), head))
    if not found:
        raise ValueError(f"no .sym members in {source}")
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Decrypt BSSYM1 Breakpad .sym files.")
    parser.add_argument("source", type=Path, help="zip, nested zip, directory, or a .sym file")
    parser.add_argument(
        "output",
        type=Path,
        nargs="?",
        help="output directory (default: <source>_syms next to the input)",
    )
    args = parser.parse_args(argv)

    source = args.source
    if not source.exists():
        print(f"not found: {source}", file=sys.stderr)
        return 1

    output = args.output
    if output is None:
        output = source.with_name(source.stem + "_syms")

    rows = decrypt_tree(source, output)
    print(f"output: {output}")
    for rel, raw_len, plain_len, head in rows:
        kind = "bssym1" if raw_len != plain_len else "plain"
        print(f"  [{kind}] {rel}")
        print(f"    {raw_len} -> {plain_len} bytes")
        print(f"    {head}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
