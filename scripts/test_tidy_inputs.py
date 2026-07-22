#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import tidy_inputs


class RepresentativeSourceTest(unittest.TestCase):
    def test_prefers_companion_source(self):
        affected = {"hogd/lib/widget.cc", "hogd/tests/widget_test.cc"}
        changed = {"hogd/tests/widget_test.cc"}

        self.assertEqual(
            tidy_inputs.representative_source(
                "hogd/lib/widget.h", affected, changed
            ),
            "hogd/lib/widget.cc",
        )

    def test_prefers_changed_dependent_without_companion(self):
        affected = {"hogd/lib/other.cc", "hogd/tests/widget_test.cc"}
        changed = {"hogd/tests/widget_test.cc"}

        self.assertEqual(
            tidy_inputs.representative_source(
                "hogd/lib/widget.h", affected, changed
            ),
            "hogd/tests/widget_test.cc",
        )

    def test_fallback_is_deterministic(self):
        affected = {"hogd/z.cc", "hogd/a.cc"}

        self.assertEqual(
            tidy_inputs.representative_source(
                "hogd/lib/widget.h", affected, set()
            ),
            "hogd/a.cc",
        )


class ChangedInputsTest(unittest.TestCase):
    def test_selects_representatives_and_deduplicates(self):
        changed = {
            "hogd/direct.cc",
            "hogd/companion.cc",
            "hogd/companion.h",
            "hogd/shared_one.h",
            "hogd/shared_two.h",
            "hogd/standalone.h",
        }
        dependencies = {
            "hogd/companion.h": {"hogd/companion.cc", "hogd/other.cc"},
            "hogd/shared_one.h": {"hogd/direct.cc", "hogd/shared.cc"},
            "hogd/shared_two.h": {"hogd/shared.cc"},
        }

        with patch.object(
            tidy_inputs, "changed_files", return_value=changed
        ), patch.object(
            tidy_inputs, "dependency_graph", return_value=(set(), dependencies)
        ):
            inputs = tidy_inputs.inputs_for_changed(
                Path("."), Path("compile_commands.json"), "base", "head"
            )

        self.assertEqual(
            inputs,
            {
                "hogd/direct.cc",
                "hogd/companion.cc",
                "hogd/shared.cc",
                "hogd/standalone.h",
            },
        )

    def test_skips_dependency_graph_without_changed_headers(self):
        with patch.object(
            tidy_inputs, "changed_files", return_value={"hogd/direct.cc"}
        ), patch.object(tidy_inputs, "dependency_graph") as dependency_graph:
            inputs = tidy_inputs.inputs_for_changed(
                Path("."), Path("compile_commands.json"), "base", "head"
            )

        self.assertEqual(inputs, {"hogd/direct.cc"})
        dependency_graph.assert_not_called()


class FullInputsTest(unittest.TestCase):
    def test_keeps_all_sources_and_only_unreferenced_headers(self):
        with patch.object(
            tidy_inputs,
            "dependency_graph",
            return_value=(
                {"hogd/one.cc", "hogd/two.cc"},
                {"hogd/referenced.h": {"hogd/one.cc"}},
            ),
        ), patch.object(
            tidy_inputs,
            "all_files",
            return_value={"hogd/referenced.h", "hogd/standalone.h"},
        ) as mock_all_files:
            inputs = tidy_inputs.inputs_for_full(
                Path("."), Path("compile_commands.json")
            )

        mock_all_files.assert_called_once_with(Path("."), ".h")
        self.assertEqual(
            inputs, {"hogd/one.cc", "hogd/two.cc", "hogd/standalone.h"}
        )


class ProxyCompileDatabaseTest(unittest.TestCase):
    def test_standalone_headers_get_test_framework_include_paths(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "hogd/base/example.cc"
            header = root / "hogd/test_ffi/helpers.h"
            source.parent.mkdir(parents=True)
            header.parent.mkdir(parents=True)
            source.touch()
            header.touch()
            compile_db = root / "compile_commands.json"
            compile_db.write_text(
                json.dumps(
                    [
                        {
                            "directory": str(root),
                            "arguments": [
                                "clang++",
                                "-I",
                                str(root / "hogd"),
                                "-c",
                                str(source),
                                "-o",
                                "example.o",
                            ],
                            "file": str(source),
                        }
                    ]
                ),
                encoding="utf-8",
            )
            proxy_dir = root / "proxies"
            proxy_compile_db = root / "proxy_compile_commands.json"

            rewritten = tidy_inputs.write_proxy_compile_db(
                root,
                compile_db,
                {"hogd/test_ffi/helpers.h"},
                proxy_dir,
                proxy_compile_db,
            )

            entries = json.loads(proxy_compile_db.read_text(encoding="utf-8"))
            proxy_entry = entries[-1]
            self.assertEqual(
                rewritten,
                {str(proxy_dir / "hogd__test_ffi__helpers.h.cc")},
            )
            for include_dir in tidy_inputs.PROXY_SYSTEM_INCLUDE_DIRS:
                self.assertIn(
                    str(root / include_dir), proxy_entry["arguments"]
                )


if __name__ == "__main__":
    unittest.main()
