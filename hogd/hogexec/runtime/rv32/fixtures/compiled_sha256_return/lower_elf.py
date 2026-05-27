#!/usr/bin/env python3
import argparse
import hashlib
import json
import struct
from pathlib import Path

K_MAX_CODE_BYTES = 64 * 1024
K_MAX_MEMORY_BYTES = 64 * 1024
ARTIFACT_MAGIC = b"GHRV32A1"
ARTIFACT_FORMAT_VERSION = 2
ARTIFACT_HEADER_BYTES = 100
ARTIFACT_VM_PROFILE_DIGEST = 0x55C53A685F0EB33D
ARTIFACT_ENTRY_ABI_VERSION = 1
ARTIFACT_SYSCALL_ABI_VERSION = 0
DISPATCH_ENTRY_BYTES = 16
GAS_TABLE_VERSION = 1
# Must match hogd/hogexec/runtime/rv32/program.h kGasScheduleDigest.
GAS_SCHEDULE_DIGEST = 0xD2F3483DC373FADA
FNV_OFFSET_BASIS = 1469598103934665603
FNV_PRIME = 1099511628211
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
STT_OBJECT = 1
STT_FUNC = 2
EM_RISCV = 243
SHN_ABS = 0xFFF1
FIXTURE_INPUT_SYMBOL = "RV32_COMPILED_SHA256_RETURN_INPUT"
SHA256_METHOD_ID = 0xCA571C66A719DD56
SHA256_REPEATED_METHOD_ID = 0x5749E8FA5D1F3CFE
SHA256_REPEATED_SYMBOL = "RV32_COMPILED_SHA256_RETURN_SHA256_REPEATED"

ALLOC_ALLOWLIST = {
    ".text",
    ".rodata",
    ".srodata",
    ".data",
    ".sdata",
    ".bss",
    ".sbss",
}


def read_c_string(blob, offset):
    end = blob.index(b"\0", offset)
    return blob[offset:end].decode("utf-8")


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sha256_bytes(blob):
    return hashlib.sha256(blob).hexdigest()


def artifact_digest_byte(digest, value):
    return ((digest ^ value) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF


def artifact_digest_u64(digest, value):
    for _ in range(8):
        digest = artifact_digest_byte(digest, value & 0xFF)
        value >>= 8
    return digest


def artifact_digest(blob):
    digest = FNV_OFFSET_BASIS
    digest = artifact_digest_u64(digest, len(blob))
    for byte in blob:
        digest = artifact_digest_byte(digest, byte)
    return digest


class ElfError(RuntimeError):
    pass


class Section:
    def __init__(self, index, name, sh_type, flags, addr, offset, size, link, entsize):
        self.index = index
        self.name = name
        self.sh_type = sh_type
        self.flags = flags
        self.addr = addr
        self.offset = offset
        self.size = size
        self.link = link
        self.entsize = entsize

    @property
    def alloc(self):
        return (self.flags & SHF_ALLOC) != 0

    @property
    def exec(self):
        return (self.flags & SHF_EXECINSTR) != 0

    @property
    def nobits(self):
        return self.sh_type == SHT_NOBITS


class Symbol:
    def __init__(self, name, value, size, sym_type, section_index):
        self.name = name
        self.value = value
        self.size = size
        self.sym_type = sym_type
        self.section_index = section_index


def parse_elf(path):
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
    shstr_blob = data[shstr[4] : shstr[4] + shstr[5]]
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


def parse_symbols(data, sections):
    symbols = []
    for section in sections:
        if section.sh_type != SHT_SYMTAB:
            continue
        if section.link >= len(sections):
            raise ElfError("symbol table has invalid string table link")
        strings = sections[section.link]
        if strings.sh_type != SHT_STRTAB:
            raise ElfError("symbol table link must point to a string table")
        string_blob = data[strings.offset : strings.offset + strings.size]
        if section.entsize != 16:
            raise ElfError("unexpected ELF32 symbol size")
        for off in range(section.offset, section.offset + section.size, section.entsize):
            st_name, st_value, st_size, st_info, _st_other, st_shndx = struct.unpack_from(
                "<IIIBBH", data, off
            )
            name = read_c_string(string_blob, st_name) if st_name != 0 else ""
            symbols.append(Symbol(name, st_value, st_size, st_info & 0x0F, st_shndx))
    return symbols


def section_for_symbol(symbol, sections):
    if symbol.section_index >= len(sections):
        return None
    return sections[symbol.section_index]


def symbol_by_name(symbols, name):
    for symbol in symbols:
        if symbol.name == name:
            return symbol
    return None


def section_span(section):
    return (section.addr, section.addr + section.size)


def spans_overlap(a_start, a_end, b_start, b_end):
    return a_start < b_end and b_start < a_end


def validate_sections(data, sections, symbols):
    text_sections = [section for section in sections if section.name == ".text"]
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
        if symbol_section.index == text.index and (
            symbol.sym_type == STT_OBJECT or symbol.name.startswith("$d")
        ):
            raise ElfError(f"data symbol {symbol.name} is not allowed in .text")
        if symbol.sym_type == STT_OBJECT and text.addr <= symbol.value < text_end:
            raise ElfError(f"object symbol {symbol.name} resolves into .text")

    memory = bytearray(K_MAX_MEMORY_BYTES)
    data_ranges = []
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
                f"data section {section.name} overlaps .text range ending at {text_end}"
            )
        for other_name, other_start, other_end in data_ranges:
            if spans_overlap(section.addr, section.addr + section.size, other_start, other_end):
                raise ElfError(f"section {section.name} overlaps {other_name}")
        data_ranges.append((section.name, section.addr, section.addr + section.size))
        if section.addr + section.size > K_MAX_MEMORY_BYTES:
            raise ElfError(f"section {section.name} exceeds memory limit")
        if section.nobits:
            continue
        if section.offset + section.size > len(data):
            raise ElfError(f"section {section.name} extends past end of file")
        memory[section.addr : section.addr + section.size] = data[
            section.offset : section.offset + section.size
        ]

    return data[text.offset : text.offset + text.size], bytes(memory)


def extract_fixture_input(memory, sections, symbols):
    symbol = symbol_by_name(symbols, FIXTURE_INPUT_SYMBOL)
    if symbol is None:
        raise ElfError(f"{FIXTURE_INPUT_SYMBOL} symbol is missing")
    if symbol.sym_type != STT_OBJECT:
        raise ElfError(f"{FIXTURE_INPUT_SYMBOL} must be an object symbol")
    section = section_for_symbol(symbol, sections)
    if section is None or not section.alloc or section.nobits or section.exec:
        raise ElfError(f"{FIXTURE_INPUT_SYMBOL} must resolve to data memory")
    if symbol.size == 0:
        raise ElfError(f"{FIXTURE_INPUT_SYMBOL} must have nonzero size")
    if symbol.value + symbol.size > len(memory):
        raise ElfError(f"{FIXTURE_INPUT_SYMBOL} exceeds memory limit")
    return symbol.value, memory[symbol.value : symbol.value + symbol.size]


def method_pc(symbols, name):
    symbol = symbol_by_name(symbols, name)
    if symbol is None:
        raise ElfError(f"{name} symbol is missing")
    if symbol.sym_type != STT_FUNC:
        raise ElfError(f"{name} must be a function symbol")
    if symbol.value % 4 != 0:
        raise ElfError(f"{name} must be four-byte aligned")
    return symbol.value


def dispatch_entries(symbols):
    entries = [
        (SHA256_REPEATED_METHOD_ID, method_pc(symbols, SHA256_REPEATED_SYMBOL)),
        (SHA256_METHOD_ID, method_pc(symbols, "_start")),
    ]
    entries.sort()
    return entries


def build_dispatch_table(entries, code):
    dispatch_table = b"".join(
        struct.pack("<QII", method_id, pc, 0) for method_id, pc in entries
    )
    if len(dispatch_table) != len(entries) * DISPATCH_ENTRY_BYTES:
        raise ElfError("internal dispatch table size mismatch")
    for _method_id, pc in entries:
        if pc >= len(code):
            raise ElfError("dispatch entry points outside .text")
    return dispatch_table


def build_artifact(code, memory, entries):
    code_offset = ARTIFACT_HEADER_BYTES
    dispatch_table = build_dispatch_table(entries, code)
    dispatch_offset = code_offset + len(code)
    memory_offset = dispatch_offset + len(dispatch_table)
    header = struct.pack(
        "<8sIIQIIIIQIIIIIIIQQQ",
        ARTIFACT_MAGIC,
        ARTIFACT_FORMAT_VERSION,
        ARTIFACT_HEADER_BYTES,
        ARTIFACT_VM_PROFILE_DIGEST,
        0,
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


def format_array(name, blob):
    out = [f"inline constexpr std::array<uint8_t, {len(blob)}> {name} = {{"]
    for i in range(0, len(blob), 12):
        chunk = ", ".join(f"0x{byte:02x}" for byte in blob[i : i + 12])
        out.append(f"    {chunk},")
    out.append("};")
    return "\n".join(out)


def write_header(path, artifact, metadata):
    body = f"""#pragma once

#include <array>
#include <cstdint>

namespace hogd::rv32::fixtures::compiled_sha256_return {{

inline constexpr uint32_t kMemoryBytes = {K_MAX_MEMORY_BYTES};
inline constexpr uint32_t kCodeBytes = {metadata['code_bytes']};
inline constexpr uint32_t kArtifactBytes = {len(artifact)};
inline constexpr uint64_t kSha256MethodId = 0x{SHA256_METHOD_ID:016x}ULL;
inline constexpr uint64_t kSha256RepeatedMethodId = 0x{SHA256_REPEATED_METHOD_ID:016x}ULL;
inline constexpr uint32_t kSha256Pc = {metadata['sha256_pc']};
inline constexpr uint32_t kSha256RepeatedPc = {metadata['sha256_repeated_pc']};
inline constexpr uint32_t kFixtureInputOffset = {metadata['fixture_input_offset']};
inline constexpr char kFixtureSha256[] = "{metadata['fixture_sha256']}";
inline constexpr char kElfSha256[] = "{metadata['elf_sha256']}";

{format_array("kInput", metadata["fixture_input"])}

{format_array("kArtifact", artifact)}

}} // namespace hogd::rv32::fixtures::compiled_sha256_return
"""
    Path(path).write_text(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True)
    parser.add_argument("--header", required=True)
    parser.add_argument("--metadata", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--linker-script", required=True)
    parser.add_argument("--lowering-tool", required=True)
    parser.add_argument("--rustc-version", required=True)
    parser.add_argument("--target-triple", required=True)
    parser.add_argument("--build-flags", required=True)
    parser.add_argument("--metadata-input", action="append", default=[])
    args = parser.parse_args()

    data, sections = parse_elf(args.elf)
    symbols = parse_symbols(data, sections)
    code, memory = validate_sections(data, sections, symbols)
    entries = dispatch_entries(symbols)
    fixture_input_offset, fixture_input = extract_fixture_input(memory, sections, symbols)
    artifact = build_artifact(code, memory, entries)
    fixture_blob = code + memory
    metadata = {
        "source_sha256": sha256_file(args.source),
        "linker_script_sha256": sha256_file(args.linker_script),
        "lowering_tool_sha256": sha256_file(args.lowering_tool),
        "metadata_input_sha256": {
            str(Path(path)): sha256_file(path) for path in args.metadata_input
        },
        "elf_sha256": sha256_file(args.elf),
        "fixture_sha256": sha256_bytes(fixture_blob),
        "artifact_sha256": sha256_bytes(artifact),
        "dispatch_entries": [
            {"method_id": f"0x{method_id:016x}", "pc": pc} for method_id, pc in entries
        ],
        "sha256_pc": dict(entries)[SHA256_METHOD_ID],
        "sha256_repeated_pc": dict(entries)[SHA256_REPEATED_METHOD_ID],
        "artifact_bytes": len(artifact),
        "fixture_input": fixture_input,
        "fixture_input_offset": fixture_input_offset,
        "fixture_input_sha256": sha256_bytes(fixture_input),
        "code_sha256": sha256_bytes(code),
        "initial_memory_sha256": sha256_bytes(memory),
        "code_bytes": len(code),
        "memory_bytes": len(memory),
        "rustc_version": args.rustc_version,
        "target_triple": args.target_triple,
        "build_flags": args.build_flags,
    }
    write_header(args.header, artifact, metadata)
    serializable = dict(metadata)
    serializable["fixture_input"] = fixture_input.hex()
    Path(args.metadata).write_text(json.dumps(serializable, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    try:
        main()
    except ElfError as exc:
        raise SystemExit(f"lower_elf.py: {exc}") from exc
