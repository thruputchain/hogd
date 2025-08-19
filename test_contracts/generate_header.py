#!/usr/bin/env python3

import argparse
import json
import sys
import os
import re

def generate_header(json_file, output_file=None, namespace=None, extra_includes=None):
    """Generate a C++ header file from a JSON contract spec."""
    
    with open(json_file, 'r') as f:
        content = f.read()
    
    # Fix the malformed JSON by properly escaping the typespec values
    # Replace the problematic typespec patterns
    content = re.sub(r'"typespec": "([^"]*)"', r'"typespec": "\1"', content)
    
    spec = json.loads(content)
    
    interface_name = spec['interface']
    version = spec['version']
    methods = spec['methods']
    
    # Generate export names
    export_names = [method['name'] for method in methods]
    entrypoints = [method['entrypoint'] for method in methods]
    export_names_str = ',\n            '.join(f'"{name}"' for name in export_names)
    entrypoints_str = ',\n            '.join(f"{ep}ULL" for ep in entrypoints)
    
    # Generate function signatures
    function_signatures = []
    forward_decls = []
    for method in methods:
        # Map JSON types to C++ types
        return_type = map_type_from_typespec(method['return'])
        err_type = None
        if "err" in method:
            err_type = method['err'] # Possible TODO(geoff): swap the error type to a typespec, instead of a string
            forward_decls.append(f"class {err_type};")

        args = []
        for arg in method['args']:
            arg_type = map_type_from_typespec(arg['typespec'])
            args.append(arg_type)

        if err_type:
            return_type = f"Result<{return_type}, {err_type}>"
        
        if args:
            signature = f"{return_type}(*)({', '.join(args)})"
        else:
            signature = f"{return_type}(*)()"
        function_signatures.append(signature)
    
    signatures_str = ',\n             '.join(function_signatures)

    namespace_forward = f"namespace {namespace} {{\n" if namespace else ""
    namespace_close = f"\n}} // namespace {namespace}" if namespace else ""
    
    if extra_includes:
        extra_includes_str = '\n'.join(f"#include \"{include}\"" for include in extra_includes)
    else:
        extra_includes_str = ""

    # Generate the header content
    header_content = f'''#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>

#include "x23/vec.h"
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

    // Specify serialization format, if non-default, with a line like the below.
    // constexpr static std::string_view kSerializationFormat = "txs";

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

'''
    
    if output_file:
        with open(output_file, 'w') as f:
            f.write(header_content)
        print(f"Generated header file: {output_file}")
    else:
        print(header_content)

def map_type_from_typespec(typespec):
    """Map typespec JSON strings to C++ types."""

    t = typespec["type"]
    if t == "unsigned":
        return "uint32_t"
    elif t == "signed":
        return "int32_t"
    elif t == "unsigned hyper":
        return "uint64_t"
    elif t == "signed hyper":
        return "int64_t"
    elif t == "bool":
        return "bool"
    elif t == "variable opaque":
        max_len = typespec["max"]
        return f"x23::Vec<uint8_t, {max_len}>"
    elif t == "fixed opaque":
        max_len = typespec["max"]
        return f"std::array<uint8_t, {max_len}>"
    elif t == "fixed array":
        max_len = typespec["max"]
        elem_type = map_type_from_typespec(typespec["elt_ty"])
        return f"std::array<{elem_type}, {max_len}>"
    elif t == "variable array":
        max_len = typespec["max"]
        elem_type = map_type_from_typespec(typespec["elt_ty"])
        return f"x23::Vec<{elem_type}, {max_len}>"
    elif t == "optional":
        elem_type = map_type_from_typespec(typespec["opt_ty"])
        return f"std::optional<{elem_type}>"
    elif t == "void":
        return "void"
    else:
        return t

def main():
    parser = argparse.ArgumentParser(
        description="Generate a C++ header file from a JSON contract spec.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s contract.json
  %(prog)s contract.json -o contract.h
  %(prog)s contract.json --output contract.h --namespace mycontract
        """
    )
    
    parser.add_argument(
        'json_spec_file',
        help='Path to the JSON contract specification file'
    )
    
    parser.add_argument(
        '-o', '--output',
        dest='output_file',
        help='Output header file path (if not specified, prints to stdout)'
    )
    
    parser.add_argument(
        '-n', '--namespace',
        help='C++ namespace to wrap the generated code'
    )

    parser.add_argument(
        '-i', '--include',
        dest='extra_includes',
        action='append',
        help='Additional header files to include (can be specified multiple times)'
    )
    
    args = parser.parse_args()
    
    if not os.path.exists(args.json_spec_file):
        print(f"Error: JSON spec file '{args.json_spec_file}' not found", file=sys.stderr)
        sys.exit(1)
    
    try:
        generate_header(args.json_spec_file, args.output_file, args.namespace, args.extra_includes)
    except Exception as e:
        print(f"Error generating header: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
