#!/usr/bin/env python3

import argparse
import json
import sys
import os
import re
import traceback


def generate_header(
    json_file, output_file=None, namespace=None, extra_includes=None, wasm_file=None
):
    """Generate a C++ header file from a JSON contract spec."""

    with open(json_file, "r") as f:
        content = f.read()

    # Fix the malformed JSON by properly escaping the typespec values
    # Replace the problematic typespec patterns
    content = re.sub(r'"typespec": "([^"]*)"', r'"typespec": "\1"', content)

    try:
        spec = json.loads(content)
    except json.JSONDecodeError as e:
        # Show which file failed and context around the error
        lines = content.split("\n")
        error_line = e.lineno - 1  # JSONDecodeError uses 1-based indexing
        start_line = max(0, error_line - 4)
        end_line = min(len(lines), error_line + 5)
        context = "\n".join(
            f"{i+1:4d}: {lines[i]}" for i in range(start_line, end_line)
        )
        error_msg = f"Failed to parse {json_file} at line {e.lineno}, column {e.colno}:\n{context}\n\nOriginal error: {e}"
        raise ValueError(error_msg) from e

    interface_name = spec["interface"]
    version = spec["version"]
    methods = spec["methods"]

    # Generate export names
    export_names = [method["name"] for method in methods]
    entrypoints = [method["entrypoint"] for method in methods]
    export_names_str = ",\n            ".join(f'"{name}"' for name in export_names)
    entrypoints_str = ",\n            ".join(f"{ep}ULL" for ep in entrypoints)

    # Generate function signatures
    function_signatures = []
    forward_decls = []
    for method in methods:
        try:
            # Map JSON types to C++ types
            return_typespec = method["return"]
            return_type = map_type_from_typespec(return_typespec)
            err_type = None
            if "err" in method:
                err = method["err"]
                # err may be a list of error type names or a single string
                if isinstance(err, list):
                    err_type = err[0] if err else None
                else:
                    err_type = err
                if err_type:
                    forward_decls.append(f"class {err_type};")

            args = []
            for arg in method["args"]:
                arg_type = map_type_from_typespec(arg["typespec"])
                args.append(arg_type)

            if err_type:
                return_type = f"Result<{return_type}, {err_type}>"

            if args:
                signature = f"{return_type}(*)({', '.join(args)})"
            else:
                signature = f"{return_type}(*)()"
            function_signatures.append(signature)
        except Exception as e:
            print(f"Error processing method {method}: {e}", file=sys.stderr)
            traceback.print_exc(file=sys.stderr)
            raise

    signatures_str = ",\n             ".join(function_signatures)

    namespace_forward = f"namespace {namespace} {{\n" if namespace else ""
    namespace_close = f"\n}} // namespace {namespace}" if namespace else ""

    wasm_str = (
        f'constexpr static std::string_view kWasmFile = "{wasm_file}";'
        if wasm_file
        else ""
    )

    if extra_includes:
        extra_includes_str = "\n".join(
            f'#include "{include}"' for include in extra_includes
        )
    else:
        extra_includes_str = ""

    # Generate the header content
    header_content = f"""#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

#include "x23/vec.h"
#include "x23/x23.h"
#include "types.hh"
#include "hogd/hogexec/runtime/contract_result.h"

// Auto-generated from contract spec
// DO NOT EDIT

using namespace hogd;

{extra_includes_str}

{namespace_forward}

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

{namespace_close}

"""

    if output_file:
        with open(output_file, "w") as f:
            f.write(header_content)
        print(f"Generated header file: {output_file}")
    else:
        print(header_content)


def map_type_from_typespec(typespec):
    """Map typespec JSON strings to C++ types.

    Args:
        typespec: The typespec dict to map
    """

    t = typespec["type"]
    if t == "unsigned":
        return "uint32_t"
    elif t == "signed":
        return "int32_t"
    elif t == "unsigned hyper":
        return "uint64_t"
    elif t == "hyper":
        return "int64_t"
    elif t == "bool":
        return "bool"
    elif t == "variable opaque":
        max_len = typespec["max"]
        return f"x23::Vec<uint8_t, {max_len}>"
    elif t == "fixed opaque":
        max_len = typespec["len"]
        return f"std::array<uint8_t, {max_len}>"
    elif t == "fixed array":
        max_len = typespec["len"]
        elem_type = map_type_from_typespec(typespec["element_type"])
        return f"std::array<{elem_type}, {max_len}>"
    elif t == "variable array":
        max_len = typespec["max"]
        elem_type = map_type_from_typespec(typespec["element_type"])
        return f"x23::Vec<{elem_type}, {max_len}>"
    elif t == "optional":
        elem_type = map_type_from_typespec(typespec["element_type"])
        return f"x23::Ptr<{elem_type}>"
    elif t == "void":
        return "void"
    elif t == "struct":
        # Check if this is a tuple (all fields named "unnamed_N" or "unnamed_optional")
        fields = typespec.get("fields", [])
        if is_tuple_struct(fields):
            # Special case: single-element struct is used as wrapper for array dimensions or optionals
            # For nested arrays like [[u8; 4]; 5], xdrs wraps each dimension in a single-element struct with "unnamed_0"
            # For optional arrays like [Option<u64>; 2], xdrs wraps optionals in struct with "unnamed_0"
            if len(fields) == 1 and fields[0]["name"] in ("unnamed_0"):
                field_typespec = fields[0]["element_type"]
                return map_type_from_typespec(field_typespec)

            # Multi-element tuple OR single-element tuple at top level: Map to std::tuple<...>
            field_types = []
            for field in fields:
                field_typespec = field["element_type"]
                field_types.append(map_type_from_typespec(field_typespec))
            return f"std::tuple<{', '.join(field_types)}>"
        else:
            # Named struct shouldn't appear in return types
            raise ValueError(f"Named structs not supported in return types: {typespec}")
    else:
        return t


def is_tuple_struct(fields):
    """Check if struct fields match tuple pattern (unnamed_0, unnamed_1, ...) or wrapper patterns (unnamed_optional)."""
    if not fields:
        return False

    # Single-element wrapper patterns: unnamed_0 (array dimension) or unnamed_optional (optional in array)
    if len(fields) == 1 and fields[0].get("name") in ("unnamed_0", "unnamed_optional"):
        return True

    # Multi-element tuple pattern: unnamed_0, unnamed_1, unnamed_2, ...
    for i, field in enumerate(fields):
        expected_name = f"unnamed_{i}"
        if field.get("name") != expected_name:
            return False

    return True


def main():
    parser = argparse.ArgumentParser(
        description="Generate a C++ header file from a JSON contract spec.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s contract.json
  %(prog)s contract.json -o contract.h
  %(prog)s contract.json --output contract.h --namespace mycontract
        """,
    )

    parser.add_argument(
        "json_spec_file", help="Path to the JSON contract specification file"
    )

    parser.add_argument(
        "-o",
        "--output",
        dest="output_file",
        help="Output header file path (if not specified, prints to stdout)",
    )

    parser.add_argument(
        "-n", "--namespace", help="C++ namespace to wrap the generated code"
    )

    parser.add_argument(
        "-w",
        "--wasm",
        dest="wasm_file",
        help="WASM file to include in the generated header",
    )

    parser.add_argument(
        "-i",
        "--include",
        dest="extra_includes",
        action="append",
        help="Additional header files to include (can be specified multiple times)",
    )

    args = parser.parse_args()

    if not os.path.exists(args.json_spec_file):
        print(
            f"Error: JSON spec file '{args.json_spec_file}' not found", file=sys.stderr
        )
        sys.exit(1)

    try:
        generate_header(
            args.json_spec_file,
            args.output_file,
            args.namespace,
            args.extra_includes,
            args.wasm_file,
        )
    except Exception as e:
        print(f"Error generating header: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
