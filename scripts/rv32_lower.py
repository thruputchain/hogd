#!/usr/bin/env python3
"""
rv32_lower.py — Produce a Groundhog RV32 v3 artifact from a contract ELF.

Reads a freestanding `riscv32im-unknown-none-elf` binary (produced by the
SDK's `<contract>-rv32` bin crate), validates it against the rules
documented in `docs/rv32/rv32-vm-experimental-design.md`, scans the
symbol table for `#[contract]`-macro-emitted entry points, and writes a
binary v3 artifact plus a JSON metadata sidecar.

This is the production successor to
`hogd/hogexec/runtime/rv32/fixtures/compiled_sha256_return/lower_elf.py`,
generalized for SDK-built contracts:

  - Dispatch entries are discovered from the symbol table by matching
    on the 16-uppercase-hex ContractEntrypointID names the SDK's
    `#[contract]` macro emits via `#[export_name = ...]` on rv32.
  - No fixture-specific symbols are special-cased.
  - Output is the binary artifact (.rv32a), not a C++ header.

The artifact format constants here MUST match
`hogd/hogexec/runtime/rv32/artifact.h`. The build script for the runtime
should cross-check `kArtifactVmProfileDigest` and `GAS_SCHEDULE_DIGEST`
against the C++ headers.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# Artifact format constants. Mirror hogd/hogexec/runtime/rv32/artifact.h.
# ---------------------------------------------------------------------------

K_MAX_CODE_BYTES = 64 * 1024
K_MAX_MEMORY_BYTES = 64 * 1024
ARTIFACT_MAGIC = b"GHRV32A1"
ARTIFACT_FORMAT_VERSION = 3
ARTIFACT_HEADER_BYTES = 172
ARTIFACT_VM_PROFILE_DIGEST = 0x55C53A685F0EB33D
ARTIFACT_ENTRY_ABI_VERSION = 1
ARTIFACT_SYSCALL_ABI_VERSION = 0
DISPATCH_ENTRY_BYTES = 16
GAS_TABLE_VERSION = 1
# Must match hogd/hogexec/runtime/rv32/program.h kGasScheduleDigest.
GAS_SCHEDULE_DIGEST = 0xEC026096ED9849F7

# ---------------------------------------------------------------------------
# ELF constants.
# ---------------------------------------------------------------------------

SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
STT_OBJECT = 1
STT_FUNC = 2
STB_GLOBAL = 1
STB_WEAK = 2
EM_RISCV = 243

ALLOC_ALLOWLIST = {
    ".text",
    ".rodata",
    ".srodata",
    ".data",
    ".sdata",
    ".bss",
    ".sbss",
}

# Matches the macro's `entrypoint_export_name`:
# `format!("{:016X}", u64::from_be_bytes(*id))`.
ENTRYPOINT_SYMBOL_RE = re.compile(r"^[0-9A-F]{16}$")


class ElfError(RuntimeError):
    pass


@dataclass
class Section:
    index: int
    name: str
    sh_type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    entsize: int

    @property
    def alloc(self) -> bool:
        return (self.flags & SHF_ALLOC) != 0

    @property
    def exec(self) -> bool:
        return (self.flags & SHF_EXECINSTR) != 0

    @property
    def nobits(self) -> bool:
        return self.sh_type == SHT_NOBITS


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    sym_type: int
    bind: int
    section_index: int


def read_c_string(blob: bytes, offset: int) -> str:
    end = blob.index(b"\0", offset)
    return blob[offset:end].decode("utf-8")


def sha256_file(path: str | Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sha256_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def artifact_digest(blob: bytes) -> bytes:
    return hashlib.sha256(blob).digest()


# ---------------------------------------------------------------------------
# ELF parsing.
# ---------------------------------------------------------------------------


def parse_elf(path: str | Path) -> tuple[bytes, list[Section]]:
    data = Path(path).read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ElfError("input is not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise ElfError("expected little-endian ELF32")
    if data[6] != 1:
        raise ElfError("unsupported ELF version")

    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    e_type = header[1]
    e_machine = header[2]
    e_entry = header[4]
    e_shoff = header[6]
    e_shentsize = header[11]
    e_shnum = header[12]
    e_shstrndx = header[13]
    if e_machine != EM_RISCV:
        raise ElfError(f"expected RISC-V ELF, got e_machine={e_machine}")
    if e_type != 2:
        raise ElfError(f"expected executable ELF, got e_type={e_type}")
    if e_entry != 0:
        raise ElfError(f"expected entry point 0, got {e_entry}")
    if e_shentsize != 40:
        raise ElfError(f"unexpected section header size {e_shentsize}")
    if e_shoff + e_shentsize * e_shnum > len(data):
        raise ElfError("section headers extend past end of file")
    if e_shstrndx >= e_shnum:
        raise ElfError("invalid section string table index")

    raw_sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        raw_sections.append(struct.unpack_from("<IIIIIIIIII", data, off))

    shstr = raw_sections[e_shstrndx]
    shstr_blob = data[shstr[4]:shstr[4] + shstr[5]]
    sections = []
    for index, raw in enumerate(raw_sections):
        name = read_c_string(shstr_blob, raw[0]) if raw[0] else ""
        sections.append(
            Section(
                index=index,
                name=name,
                sh_type=raw[1],
                flags=raw[2],
                addr=raw[3],
                offset=raw[4],
                size=raw[5],
                link=raw[6],
                entsize=raw[9],
            )
        )

    return data, sections


def parse_symbols(data: bytes, sections: list[Section]) -> list[Symbol]:
    symbols: list[Symbol] = []
    for section in sections:
        if section.sh_type != SHT_SYMTAB:
            continue
        if section.link >= len(sections):
            raise ElfError("symbol table has invalid string table link")
        strings = sections[section.link]
        if strings.sh_type != SHT_STRTAB:
            raise ElfError("symbol table link must point to a string table")
        string_blob = data[strings.offset:strings.offset + strings.size]
        if section.entsize != 16:
            raise ElfError("unexpected ELF32 symbol size")
        for off in range(
            section.offset, section.offset + section.size, section.entsize
        ):
            (
                st_name,
                st_value,
                st_size,
                st_info,
                _st_other,
                st_shndx,
            ) = struct.unpack_from("<IIIBBH", data, off)
            name = read_c_string(string_blob, st_name) if st_name != 0 else ""
            symbols.append(
                Symbol(
                    name=name,
                    value=st_value,
                    size=st_size,
                    sym_type=st_info & 0x0F,
                    bind=(st_info >> 4) & 0x0F,
                    section_index=st_shndx,
                )
            )
    return symbols


def section_for_symbol(
    symbol: Symbol, sections: list[Section]
) -> Section | None:
    if symbol.section_index >= len(sections):
        return None
    return sections[symbol.section_index]


def symbol_by_name(symbols: list[Symbol], name: str) -> Symbol | None:
    for symbol in symbols:
        if symbol.name == name:
            return symbol
    return None


# ---------------------------------------------------------------------------
# Validation. Rules from docs/rv32/rv32-vm-experimental-design.md.
# ---------------------------------------------------------------------------


def spans_overlap(
    a_start: int, a_end: int, b_start: int, b_end: int
) -> bool:
    return a_start < b_end and b_start < a_end


def validate_sections(
    data: bytes, sections: list[Section], symbols: list[Symbol]
) -> tuple[bytes, bytes]:
    text_sections = [s for s in sections if s.name == ".text"]
    if len(text_sections) != 1:
        raise ElfError("expected exactly one .text section")
    text = text_sections[0]
    if not text.exec or not text.alloc:
        raise ElfError(".text must be alloc+exec")
    if text.addr != 0:
        raise ElfError(f".text must start at vaddr 0, got {text.addr}")
    if text.size == 0:
        raise ElfError(".text is empty")
    if text.size % 4 != 0:
        raise ElfError(".text size must be four-byte aligned")
    if text.size > K_MAX_CODE_BYTES:
        raise ElfError(f".text exceeds {K_MAX_CODE_BYTES} bytes")
    if text.offset + text.size > len(data):
        raise ElfError(".text extends past end of file")

    start = symbol_by_name(symbols, "_start")
    if start is None:
        raise ElfError("_start symbol is missing")
    if start.value != 0:
        raise ElfError(f"_start must be at vaddr 0, got {start.value}")
    if start.sym_type != STT_FUNC:
        raise ElfError("_start must be a function symbol")

    text_end = text.addr + text.size
    for symbol in symbols:
        symbol_section = section_for_symbol(symbol, sections)
        if symbol_section is None:
            continue
        if (
            symbol_section.index == text.index
            and (
                symbol.sym_type == STT_OBJECT
                or symbol.name.startswith("$d")
            )
        ):
            raise ElfError(
                f"data symbol {symbol.name!r} is not allowed in .text"
            )
        if (
            symbol.sym_type == STT_OBJECT
            and text.addr <= symbol.value < text_end
        ):
            raise ElfError(
                f"object symbol {symbol.name!r} resolves into .text"
            )

    memory = bytearray(K_MAX_MEMORY_BYTES)
    data_ranges: list[tuple[str, int, int]] = []
    for section in sections:
        if section.sh_type in (SHT_REL, SHT_RELA) and section.size != 0:
            raise ElfError(f"relocations are not allowed: {section.name}")
        if section.exec and section.name != ".text":
            raise ElfError(f"unexpected executable section: {section.name}")
        if not section.alloc:
            continue
        if section.name not in ALLOC_ALLOWLIST:
            raise ElfError(f"unexpected alloc section: {section.name}")
        if section.name == ".text":
            continue
        if section.addr < text_end:
            raise ElfError(
                f"data section {section.name} overlaps .text range ending at "
                f"{text_end}"
            )
        for other_name, other_start, other_end in data_ranges:
            if spans_overlap(
                section.addr,
                section.addr + section.size,
                other_start,
                other_end,
            ):
                raise ElfError(
                    f"section {section.name} overlaps {other_name}"
                )
        data_ranges.append(
            (section.name, section.addr, section.addr + section.size)
        )
        if section.addr + section.size > K_MAX_MEMORY_BYTES:
            raise ElfError(f"section {section.name} exceeds memory limit")
        if section.nobits:
            continue
        if section.offset + section.size > len(data):
            raise ElfError(
                f"section {section.name} extends past end of file"
            )
        memory[section.addr:section.addr + section.size] = data[
            section.offset:section.offset + section.size
        ]

    return data[text.offset:text.offset + text.size], bytes(memory)


# ---------------------------------------------------------------------------
# Dispatch discovery.
# ---------------------------------------------------------------------------


def discover_dispatch_entries(
    sections: list[Section],
    symbols: list[Symbol],
    code_size: int,
) -> list[tuple[int, int]]:
    """
    Scan the symbol table for `#[contract]`-macro entry points.

    The SDK macro emits each method as
    `#[export_name = "<16-UPPER-HEX>"]` (the ContractEntrypointID).
    We accept globally-bound function symbols matching that shape that
    *also* resolve to the `.text` section — not any low-valued absolute
    or non-text symbol that happens to match the hex regex.

    Returns: sorted list of (method_id, pc) tuples.
    """
    text_sections = [s for s in sections if s.name == ".text"]
    if len(text_sections) != 1:
        raise ElfError("expected exactly one .text section")
    text_index = text_sections[0].index

    entries: dict[int, int] = {}
    for symbol in symbols:
        if not ENTRYPOINT_SYMBOL_RE.match(symbol.name):
            continue
        if symbol.sym_type != STT_FUNC:
            raise ElfError(
                f"entry-point-shaped symbol {symbol.name!r} is not a function"
            )
        if symbol.bind not in (STB_GLOBAL, STB_WEAK):
            # Local symbols matching the hex shape would be a name collision
            # rather than an intended export. Skip them silently.
            continue
        if symbol.section_index != text_index:
            # Reject absolute symbols (SHN_ABS), undefined symbols, and
            # symbols in any other section. A dispatch entry must point
            # into executable code.
            raise ElfError(
                f"entry point {symbol.name!r} does not resolve to .text "
                f"(section_index={symbol.section_index})"
            )
        if symbol.value % 4 != 0:
            raise ElfError(
                f"entry point {symbol.name!r} is not four-byte aligned "
                f"(value={symbol.value})"
            )
        if symbol.value >= code_size:
            raise ElfError(
                f"entry point {symbol.name!r} pc={symbol.value} "
                f"falls outside .text (code_size={code_size})"
            )
        if symbol.size != 0 and symbol.value + symbol.size > code_size:
            raise ElfError(
                f"entry point {symbol.name!r} extends past .text "
                f"(pc={symbol.value}, size={symbol.size}, "
                f"code_size={code_size})"
            )
        method_id = int(symbol.name, 16)
        if method_id in entries and entries[method_id] != symbol.value:
            raise ElfError(
                f"duplicate dispatch entry for {symbol.name!r} "
                f"({entries[method_id]} vs {symbol.value})"
            )
        entries[method_id] = symbol.value

    if not entries:
        raise ElfError(
            "no #[contract] entry-point symbols found in ELF symbol table"
        )
    return sorted(entries.items())


def build_dispatch_table(
    entries: list[tuple[int, int]], code_size: int
) -> bytes:
    """
    Pack entries into the v3 dispatch table layout:
        struct DispatchEntry { uint64_t method_id; uint32_t pc; uint32_t _reserved; }
    Sorted by method_id. The C++ parser enforces this; we enforce it
    here so the digest commits to a canonical form.
    """
    for method_id, pc in entries:
        if pc >= code_size:
            raise ElfError(
                f"dispatch entry {method_id:016x} pc={pc} outside .text"
            )
    table = b"".join(
        struct.pack("<QII", method_id, pc, 0) for method_id, pc in entries
    )
    expected = len(entries) * DISPATCH_ENTRY_BYTES
    if len(table) != expected:
        raise ElfError("internal dispatch table size mismatch")
    return table


# ---------------------------------------------------------------------------
# Artifact assembly.
# ---------------------------------------------------------------------------


def build_artifact(
    code: bytes, memory: bytes, entries: list[tuple[int, int]]
) -> bytes:
    code_offset = ARTIFACT_HEADER_BYTES
    dispatch_table = build_dispatch_table(entries, len(code))
    dispatch_offset = code_offset + len(code)
    memory_offset = dispatch_offset + len(dispatch_table)
    header = struct.pack(
        "<8sIIQIIIIQIIIIIII32s32s32s",
        ARTIFACT_MAGIC,
        ARTIFACT_FORMAT_VERSION,
        ARTIFACT_HEADER_BYTES,
        ARTIFACT_VM_PROFILE_DIGEST,
        0,  # flags
        ARTIFACT_ENTRY_ABI_VERSION,
        ARTIFACT_SYSCALL_ABI_VERSION,
        GAS_TABLE_VERSION,
        GAS_SCHEDULE_DIGEST,
        K_MAX_MEMORY_BYTES,
        len(code),
        len(memory),
        code_offset,
        memory_offset,
        dispatch_offset,
        len(dispatch_table),
        artifact_digest(code),
        artifact_digest(memory),
        artifact_digest(dispatch_table),
    )
    if len(header) != ARTIFACT_HEADER_BYTES:
        raise ElfError("internal artifact header size mismatch")
    return header + code + dispatch_table + memory


# ---------------------------------------------------------------------------
# CLI.
# ---------------------------------------------------------------------------


def verify_artifact(artifact: bytes) -> None:
    """
    Self-check the freshly-written artifact: re-parse the header,
    confirm magic and digests, and confirm the dispatch table is
    sorted with zero reserved bytes. This is cheap insurance against
    a regression in `build_artifact` going undetected until the C++
    parser flags it.
    """
    if len(artifact) < ARTIFACT_HEADER_BYTES:
        raise ElfError("verify: artifact shorter than header")
    if artifact[:8] != ARTIFACT_MAGIC:
        raise ElfError("verify: bad magic")

    (
        _magic,
        fmt_ver,
        hdr_bytes,
        vm_profile,
        _flags,
        entry_abi,
        syscall_abi,
        gas_ver,
        gas_digest,
        mem_bytes,
        code_size,
        init_mem_size,
        code_off,
        mem_off,
        disp_off,
        disp_size,
        code_dig,
        mem_dig,
        disp_dig,
    ) = struct.unpack_from("<8sIIQIIIIQIIIIIII32s32s32s", artifact, 0)

    if fmt_ver != ARTIFACT_FORMAT_VERSION:
        raise ElfError(f"verify: format_version {fmt_ver}")
    if hdr_bytes != ARTIFACT_HEADER_BYTES:
        raise ElfError(f"verify: header_bytes {hdr_bytes}")
    if vm_profile != ARTIFACT_VM_PROFILE_DIGEST:
        raise ElfError("verify: vm_profile mismatch")
    if entry_abi != ARTIFACT_ENTRY_ABI_VERSION:
        raise ElfError("verify: entry_abi mismatch")
    if syscall_abi != ARTIFACT_SYSCALL_ABI_VERSION:
        raise ElfError("verify: syscall_abi mismatch")
    if gas_ver != GAS_TABLE_VERSION or gas_digest != GAS_SCHEDULE_DIGEST:
        raise ElfError("verify: gas schedule mismatch")
    if mem_bytes != K_MAX_MEMORY_BYTES:
        raise ElfError("verify: memory_bytes mismatch")

    code = artifact[code_off:code_off + code_size]
    disp = artifact[disp_off:disp_off + disp_size]
    mem = artifact[mem_off:mem_off + init_mem_size]

    if artifact_digest(code) != code_dig:
        raise ElfError("verify: code digest mismatch")
    if artifact_digest(mem) != mem_dig:
        raise ElfError("verify: initial_memory digest mismatch")
    if artifact_digest(disp) != disp_dig:
        raise ElfError("verify: dispatch digest mismatch")

    if disp_size % DISPATCH_ENTRY_BYTES != 0:
        raise ElfError("verify: dispatch size not a multiple of entry size")
    prev_id = -1
    for i in range(0, disp_size, DISPATCH_ENTRY_BYTES):
        method_id, pc, reserved = struct.unpack_from("<QII", disp, i)
        if reserved != 0:
            raise ElfError(f"verify: dispatch entry {i} reserved={reserved}")
        if method_id <= prev_id:
            raise ElfError("verify: dispatch entries not strictly sorted")
        if pc >= code_size or pc % 4 != 0:
            raise ElfError(
                f"verify: dispatch entry {method_id:016X} pc={pc} invalid"
            )
        prev_id = method_id


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--elf", required=True, help="Input rv32 ELF (from cargo build)"
    )
    parser.add_argument(
        "--out", required=True, help="Output artifact path (.rv32a)"
    )
    parser.add_argument(
        "--metadata", help="Optional JSON sidecar for build metadata"
    )
    parser.add_argument(
        "--rustc-version", help="Recorded in metadata if --metadata set"
    )
    parser.add_argument(
        "--target-triple", help="Recorded in metadata if --metadata set"
    )
    parser.add_argument(
        "--build-flags", help="Recorded in metadata if --metadata set"
    )
    args = parser.parse_args()

    data, sections = parse_elf(args.elf)
    symbols = parse_symbols(data, sections)
    code, memory = validate_sections(data, sections, symbols)
    entries = discover_dispatch_entries(sections, symbols, len(code))
    artifact = build_artifact(code, memory, entries)
    verify_artifact(artifact)

    Path(args.out).write_bytes(artifact)

    if args.metadata:
        meta = {
            "elf_sha256": sha256_file(args.elf),
            "artifact_sha256": sha256_bytes(artifact),
            "code_sha256": sha256_bytes(code),
            "initial_memory_sha256": sha256_bytes(memory),
            "code_bytes": len(code),
            "memory_bytes": len(memory),
            "artifact_bytes": len(artifact),
            "dispatch_entries": [
                {"method_id": f"0x{method_id:016X}", "pc": pc}
                for method_id, pc in entries
            ],
            "rustc_version": args.rustc_version,
            "target_triple": args.target_triple,
            "build_flags": args.build_flags,
        }
        Path(args.metadata).write_text(
            json.dumps(meta, indent=2, sort_keys=True) + "\n"
        )


if __name__ == "__main__":
    try:
        main()
    except ElfError as exc:
        raise SystemExit(f"rv32_lower.py: {exc}") from exc
