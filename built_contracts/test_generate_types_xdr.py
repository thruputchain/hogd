#!/usr/bin/env python3
"""Tests for generate_types_xdr.py

Run from built_contracts/:
    python3 -m unittest test_generate_types_xdr -v
"""

import unittest
from typing import Any

from generate_types_xdr import map_xdr_type, emit_enum, emit_struct, generate_xdr


def spec_id(declared: str) -> dict[str, Any]:
    """A spec id object, as the spec JSON names a user-defined type."""
    return {"crate": "demo", "major": "0.2", "module_path": "demo::types",
            "declared": declared}


class TestMapXdrType(unittest.TestCase):
    def test_primitives(self) -> None:
        self.assertEqual(map_xdr_type({"kind": "u32"}), ("unsigned int", ""))
        self.assertEqual(map_xdr_type({"kind": "i32"}), ("int", ""))
        self.assertEqual(map_xdr_type({"kind": "u64"}), ("unsigned hyper", ""))
        self.assertEqual(map_xdr_type({"kind": "i64"}), ("hyper", ""))
        self.assertEqual(map_xdr_type({"kind": "bool"}), ("bool", ""))

    def test_128bit(self) -> None:
        self.assertEqual(map_xdr_type({"kind": "u128"}), ("uint128_xdr", ""))
        self.assertEqual(map_xdr_type({"kind": "i128"}), ("int128_xdr", ""))

    def test_address(self) -> None:
        self.assertEqual(map_xdr_type({"kind": "address"}), ("ContractAddress", ""))

    def test_named(self) -> None:
        self.assertEqual(map_xdr_type({"kind": "named", "name": spec_id("Foo")}),
                         ("Foo", ""))

    def test_vec_u8_is_opaque(self) -> None:
        ty = {"kind": "vec", "element": {"kind": "u8"}, "max_len": 1000}
        self.assertEqual(map_xdr_type(ty), ("opaque", "<1000>"))

    def test_vec_non_u8(self) -> None:
        ty = {"kind": "vec", "element": {"kind": "u32"}, "max_len": 50}
        self.assertEqual(map_xdr_type(ty), ("unsigned int", "<50>"))

    def test_array_u8_is_opaque(self) -> None:
        ty = {"kind": "array", "element": {"kind": "u8"}, "len": 32}
        self.assertEqual(map_xdr_type(ty), ("opaque", "[32]"))

    def test_array_non_u8(self) -> None:
        ty = {"kind": "array", "element": {"kind": "u64"}, "len": 4}
        self.assertEqual(map_xdr_type(ty), ("unsigned hyper", "[4]"))

    def test_nested_vec_raises(self) -> None:
        ty = {"kind": "vec",
              "element": {"kind": "vec", "element": {"kind": "u8"}, "max_len": 8},
              "max_len": 4}
        with self.assertRaises(ValueError):
            map_xdr_type(ty)

    def test_bare_u8_raises(self) -> None:
        with self.assertRaises(ValueError):
            map_xdr_type({"kind": "u8"})

    def test_unknown_kind_raises(self) -> None:
        with self.assertRaises(ValueError):
            map_xdr_type({"kind": "some_unknown_type"})


class TestEmitEnum(unittest.TestCase):
    def test_basic_enum(self) -> None:
        result = emit_enum("MyError", [
            {"name": "NotFound", "value": 1},
            {"name": "Unauthorized", "value": 2},
        ])
        self.assertIn("enum MyError {", result)
        self.assertIn("\tNotFound = 1,", result)
        self.assertIn("\tUnauthorized = 2,", result)
        self.assertIn("};", result)


class TestEmitStruct(unittest.TestCase):
    def test_basic_struct(self) -> None:
        result = emit_struct("ChannelState", [
            {"name": "channel_id", "type": {"kind": "u64"}},
            {"name": "balance", "type": {"kind": "i128"}},
        ])
        self.assertIn("struct ChannelState {", result)
        self.assertIn("\tunsigned hyper channel_id;", result)
        self.assertIn("\tint128_xdr balance;", result)

    def test_struct_with_opaque(self) -> None:
        result = emit_struct("WithBytes", [
            {"name": "data", "type": {"kind": "vec", "element": {"kind": "u8"}, "max_len": 256}},
            {"name": "hash", "type": {"kind": "array", "element": {"kind": "u8"}, "len": 32}},
        ])
        self.assertIn("\topaque data<256>;", result)
        self.assertIn("\topaque hash[32];", result)


class TestGenerateXdr(unittest.TestCase):
    def test_empty_type_defs(self) -> None:
        result = generate_xdr([], "my_ns")
        self.assertIn("namespace my_ns {", result)
        self.assertIn("} // namespace", result)
        self.assertNotIn("%#include", result)

    def test_with_type_defs(self) -> None:
        type_defs = [
            {"kind": "enum", "name": spec_id("E"), "variants": [{"name": "A", "value": 1}]},
            {"kind": "struct", "name": spec_id("S"), "fields": [
                {"name": "x", "type": {"kind": "u32"}}
            ]},
        ]
        result = generate_xdr(type_defs, "test_ns")
        self.assertIn('%#include "types.hh"', result)
        self.assertIn("namespace test_ns {", result)
        self.assertIn("enum E {", result)
        self.assertIn("struct S {", result)

    def test_types_are_defined_before_the_structs_that_use_them(self) -> None:
        # The spec lists these by name, which puts each user before its use.
        named = lambda n: {"kind": "named", "name": spec_id(n)}
        type_defs = [
            {"kind": "struct", "name": spec_id("Call"), "fields": [
                {"name": "tokens", "type": {
                    "kind": "vec", "element": named("Ref"), "max_len": 64}},
            ]},
            {"kind": "struct", "name": spec_id("Ref"), "fields": [
                {"name": "source", "type": named("Source")},
                {"name": "index", "type": {"kind": "u32"}},
            ]},
            {"kind": "enum", "name": spec_id("Source"),
             "variants": [{"name": "A", "value": 0}]},
            {"kind": "struct", "name": spec_id("Unrelated"), "fields": [
                {"name": "x", "type": {"kind": "u32"}}
            ]},
        ]
        result = generate_xdr(type_defs, "test_ns")
        positions = [result.index(d) for d in (
            "enum Source {", "struct Ref {", "struct Call {",
            "struct Unrelated {")]
        self.assertEqual(positions, sorted(positions))

    def test_spec_order_is_kept_without_dependencies(self) -> None:
        type_defs = [
            {"kind": "struct", "name": spec_id(n), "fields": [
                {"name": "x", "type": {"kind": "u32"}}
            ]} for n in ("B", "A", "C")
        ]
        result = generate_xdr(type_defs, "test_ns")
        positions = [result.index(f"struct {n} {{") for n in ("B", "A", "C")]
        self.assertEqual(positions, sorted(positions))

    def test_union(self) -> None:
        type_defs = [
            {"kind": "struct", "name": spec_id("User"), "fields": [
                {"name": "r", "type": {"kind": "named", "name": spec_id("Ref")}},
            ]},
            {"kind": "union", "name": spec_id("Ref"), "cases": [
                {"name": "None", "discriminant": 0,
                 "case_type": {"kind": "void"}},
                {"name": "Minted", "discriminant": 1,
                 "case_type": {"kind": "tuple", "items": [{"kind": "u32"}]}},
                {"name": "FromCall", "discriminant": 2,
                 "case_type": {"kind": "struct", "fields": [
                     {"name": "call", "type": {"kind": "u32"}},
                     {"name": "index", "type": {"kind": "u32"}}]}},
                {"name": "Pair", "discriminant": 3,
                 "case_type": {"kind": "tuple", "items": [
                     {"kind": "u32"},
                     {"kind": "named", "name": spec_id("Leaf")}]}},
            ]},
            {"kind": "struct", "name": spec_id("Leaf"), "fields": [
                {"name": "x", "type": {"kind": "u32"}}
            ]},
        ]
        result = generate_xdr(type_defs, "test_ns")
        self.assertIn(
            "struct RefFromCall {\n\tunsigned int call;\n\tunsigned int index;\n};",
            result)
        self.assertIn(
            "struct RefPair {\n\tunsigned int item0;\n\tLeaf item1;\n};", result)
        # The discriminants are the spec's, under the cases' own names.
        self.assertIn(
            "enum RefKind {\n\tNone = 0,\n\tMinted = 1,\n"
            "\tFromCall = 2,\n\tPair = 3,\n};", result)
        self.assertIn(
            "union Ref switch (RefKind kind) {\n"
            "case None:\n\tvoid;\n"
            "case Minted:\n\tunsigned int minted;\n"
            "case FromCall:\n\tRefFromCall from_call;\n"
            "case Pair:\n\tRefPair pair;\n"
            "};", result)
        # A union follows what its cases name, and precedes what names it.
        positions = [result.index(d) for d in (
            "struct Leaf {", "union Ref ", "struct User {")]
        self.assertEqual(positions, sorted(positions))

    def test_self_containing_type_raises(self) -> None:
        type_defs = [
            {"kind": "struct", "name": spec_id("Loop"), "fields": [
                {"name": "next", "type": {
                    "kind": "option",
                    "inner": {"kind": "named", "name": spec_id("Loop")}}},
            ]},
        ]
        with self.assertRaises(ValueError):
            generate_xdr(type_defs, "test_ns")


if __name__ == "__main__":
    unittest.main()
