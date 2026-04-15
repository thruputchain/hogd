#!/usr/bin/env python3
"""Convert spec JSON type_defs to XDR .x format."""

import argparse
import json
import sys


def emit_enum(name, variants):
    lines = [f"enum {name} {{"]
    for v in variants:
        lines.append(f"\t{v['name']} = {v['value']},")
    lines.append("};")
    return "\n".join(lines)


def emit_type_def(td):
    kind = td["kind"]
    if kind == "enum":
        return emit_enum(td["name"], td["variants"])
    else:
        print(f"warning: unsupported type_def kind '{kind}', skipping", file=sys.stderr)
        return None


def main():
    parser = argparse.ArgumentParser(description="Generate .x from spec JSON type_defs")
    parser.add_argument("spec_json", help="Path to spec JSON file")
    parser.add_argument("-o", "--output", required=True, help="Output .x file")
    parser.add_argument("-n", "--namespace", required=True, help="XDR namespace")
    args = parser.parse_args()

    with open(args.spec_json) as f:
        spec = json.load(f)

    type_defs = spec.get("type_defs", [])
    if not type_defs:
        # Write empty file with namespace
        with open(args.output, "w") as f:
            f.write(f"namespace {args.namespace} {{\n\n}} // namespace\n")
        return

    body_parts = []
    for td in type_defs:
        result = emit_type_def(td)
        if result:
            body_parts.append(result)

    with open(args.output, "w") as f:
        f.write(f"namespace {args.namespace} {{\n\n")
        f.write("\n\n".join(body_parts))
        f.write(f"\n\n\n}} // namespace\n")

    print(f"Generated {args.output}")


if __name__ == "__main__":
    main()
