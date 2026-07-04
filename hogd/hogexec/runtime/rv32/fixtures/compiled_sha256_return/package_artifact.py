#!/usr/bin/env python3
import argparse
import hashlib
import json
import struct
from pathlib import Path

K_MAX_MEMORY_BYTES = 64 * 1024
ARTIFACT_MAGIC = b"GHRV32A1"
# rv32-lower 0.5.11 emits format v4 / entry ABI 2; package_artifact.py
# validates that, then upgrades to the v5 format hogd loads (temporary until
# a v5-emitting rv32-lower ships).
LOWERER_ARTIFACT_FORMAT_VERSION = 4
LOWERER_ARTIFACT_HEADER_BYTES = 132
LOWERER_ARTIFACT_ENTRY_ABI_VERSION = 2
ARTIFACT_FORMAT_VERSION = 5
ARTIFACT_HEADER_BYTES = 128
ARTIFACT_VM_PROFILE_DIGEST = 0x55C53A685F0EB33D
ARTIFACT_FLAGS_REQUIRED_VALUE = 0
ARTIFACT_ENTRY_ABI_VERSION = 3
ARTIFACT_SYSCALL_ABI_VERSION = 1
GAS_TABLE_VERSION = 3
GAS_SCHEDULE_DIGEST = 0x918317C21C46BB60
SHT_SYMTAB = 2
SHT_STRTAB = 3
STT_OBJECT = 1
EM_RISCV = 243
FIXTURE_INPUT_SYMBOL = "RV32_COMPILED_SHA256_RETURN_INPUT"
# v4 dispatches in-guest: _start reads the seeded method id from a0/a1 and
# routes to the sha256 body (SHA256_METHOD_ID) or the sha256_repeated body
# (SHA256_REPEATED_METHOD_ID), trapping on any other id. These MethodIDs are
# emitted to the fixture header and the C++ parity test static_asserts them
# against the generated Sha256Wrapper WASM contract metadata.
SHA256_METHOD_ID = 0xE8CCFC8FFC9E81B9
SHA256_REPEATED_METHOD_ID = 0x2F2CB89270E38729


class FixtureError(RuntimeError):
    pass


class Section:
    def __init__(
        self, index, name, sh_type, flags, addr, offset, size, link, entsize
    ):
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
        return (self.flags & 0x2) != 0

    @property
    def exec(self):
        return (self.flags & 0x4) != 0

    @property
    def nobits(self):
        return self.sh_type == 8


class Symbol:
    def __init__(self, name, value, size, sym_type, section_index):
        self.name = name
        self.value = value
        self.size = size
        self.sym_type = sym_type
        self.section_index = section_index


def read_c_string(blob, offset):
    end = blob.index(b"\0", offset)
    return blob[offset:end].decode("utf-8")


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sha256_bytes(blob):
    return hashlib.sha256(blob).hexdigest()


def parse_elf(path):
    data = Path(path).read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise FixtureError("input is not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise FixtureError("expected little-endian ELF32")

    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    e_type = header[1]
    e_machine = header[2]
    e_shoff = header[6]
    e_shentsize = header[11]
    e_shnum = header[12]
    e_shstrndx = header[13]
    if e_machine != EM_RISCV:
        raise FixtureError(f"expected RISC-V ELF, got e_machine={e_machine}")
    if e_type != 2:
        raise FixtureError(f"expected executable ELF, got e_type={e_type}")
    if e_shentsize != 40:
        raise FixtureError(f"unexpected section header size {e_shentsize}")
    if e_shoff + e_shentsize * e_shnum > len(data):
        raise FixtureError("section headers extend past end of file")
    if e_shstrndx >= e_shnum:
        raise FixtureError("invalid section string table index")

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


def parse_symbols(data, sections):
    symbols = []
    for section in sections:
        if section.sh_type != SHT_SYMTAB:
            continue
        if section.link >= len(sections):
            raise FixtureError("symbol table has invalid string table link")
        strings = sections[section.link]
        if strings.sh_type != SHT_STRTAB:
            raise FixtureError(
                "symbol table link must point to a string table"
            )
        string_blob = data[strings.offset:strings.offset + strings.size]
        if section.entsize != 16:
            raise FixtureError("unexpected ELF32 symbol size")
        for off in range(
            section.offset, section.offset + section.size, section.entsize
        ):
            raw_symbol = struct.unpack_from("<IIIBBH", data, off)
            st_name, st_value, st_size, st_info = raw_symbol[:4]
            st_shndx = raw_symbol[5]
            name = read_c_string(string_blob, st_name) if st_name != 0 else ""
            symbols.append(
                Symbol(name, st_value, st_size, st_info & 0x0F, st_shndx)
            )
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


def parse_artifact(path):
    artifact = Path(path).read_bytes()
    if len(artifact) < LOWERER_ARTIFACT_HEADER_BYTES:
        raise FixtureError("artifact is shorter than the header")
    header = struct.unpack_from("<8sIIQIIIIQIIIII32s32s", artifact, 0)
    magic = header[0]
    format_version = header[1]
    header_bytes = header[2]
    vm_profile_digest = header[3]
    flags = header[4]
    entry_abi_version = header[5]
    syscall_abi_version = header[6]
    gas_table_version = header[7]
    gas_schedule_digest = header[8]
    memory_bytes = header[9]
    code_bytes = header[10]
    initial_memory_bytes = header[11]
    code_offset = header[12]
    initial_memory_offset = header[13]
    code_digest = header[14]
    initial_memory_digest = header[15]
    if magic != ARTIFACT_MAGIC:
        raise FixtureError("artifact has wrong magic")
    if format_version != LOWERER_ARTIFACT_FORMAT_VERSION:
        raise FixtureError(
            f"unexpected artifact format version {format_version}"
        )
    if header_bytes != LOWERER_ARTIFACT_HEADER_BYTES:
        raise FixtureError(f"unexpected artifact header size {header_bytes}")
    if vm_profile_digest != ARTIFACT_VM_PROFILE_DIGEST:
        raise FixtureError(
            f"unexpected VM profile digest 0x{vm_profile_digest:016x}"
        )
    if flags != ARTIFACT_FLAGS_REQUIRED_VALUE:
        raise FixtureError(f"unexpected artifact flags {flags}")
    if entry_abi_version != LOWERER_ARTIFACT_ENTRY_ABI_VERSION:
        raise FixtureError(f"unexpected entry ABI version {entry_abi_version}")
    if syscall_abi_version != ARTIFACT_SYSCALL_ABI_VERSION:
        raise FixtureError(
            f"unexpected syscall ABI version {syscall_abi_version}"
        )
    if gas_table_version != GAS_TABLE_VERSION:
        raise FixtureError(f"unexpected gas table version {gas_table_version}")
    if gas_schedule_digest != GAS_SCHEDULE_DIGEST:
        raise FixtureError(
            f"unexpected gas schedule digest 0x{gas_schedule_digest:016x}"
        )
    if memory_bytes != K_MAX_MEMORY_BYTES:
        raise FixtureError(f"unexpected memory size {memory_bytes}")
    if code_offset + code_bytes > len(artifact):
        raise FixtureError("code section extends past artifact end")
    if initial_memory_offset + initial_memory_bytes > len(artifact):
        raise FixtureError("initial memory section extends past artifact end")
    if initial_memory_offset != code_offset + code_bytes:
        raise FixtureError("artifact sections are not contiguous")
    if initial_memory_offset + initial_memory_bytes != len(artifact):
        raise FixtureError("artifact has trailing bytes")
    code = artifact[code_offset:code_offset + code_bytes]
    memory = artifact[
        initial_memory_offset:initial_memory_offset + initial_memory_bytes
    ]
    if code_digest != hashlib.sha256(code).digest():
        raise FixtureError("code section digest mismatch")
    if initial_memory_digest != hashlib.sha256(memory).digest():
        raise FixtureError("initial memory section digest mismatch")
    return artifact, code, memory, memory_bytes


def upgrade_artifact_to_v5(code, memory, memory_bytes):
    """Rewrite the validated v4 sections as a v5 artifact.

    v5 stores the image at data-space VA initial_memory_start instead of 0;
    trim the leading zeros (the fixture's code-sized data shadow) into the
    start offset. Deterministic: trim count is a pure function of the bytes.
    """
    start = 0
    while start < len(memory) and memory[start] == 0:
        start += 1
    trimmed = memory[start:]
    if not trimmed:
        start = 0
    header = struct.pack(
        "<8sIIQIIIIQIIII32s32s",
        ARTIFACT_MAGIC,
        ARTIFACT_FORMAT_VERSION,
        ARTIFACT_HEADER_BYTES,
        ARTIFACT_VM_PROFILE_DIGEST,
        ARTIFACT_FLAGS_REQUIRED_VALUE,
        ARTIFACT_ENTRY_ABI_VERSION,
        ARTIFACT_SYSCALL_ABI_VERSION,
        GAS_TABLE_VERSION,
        GAS_SCHEDULE_DIGEST,
        memory_bytes,
        len(code),
        len(trimmed),
        start,
        hashlib.sha256(code).digest(),
        hashlib.sha256(trimmed).digest(),
    )
    return header + code + trimmed, trimmed, start


def extract_fixture_input(memory, sections, symbols):
    symbol = symbol_by_name(symbols, FIXTURE_INPUT_SYMBOL)
    if symbol is None:
        raise FixtureError(f"{FIXTURE_INPUT_SYMBOL} symbol is missing")
    if symbol.sym_type != STT_OBJECT:
        raise FixtureError(f"{FIXTURE_INPUT_SYMBOL} must be an object symbol")
    section = section_for_symbol(symbol, sections)
    if section is None or not section.alloc or section.nobits or section.exec:
        raise FixtureError(
            f"{FIXTURE_INPUT_SYMBOL} must resolve to data memory"
        )
    if symbol.size == 0:
        raise FixtureError(f"{FIXTURE_INPUT_SYMBOL} must have nonzero size")
    if symbol.value + symbol.size > len(memory):
        raise FixtureError(
            f"{FIXTURE_INPUT_SYMBOL} exceeds initialized memory"
        )
    return symbol.value, memory[symbol.value:symbol.value + symbol.size]


def format_array(name, blob):
    out = [
        f"inline constexpr std::array<uint8_t, {len(blob)}> {name} = {{"
    ]
    for i in range(0, len(blob), 12):
        chunk = ", ".join(f"0x{byte:02x}" for byte in blob[i:i + 12])
        out.append(f"    {chunk},")
    out.append("};")
    return "\n".join(out)


def write_header(path, artifact, metadata):
    initial_memory_bytes = metadata["initial_memory_bytes"]
    sha256_repeated_id = f"0x{SHA256_REPEATED_METHOD_ID:016x}ULL"
    fixture_input_offset = metadata["fixture_input_offset"]
    body = f"""#pragma once

#include <array>
#include <cstdint>

namespace hogd::rv32::fixtures::compiled_sha256_return {{

inline constexpr uint32_t kMemoryBytes = {metadata['memory_bytes']};
inline constexpr uint32_t kInitialMemoryBytes = {initial_memory_bytes};
inline constexpr uint32_t kInitialMemoryStart = {metadata['initial_memory_start']};
inline constexpr uint32_t kCodeBytes = {metadata['code_bytes']};
inline constexpr uint32_t kArtifactBytes = {len(artifact)};
inline constexpr uint64_t kSha256MethodId = 0x{SHA256_METHOD_ID:016x}ULL;
inline constexpr uint64_t kSha256RepeatedMethodId = {sha256_repeated_id};
inline constexpr uint32_t kFixtureInputOffset = {fixture_input_offset};
inline constexpr char kFixtureSha256[] =
    "{metadata['fixture_sha256']}";
inline constexpr char kElfSha256[] =
    "{metadata['elf_sha256']}";

{format_array('kInput', metadata['fixture_input'])}

{format_array('kArtifact', artifact)}

}} // namespace hogd::rv32::fixtures::compiled_sha256_return
"""
    Path(path).write_text(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True)
    parser.add_argument("--artifact", required=True)
    parser.add_argument("--lowerer-metadata", required=True)
    parser.add_argument("--header", required=True)
    parser.add_argument("--metadata", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--linker-script", required=True)
    parser.add_argument("--packaging-tool", required=True)
    parser.add_argument("--lowering-tool", required=True)
    parser.add_argument("--lowering-tool-version", required=True)
    parser.add_argument("--rustc-version", required=True)
    parser.add_argument("--target-triple", required=True)
    parser.add_argument("--build-flags", required=True)
    parser.add_argument("--metadata-input", action="append", default=[])
    args = parser.parse_args()

    data, sections = parse_elf(args.elf)
    symbols = parse_symbols(data, sections)
    _, code, memory, memory_bytes = parse_artifact(args.artifact)
    fixture_input_offset, fixture_input = extract_fixture_input(
        memory, sections, symbols
    )
    artifact, stored_memory, initial_memory_start = upgrade_artifact_to_v5(
        code, memory, memory_bytes
    )
    lowerer_metadata = json.loads(Path(args.lowerer_metadata).read_text())
    fixture_blob = code + stored_memory
    metadata = {
        "source_sha256": sha256_file(args.source),
        "linker_script_sha256": sha256_file(args.linker_script),
        "packaging_tool_sha256": sha256_file(args.packaging_tool),
        "lowering_tool": args.lowering_tool,
        "lowering_tool_version": args.lowering_tool_version,
        "lowerer_metadata": lowerer_metadata,
        "metadata_input_sha256": {
            str(Path(path)): sha256_file(path) for path in args.metadata_input
        },
        "elf_sha256": sha256_file(args.elf),
        "fixture_sha256": sha256_bytes(fixture_blob),
        "artifact_sha256": sha256_bytes(artifact),
        "method_ids": [
            f"0x{SHA256_METHOD_ID:016x}",
            f"0x{SHA256_REPEATED_METHOD_ID:016x}",
        ],
        "artifact_bytes": len(artifact),
        "fixture_input": fixture_input,
        "fixture_input_offset": fixture_input_offset,
        "fixture_input_sha256": sha256_bytes(fixture_input),
        "code_sha256": sha256_bytes(code),
        "initial_memory_sha256": sha256_bytes(stored_memory),
        "code_bytes": len(code),
        "memory_bytes": memory_bytes,
        "initial_memory_bytes": len(stored_memory),
        "initial_memory_start": initial_memory_start,
        "rustc_version": args.rustc_version,
        "target_triple": args.target_triple,
        "build_flags": args.build_flags,
    }
    write_header(args.header, artifact, metadata)
    serializable = dict(metadata)
    serializable["fixture_input"] = fixture_input.hex()
    Path(args.metadata).write_text(
        json.dumps(serializable, indent=2, sort_keys=True) + "\n"
    )


if __name__ == "__main__":
    try:
        main()
    except FixtureError as exc:
        raise SystemExit(f"package_artifact.py: {exc}") from exc
