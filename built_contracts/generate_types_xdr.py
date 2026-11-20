#!/usr/bin/env python3
"""Convert spec JSON type_defs to XDR .x format."""

import argparse
import json
import sys
from typing import Any


def declared_name(spec_id: dict[str, Any]) -> str:
    """The declared name of a spec id: what the generated XDR calls a type.

    The spec names a user-defined type by an id object (`crate`, `major`,
    `module_path`, `declared`). XDR has no modules, so only the declared name
    is used.
    """
    return spec_id["declared"]


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
        return declared_name(ty["name"]), ""
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


def arm_name(case_name: str) -> str:
    """The XDR arm name for a union case: its name in snake_case."""
    out = ""
    for i, ch in enumerate(case_name):
        if ch.isupper() and i > 0:
            out += "_"
        out += ch.lower()
    return out


def emit_union(name: str, cases: list[dict[str, Any]]) -> str:
    """Emit a data-carrying enum as an XDR union switched on its discriminant.

    An XDR arm holds one declaration, so a case with several fields gets a
    struct of its own, named for the union and the case and emitted ahead of
    the union. A one-item tuple case holds that item directly.
    """
    structs: list[str] = []
    arms: list[str] = []
    for case in cases:
        case_type = case["case_type"]
        kind: str = case_type["kind"]
        arms.append(f"case {case['discriminant']}:")
        if kind == "void":
            arms.append("\tvoid;")
            continue
        arm = arm_name(case["name"])
        if kind == "tuple" and len(case_type["items"]) == 1:
            xdr_type, suffix = map_xdr_type(case_type["items"][0])
            arms.append(f"\t{xdr_type} {arm}{suffix};")
            continue
        if kind == "tuple":
            fields = [{"name": f"item{i}", "type": item}
                      for i, item in enumerate(case_type["items"])]
        elif kind == "struct":
            fields = case_type["fields"]
        else:
            raise ValueError(f"unmapped union case kind '{kind}'")
        struct_name = f"{name}{case['name']}"
        structs.append(emit_struct(struct_name, fields))
        arms.append(f"\t{struct_name} {arm};")
    union = "\n".join([f"union {name} switch (int kind) {{", *arms, "};"])
    return "\n\n".join([*structs, union])


def emit_type_def(td: dict[str, Any]) -> str | None:
    kind: str = td["kind"]
    if kind == "enum":
        return emit_enum(declared_name(td["name"]), td["variants"])
    elif kind == "struct":
        return emit_struct(declared_name(td["name"]), td["fields"])
    elif kind == "union":
        return emit_union(declared_name(td["name"]), td["cases"])
    else:
        print(f"warning: unsupported type_def kind '{kind}', skipping", file=sys.stderr)
        return None


def named_refs(ty: dict[str, Any]) -> list[str]:
    """The declared names of the user-defined types `ty` mentions."""
    kind: str = ty["kind"]
    if kind == "named":
        return [declared_name(ty["name"])]
    refs: list[str] = []
    for key in ("element", "inner"):
        if key in ty:
            refs += named_refs(ty[key])
    for item in ty.get("items", []):
        refs += named_refs(item)
    return refs


def field_types(td: dict[str, Any]) -> list[dict[str, Any]]:
    """The types of everything a struct or union type_def holds."""
    types = [field["type"] for field in td.get("fields", [])]
    for case in td.get("cases", []):
        case_type = case["case_type"]
        types += case_type.get("items", [])
        types += [field["type"] for field in case_type.get("fields", [])]
    return types


def definition_order(type_defs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Order type_defs so each follows the type_defs its fields name.

    XDR, like the C++ generated from it, needs a type defined before a struct
    uses it, and the spec lists type_defs in no such order. Types that do not
    depend on each other keep their spec order.
    """
    by_name = {declared_name(td["name"]): td for td in type_defs}
    ordered: list[dict[str, Any]] = []
    placed: set[str] = set()

    def place(td: dict[str, Any], pending: tuple[str, ...]) -> None:
        name = declared_name(td["name"])
        if name in placed:
            return
        if name in pending:
            raise ValueError(f"type '{name}' contains itself; XDR cannot define it")
        for ty in field_types(td):
            for ref in named_refs(ty):
                if ref in by_name:
                    place(by_name[ref], pending + (name,))
        placed.add(name)
        ordered.append(td)

    for td in type_defs:
        place(td, ())
    return ordered


def generate_xdr(type_defs: list[dict[str, Any]], namespace: str) -> str:
    """Generate XDR .x content from type_defs."""
    if not type_defs:
        return f"namespace {namespace} {{\n\n}} // namespace\n"

    body_parts: list[str] = []
    for td in definition_order(type_defs):
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
