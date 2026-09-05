#!/usr/bin/env python3
"""Convert spec JSON type_defs to XDR .x format."""

import argparse
import json
import sys
from typing import Any


def map_xdr_type(ty: dict[str, Any]) -> tuple[str, str]:
    """Map a spec type to an XDR (type, declarator suffix) pair.

    XDR attaches vec/array sizes to the declarator, not the type
    (`opaque data<256>;`, `opaque hash[32];`), so struct emission must
    place the suffix after the field name.
    """
    kind: str = ty["kind"]
    if kind == "u32":
        return "unsigned int", ""
    elif kind == "i32":
        return "int", ""
    elif kind == "u64":
        return "unsigned hyper", ""
    elif kind == "i64":
        return "hyper", ""
    elif kind == "u128":
        return "uint128_xdr", ""
    elif kind == "i128":
        return "int128_xdr", ""
    elif kind == "bool":
        return "bool", ""
    elif kind == "address":
        return "ContractAddress", ""
    elif kind == "named":
        return ty["name"], ""
    elif kind == "vec":
        inner: dict[str, Any] = ty["element"]
        max_len: int = ty["max_len"]
        if inner["kind"] == "u8":
            return "opaque", f"<{max_len}>"
        inner_type, inner_suffix = map_xdr_type(inner)
        if inner_suffix:
            raise ValueError("nested vec/array requires a named typedef in XDR")
        return inner_type, f"<{max_len}>"
    elif kind == "array":
        inner = ty["element"]
        length: int = ty["len"]
        if inner["kind"] == "u8":
            return "opaque", f"[{length}]"
        inner_type, inner_suffix = map_xdr_type(inner)
        if inner_suffix:
            raise ValueError("nested vec/array requires a named typedef in XDR")
        return inner_type, f"[{length}]"
    elif kind == "u8":
        raise ValueError("bare u8 cannot appear in XDR — must be inside vec/array as opaque")
    else:
        raise ValueError(f"unmapped XDR type kind '{kind}'")


def emit_enum(name: str, variants: list[dict[str, Any]]) -> str:
    lines: list[str] = [f"enum {name} {{"]
    for v in variants:
        lines.append(f"\t{v['name']} = {v['value']},")
    lines.append("};")
    return "\n".join(lines)


def emit_struct(name: str, fields: list[dict[str, Any]]) -> str:
    lines: list[str] = [f"struct {name} {{"]
    for f in fields:
        xdr_type, suffix = map_xdr_type(f["type"])
        lines.append(f"\t{xdr_type} {f['name']}{suffix};")
    lines.append("};")
    return "\n".join(lines)


def emit_type_def(td: dict[str, Any]) -> str | None:
    kind: str = td["kind"]
    if kind == "enum":
        return emit_enum(td["name"], td["variants"])
    elif kind == "struct":
        return emit_struct(td["name"], td["fields"])
    else:
        print(f"warning: unsupported type_def kind '{kind}', skipping", file=sys.stderr)
        return None


def generate_xdr(type_defs: list[dict[str, Any]], namespace: str) -> str:
    """Generate XDR .x content from type_defs."""
    if not type_defs:
        return f"namespace {namespace} {{\n\n}} // namespace\n"

    body_parts: list[str] = []
    for td in type_defs:
        result = emit_type_def(td)
        if result:
            body_parts.append(result)

    out = f"%#include \"types.hh\"\n\n"
    out += f"namespace {namespace} {{\n\n"
    out += "\n\n".join(body_parts)
    out += f"\n\n\n}} // namespace\n"
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate .x from spec JSON type_defs")
    parser.add_argument("spec_json", help="Path to spec JSON file")
    parser.add_argument("-o", "--output", required=True, help="Output .x file")
    parser.add_argument("-n", "--namespace", required=True, help="XDR namespace")
    args = parser.parse_args()

    with open(args.spec_json) as f:
        spec: dict[str, Any] = json.load(f)

    content = generate_xdr(spec.get("type_defs", []), args.namespace)

    with open(args.output, "w") as f:
        f.write(content)

    print(f"Generated {args.output}")


if __name__ == "__main__":
    main()
