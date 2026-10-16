#!/usr/bin/env python3
"""Integration checks for portable cache keys and dependency tracking.

Run: python3 scripts/test_ccache_launcher.py
Uses temporary, isolated caches; never reads or writes the shared cache.
"""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

LAUNCHER = Path(__file__).with_name("ccache_launcher.py").resolve()


@unittest.skipUnless(shutil.which("ccache"), "ccache is required")
class CachePathsTest(unittest.TestCase):
    @unittest.skipUnless(
        shutil.which("cmake") and shutil.which("gcc"), "CMake and GCC are required"
    )
    def test_build_system_dependencies(self):
        for generator, tool in (("Ninja", "ninja"), ("Unix Makefiles", "make")):
            if not shutil.which(tool):
                continue
            with self.subTest(generator=generator), tempfile.TemporaryDirectory(
                prefix="hogd cmake cache "
            ) as temporary:
                root = Path(temporary)
                config = root / "ccache.conf"
                config.write_text("compiler_check = content\n")
                env = {
                    key: value
                    for key, value in os.environ.items()
                    if not key.startswith("CCACHE_")
                }
                env.update(
                    CCACHE_CONFIGPATH=str(config), CCACHE_DIR=str(root / "cache")
                )
                for index in range(2):
                    source = root / f"checkout{index}/source"
                    build = (
                        source / "build"
                        if index == 0
                        else root / "elsewhere/deep/output"
                    )
                    source.mkdir(parents=True)
                    (source / "probe.c").write_text(
                        '#include "source.h"\n#include "generated.h"\n'
                        "int main(void) { return SOURCE_VALUE + GENERATED_VALUE; }\n"
                    )
                    (source / "source.h").write_text("#define SOURCE_VALUE 40\n")
                    (source / "generated.h.in").write_text(
                        "#define GENERATED_VALUE 2\n"
                    )
                    launcher = ";".join(
                        (
                            sys.executable,
                            str(LAUNCHER),
                            str(source),
                            str(build),
                            shutil.which("ccache"),
                        )
                    )
                    (source / "CMakeLists.txt").write_text(
                        "cmake_minimum_required(VERSION 3.18)\nproject(cache_paths C)\n"
                        f'set(CMAKE_C_COMPILER_LAUNCHER "{launcher}")\n'
                        "configure_file(generated.h.in generated.h COPYONLY)\n"
                        "add_executable(probe probe.c)\n"
                        'target_include_directories(probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")\n'
                    )
                    subprocess.run(
                        [
                            "cmake",
                            "-S",
                            str(source),
                            "-B",
                            str(build),
                            "-G",
                            generator,
                            "-DCMAKE_C_COMPILER=gcc",
                            "-DCMAKE_BUILD_TYPE=Debug",
                        ],
                        env=env,
                        check=True,
                        capture_output=True,
                    )
                    command = ["cmake", "--build", str(build), "--parallel", "4"]
                    subprocess.run(command, env=env, check=True, capture_output=True)
                    if index == 0:
                        first_stats = self.stats(env)
                    else:
                        self.assertEqual(
                            self.stats(env)["direct_cache_hit"],
                            first_stats["direct_cache_hit"] + 1,
                        )
                    stats = self.stats(env)
                    subprocess.run(command, env=env, check=True, capture_output=True)
                    self.assertEqual(self.stats(env)["cache_miss"], stats["cache_miss"])
                    self.assertEqual(
                        self.stats(env)["direct_cache_hit"], stats["direct_cache_hit"]
                    )

                # Exercise depfiles emitted by the hit in the second checkout.
                for header, value, expected in (
                    (build / "generated.h", "GENERATED_VALUE 3", 43),
                    (source / "source.h", "SOURCE_VALUE 41", 44),
                ):
                    header.write_text(f"#define {value}\n")
                    subprocess.run(command, env=env, check=True, capture_output=True)
                    self.assertEqual(
                        subprocess.run([str(build / "probe")]).returncode, expected
                    )

    def test_portability_and_invalidation(self):
        for compiler in ("gcc", "g++", "clang", "clang++"):
            if not shutil.which(compiler):
                continue
            for debug in (False, True):
                for response in (False, True):
                    with self.subTest(
                        compiler=compiler, debug=debug, response=response
                    ):
                        self.check_builds(compiler, debug, response)

    def check_builds(self, compiler, debug, response):
        with tempfile.TemporaryDirectory(prefix="hogd cache test ") as temporary:
            root = Path(temporary)
            config = root / "ccache.conf"
            config.write_text("compiler_check = content\n")
            env = {
                key: value
                for key, value in os.environ.items()
                if not key.startswith("CCACHE_")
            }
            env.update(CCACHE_CONFIGPATH=str(config), CCACHE_DIR=str(root / "cache"))
            # An inherited BASEDIR must not undo the launcher's stable aliases.
            env["CCACHE_BASEDIR"] = str(root)
            builds = []
            for source, build, nested in (
                (root / "alice/repo", root / "alice/repo/build", ""),
                (
                    root / "bob/deep/checkout",
                    root / "unrelated/output/deep/build",
                    "subdir",
                ),
            ):
                source.mkdir(parents=True)
                (source / "include").mkdir()
                (source / "include/source.h").write_text("#define SOURCE_VALUE 40\n")
                (source / "probe.c").write_text(
                    '#include <stdio.h>\n#include <source.h>\n#include "generated.h"\n'
                    "int main(void) { puts(__FILE__); return SOURCE_VALUE + GENERATED_VALUE; }\n"
                )
                cwd = build / nested
                cwd.mkdir(parents=True)
                (build / "generated.h").write_text("#define GENERATED_VALUE 2\n")
                # Exercise both joined and separate include options, absolute
                # and relative source paths, and Make-style nested directories.
                args = [
                    "-isystem",
                    str(source / "include"),
                    "-I" + str(build),
                    "-x",
                    "c++" if compiler.endswith("++") else "c",
                    "-g" if debug else "-O2",
                    "-MD",
                    "-MF",
                    "probe.d",
                    "-MT",
                    "probe.o",
                    "-c",
                    os.path.relpath(source / "probe.c", cwd),
                    "-o",
                    "probe.o",
                ]
                if response:
                    (cwd / "input.rsp").write_text(
                        " ".join(shlex.quote(arg) for arg in args)
                    )
                    args = ["@input.rsp"]
                command = [
                    sys.executable,
                    str(LAUNCHER),
                    str(source),
                    str(build),
                    shutil.which("ccache"),
                    shutil.which(compiler),
                    *args,
                ]
                subprocess.run(
                    command, cwd=cwd, env=env, check=True, capture_output=True
                )
                builds.append((source, build, cwd, command))
                self.check_output(compiler, cwd)
                dependencies = (cwd / "probe.d").read_text()
                self.assertIn(".hogd-cache/source/include/source.h", dependencies)
                self.assertIn(".hogd-cache/build/generated.h", dependencies)
                self.assertNotIn(str(source), dependencies)

            stats = self.stats(env)
            self.assertEqual(stats["cache_miss"], 1)
            self.assertEqual(stats["direct_cache_hit"], 1)

            source, build, cwd, command = builds[-1]
            # Changes under either alias must invalidate a direct hit.
            (build / "generated.h").write_text("#define GENERATED_VALUE 3\n")
            subprocess.run(command, cwd=cwd, env=env, check=True, capture_output=True)
            self.check_output(compiler, cwd, expected=43)
            (source / "include/source.h").write_text("#define SOURCE_VALUE 41\n")
            subprocess.run(command, cwd=cwd, env=env, check=True, capture_output=True)
            self.check_output(compiler, cwd, expected=44)
            self.assertEqual(self.stats(env)["cache_miss"], 3)

    def stats(self, env):
        return json.loads(
            subprocess.check_output(
                ["ccache", "--print-stats", "--format=json"], env=env, text=True
            )
        )

    def check_output(self, compiler, cwd, expected=42):
        subprocess.run(
            [compiler, "probe.o", "-o", "probe"],
            cwd=cwd,
            check=True,
            capture_output=True,
        )
        result = subprocess.run(
            [str(cwd / "probe")], cwd=cwd, text=True, capture_output=True
        )
        self.assertEqual(result.returncode, expected)
        self.assertEqual(result.stdout.strip(), ".hogd-cache/source/probe.c")


if __name__ == "__main__":
    unittest.main()
