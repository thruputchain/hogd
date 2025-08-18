#!/usr/bin/env python3

import json
import sys
import os
import re

def generate_header(json_file, output_file=None):
    """Generate a C++ header file from a JSON contract spec."""
    
    with open(json_file, 'r') as f:
        content = f.read()
    
    # Fix the malformed JSON by properly escaping the typespec values
    # Replace the problematic typespec patterns
    content = re.sub(r'"typespec": "([^"]*)"', r'"typespec": "\1"', content)
    
    try:
        spec = json.loads(content)
    except json.JSONDecodeError as e:
        print(f"Error parsing JSON: {e}")
        print("Attempting to fix malformed JSON...")
        
        # More aggressive fix for the typespec issue
        content = re.sub(r'"typespec": "([^"]*)"', lambda m: f'"typespec": "{m.group(1).replace('"', '\\"')}"', content)
        
        try:
            spec = json.loads(content)
        except json.JSONDecodeError as e2:
            print(f"Still cannot parse JSON: {e2}")
            print("Content:")
            print(content)
            sys.exit(1)
    
    interface_name = spec['interface']
    version = spec['version']
    methods = spec['methods']
    
    # Generate export names
    export_names = [method['name'] for method in methods]
    export_names_str = ',\n            '.join(f'"{name}"' for name in export_names)
    
    # Generate function signatures
    function_signatures = []
    for method in methods:
        # Map JSON types to C++ types
        return_type = map_type(method['return'])
        args = []
        for arg in method['args']:
            arg_type = map_type_from_typespec(arg['typespec'])
            args.append(arg_type)
        
        if args:
            signature = f"{return_type}(*)({', '.join(args)})"
        else:
            signature = f"{return_type}(*)()"
        function_signatures.append(signature)
    
    signatures_str = ', '.join(function_signatures)
    
    # Generate the header content
    header_content = f'''#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

// Auto-generated from contract spec

struct {interface_name} {{
    constexpr static std::string_view kTraitName = "{interface_name}";
    constexpr static std::string_view kVersion = "{version}";

    // Specify serialization format, if non-default, with a line like the below.
    // constexpr static std::string_view kSerializationFormat = "txs";

    // These will be sorted before being passed as indices to method id derivation.
    constexpr static auto kExportNames = std::array<std::string_view, {len(export_names)}>{{
            {export_names_str}
        }};

    using export_types = std::tuple<{signatures_str}>;
}};
'''
    
    if output_file:
        with open(output_file, 'w') as f:
            f.write(header_content)
        print(f"Generated header file: {output_file}")
    else:
        print(header_content)

def map_type(json_type):
    """Map JSON type specifications to C++ types."""
    if json_type == "unsigned int":
        return "uint32_t"
    elif json_type == "int":
        return "int32_t"
    elif json_type == "unsigned long":
        return "uint64_t"
    elif json_type == "long":
        return "int64_t"
    elif json_type == "bool":
        return "bool"
    elif json_type == "string":
        return "std::string_view"
    elif json_type == "void":
        return "void"
    else:
        # Default to uint32_t for unknown types
        return "uint32_t"

def map_type_from_typespec(typespec):
    """Map typespec JSON strings to C++ types."""

    t = typespec["type"]
    if t == "unsigned":
        return "uint32_t"
    elif t == "signed":
        return "int32_t"
    elif t == "bool":
        return "bool"
    elif t == "string":
        return "std::string_view"
    else:
        raise ValueError("unimplemented typespec")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_header.py <json_spec_file> [output_header_file]")
        sys.exit(1)
    
    json_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    if not os.path.exists(json_file):
        print(f"Error: JSON spec file '{json_file}' not found")
        sys.exit(1)
    
    generate_header(json_file, output_file)

if __name__ == "__main__":
    main()
