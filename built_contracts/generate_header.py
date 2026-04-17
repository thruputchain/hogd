#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import struct
import sys


def major_version(version_str):
    """Extract major version matching sdk_rs/contract-util/src/semver.rs.

    - "1.2.3" -> "1"
    - "0.9.2" -> "0.9"
    - "0.0.2" -> "0.0.2"
    """
    parts = version_str.split(".")
    major = int(parts[0]) if len(parts) > 0 else 0
    minor = int(parts[1]) if len(parts) > 1 else 0
    patch = int(parts[2].split("-")[0]) if len(parts) > 2 else 0
    if major > 0:
        return str(major)
    elif minor > 0:
        return f"{major}.{minor}"
    elif patch > 0:
        return f"{major}.{minor}.{patch}"
    else:
        return version_str


def compute_entrypoint_id(interface_name, version_str, method_name):
    """Compute entrypoint ID: sha256(interface_name || major_ver || method_name)[0:8]."""
    mv = major_version(version_str)
    h = hashlib.sha256()
    h.update(interface_name.encode())
    h.update(mv.encode())
    h.update(method_name.encode())
    return struct.unpack(">Q", h.digest()[:8])[0]


def map_type(ty):
    """Map a spec type descriptor to a C++ type string."""
    kind = ty["kind"]
    if kind == "u8":
        return "uint8_t"
    elif kind == "u32":
        return "uint32_t"
    elif kind == "i32":
        return "int32_t"
    elif kind == "u64":
        return "uint64_t"
    elif kind == "i64":
        return "int64_t"
    elif kind == "u128":
        return "std::tuple<uint64_t, uint64_t>"
    elif kind == "i128":
        return "std::tuple<int64_t, uint64_t>"
    elif kind == "bool":
        return "bool"
    elif kind == "void":
        return "void"
    elif kind == "address":
        return "ContractAddress"
    elif kind == "vec":
        inner = map_type(ty["inner"])
        max_len = ty["max_len"]
        return f"x23::Vec<{inner}, {max_len}>"
    elif kind == "array":
        inner = map_type(ty["element"])
        length = ty["len"]
        return f"std::array<{inner}, {length}>"
    elif kind == "option":
        inner = map_type(ty["inner"])
        return f"x23::Ptr<{inner}>"
    elif kind == "tuple":
        items = [map_type(item) for item in ty["items"]]
        if len(items) == 1:
            return items[0]
        return f"std::tuple<{', '.join(items)}>"
    elif kind == "named":
        return ty["name"]
    else:
        return kind


def generate_interface_struct(interface, wasm_file=None):
    """Generate a single C++ struct for one contract interface."""
    interface_name = interface["name"]
    version = interface["version"]
    methods = interface["methods"]
    throws = interface.get("throws", [])

    # Build list of ContractException<Type, Selector> for each throw
    exception_types = []
    for throw_entry in throws:
        selector = throw_entry.get("selector", "")
        err_ty = throw_entry.get("type", {})
        if err_ty.get("kind") == "named":
            name = err_ty["name"]
            exception_types.append(f"ContractException<{name}, {selector}ULL>")

    # Method names and entrypoint IDs
    export_names = [m["name"] for m in methods]
    entrypoints = [
        compute_entrypoint_id(interface_name, version, name) for name in export_names
    ]

    export_names_str = ",\n            ".join(f'"{name}"' for name in export_names)
    entrypoints_str = ",\n            ".join(f"0x{ep:016x}ULL" for ep in entrypoints)

    # Function signatures
    function_signatures = []
    for method in methods:
        return_type = map_type(method["return_type"])
        args = [map_type(arg["ty"]) for arg in method["args"]]

        if exception_types:
            exn_args = ", ".join(exception_types)
            return_type = f"Result<{return_type}, {exn_args}>"

        if args:
            sig = f"{return_type}(*)({', '.join(args)})"
        else:
            sig = f"{return_type}(*)()"
        function_signatures.append(sig)

    signatures_str = ",\n             ".join(function_signatures)

    wasm_str = (
        f'constexpr static std::string_view kWasmFile = "{wasm_file}";'
        if wasm_file
        else ""
    )

    return f"""
struct {interface_name} {{
    constexpr static std::string_view kTraitName = "{interface_name}";
    constexpr static std::string_view kVersion = "{version}";
    {wasm_str}

    constexpr static std::string_view kSerializationFormat = "cxs";

    // These will be sorted before being passed as indices to method id derivation.
    constexpr static auto kExportNames = std::array<std::string_view, {len(export_names)}>{{
            {export_names_str}
        }};

    constexpr static auto kEntrypoints = std::array<uint64_t, {len(entrypoints)}>{{
            {entrypoints_str}
        }};

    using export_types = std::tuple<{signatures_str}>;
}};
"""


def generate_header(
    json_file, output_file=None, namespace=None, extra_includes=None, wasm_file=None
):
    with open(json_file, "r") as f:
        spec = json.load(f)

    interfaces = spec["interfaces"]

    namespace_forward = f"namespace {namespace} {{\n" if namespace else ""
    namespace_close = f"\n}} // namespace {namespace}" if namespace else ""

    if extra_includes:
        extra_includes_str = "\n".join(
            f'#include "{include}"' for include in extra_includes
        )
    else:
        extra_includes_str = ""

    # Generate a struct for each interface
    structs = []
    for iface in interfaces:
        structs.append(generate_interface_struct(iface, wasm_file))

    header_content = f"""#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

#include "x23/vec.h"
#include "x23/x23.h"
#include "types.hh"
#include "hogd/hogexec/components/contract_result.h"

// Auto-generated from contract spec
// DO NOT EDIT

using namespace hogd;

{extra_includes_str}

{namespace_forward}
{"".join(structs)}
{namespace_close}

"""

    if output_file:
        with open(output_file, "w") as f:
            f.write(header_content)
        print(f"Generated header file: {output_file}")
    else:
        print(header_content)


def main():
    parser = argparse.ArgumentParser(
        description="Generate a C++ header file from a JSON contract spec.",
    )
    parser.add_argument("json_spec_file", help="Path to the JSON contract spec file")
    parser.add_argument("-o", "--output", dest="output_file")
    parser.add_argument("-n", "--namespace")
    parser.add_argument("-w", "--wasm", dest="wasm_file")
    parser.add_argument("-i", "--include", dest="extra_includes", action="append")
    args = parser.parse_args()

    if not os.path.exists(args.json_spec_file):
        print(f"Error: JSON spec file '{args.json_spec_file}' not found", file=sys.stderr)
        sys.exit(1)

    generate_header(
        args.json_spec_file,
        args.output_file,
        args.namespace,
        args.extra_includes,
        args.wasm_file,
    )


if __name__ == "__main__":
    main()
