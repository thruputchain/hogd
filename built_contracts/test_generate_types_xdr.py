#!/usr/bin/env python3
"""Tests for generate_types_xdr.py"""

import unittest
from generate_types_xdr import map_xdr_type, emit_enum, emit_struct, generate_xdr


class TestMapXdrType(unittest.TestCase):
    def test_primitives(self) -> None:
        self.assertEqual(map_xdr_type({"kind": "u32"}), "unsigned int")
        self.assertEqual(map_xdr_type({"kind": "i32"}), "int")
        self.assertEqual(map_xdr_type({"kind": "u64"}), "unsigned hyper")
        self.assertEqual(map_xdr_type({"kind": "i64"}), "hyper")
        self.assertEqual(map_xdr_type({"kind": "bool"}), "bool")

    def test_128bit(self) -> None:
        self.assertEqual(map_xdr_type({"kind": "u128"}), "uint128_xdr")
        self.assertEqual(map_xdr_type({"kind": "i128"}), "int128_xdr")

    def test_address(self) -> None:
        self.assertEqual(map_xdr_type({"kind": "address"}), "ContractAddress")

    def test_named(self) -> None:
        self.assertEqual(map_xdr_type({"kind": "named", "name": "Foo"}), "Foo")

    def test_vec_u8_is_opaque(self) -> None:
        ty = {"kind": "vec", "inner": {"kind": "u8"}, "max_len": 1000}
        self.assertEqual(map_xdr_type(ty), "opaque<1000>")

    def test_vec_non_u8(self) -> None:
        ty = {"kind": "vec", "inner": {"kind": "u32"}, "max_len": 50}
        self.assertEqual(map_xdr_type(ty), "unsigned int<50>")

    def test_array_u8_is_opaque(self) -> None:
        ty = {"kind": "array", "element": {"kind": "u8"}, "len": 32}
        self.assertEqual(map_xdr_type(ty), "opaque[32]")

    def test_array_non_u8(self) -> None:
        ty = {"kind": "array", "element": {"kind": "u64"}, "len": 4}
        self.assertEqual(map_xdr_type(ty), "unsigned hyper[4]")

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
            {"name": "channel_id", "ty": {"kind": "u64"}},
            {"name": "balance", "ty": {"kind": "i128"}},
        ])
        self.assertIn("struct ChannelState {", result)
        self.assertIn("\tunsigned hyper channel_id;", result)
        self.assertIn("\tint128_xdr balance;", result)

    def test_struct_with_opaque(self) -> None:
        result = emit_struct("WithBytes", [
            {"name": "data", "ty": {"kind": "vec", "inner": {"kind": "u8"}, "max_len": 256}},
            {"name": "hash", "ty": {"kind": "array", "element": {"kind": "u8"}, "len": 32}},
        ])
        self.assertIn("\topaque<256> data;", result)
        self.assertIn("\topaque[32] hash;", result)


class TestGenerateXdr(unittest.TestCase):
    def test_empty_type_defs(self) -> None:
        result = generate_xdr([], "my_ns")
        self.assertIn("namespace my_ns {", result)
        self.assertIn("} // namespace", result)
        self.assertNotIn("%#include", result)

    def test_with_type_defs(self) -> None:
        type_defs = [
            {"kind": "enum", "name": "E", "variants": [{"name": "A", "value": 1}]},
            {"kind": "struct", "name": "S", "fields": [
                {"name": "x", "ty": {"kind": "u32"}}
            ]},
        ]
        result = generate_xdr(type_defs, "test_ns")
        self.assertIn('%#include "types.hh"', result)
        self.assertIn("namespace test_ns {", result)
        self.assertIn("enum E {", result)
        self.assertIn("struct S {", result)


if __name__ == "__main__":
    unittest.main()
