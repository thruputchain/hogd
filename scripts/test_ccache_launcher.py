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
    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux locale normalization")
    def test_locale_remote_cache_sharing(self):
        for compiler in ("gcc", "g++", "clang", "clang++"):
            if not shutil.which(compiler):
                continue
            for response in (False, True):
                with self.subTest(
                    compiler=compiler, response=response
                ), tempfile.TemporaryDirectory(prefix="hogd-cache-locale-") as temporary:
                    root = Path(temporary)
                    source, build = root / "source-é", root / "build-é"
                    source.mkdir()
                    build.mkdir()
                    (source / "probe-é.c").write_text(
                        "#warning locale cache probe\nint probe(void) { return 42; }\n"
                    )
                    config = root / "ccache.conf"
                    config.write_text(
                        "compiler_check = content\n"
                        f"remote_storage = {(root / 'remote').as_uri()}\n"
                    )
                    env = {
                        key: value
                        for key, value in os.environ.items()
                        if not key.startswith(("CCACHE_", "LC_"))
                        and key not in ("LANG", "LANGUAGE")
                    }
                    env["CCACHE_CONFIGPATH"] = str(config)
                    arguments = [
                        "-x",
                        "c++" if compiler.endswith("++") else "c",
                        "-c",
                        str(source / "probe-é.c"),
                        "-o",
                        "probe.o",
                    ]
                    if response:
                        (build / "input-é.rsp").write_text(
                            " ".join(shlex.quote(argument) for argument in arguments),
                            encoding="utf-8",
                        )
                        arguments = ["@input-é.rsp"]
                    command = [
                        sys.executable,
                        str(LAUNCHER),
                        str(source),
                        str(build),
                        shutil.which("ccache"),
                        shutil.which(compiler),
                        *arguments,
                    ]
                    # CI and login shells supply different locale variables.
                    # Each pass starts with an empty local cache, so subsequent
                    # hits must come from the isolated file-backed remote cache.
                    for index, locale in enumerate(
                        (
                            {"LC_CTYPE": "C.UTF-8"},
                            {"LANG": "en_US.UTF-8"},
                            {
                                "LANG": "en_US.UTF-8",
                                "LC_ALL": "C",
                                "LC_CTYPE": "POSIX",
                                "LC_MESSAGES": "C",
                                "LC_TIME": "en_US.UTF-8",
                                "LANGUAGE": "de:fr",
                            },
                            {
                                # Normalizing the child environment does not
                                # change Python's startup filesystem encoding.
                                "LC_ALL": "C",
                                "PYTHONUTF8": "0",
                                "PYTHONCOERCECLOCALE": "0",
                            },
                        )
                    ):
                        with self.subTest(locale=locale):
                            cache_env = {
                                **env,
                                **locale,
                                "CCACHE_DIR": str(root / f"local-{index}"),
                            }
                            result = subprocess.run(
                                command,
                                cwd=build,
                                env=cache_env,
                                check=True,
                                capture_output=True,
                            )
                            stats = self.stats(cache_env)
                            output = (build / "probe.o").read_bytes()
                            (build / "probe.o").unlink()
                            self.assertEqual(stats["remote_storage_error"], 0)
                            if index == 0:
                                self.assertEqual(stats["cache_miss"], 1)
                                self.assertGreater(stats["remote_storage_write"], 0)
                                self.assertIn(b"locale cache probe", result.stderr)
                                expected_output, expected_stderr = output, result.stderr
                            else:
                                self.assertEqual(stats["cache_miss"], 0)
                                self.assertEqual(
                                    stats["direct_cache_hit"]
                                    + stats["preprocessed_cache_hit"],
                                    1,
                                )
                                self.assertGreater(stats["remote_storage_hit"], 0)
                                self.assertEqual(output, expected_output)
                                self.assertEqual(result.stderr, expected_stderr)

    def test_compiler_alias_cache_sharing(self):
        for compiler, alias in (
            ("gcc", "cc"),
            ("g++", "c++"),
            ("clang", "cc"),
            ("clang++", "c++"),
        ):
            # Clang chooses its C++ driver behavior from argv[0], including
            # installations where clang and clang++ are the same executable.
            executable = shutil.which("clang" if compiler == "clang++" else compiler)
            if not executable:
                continue
            compiler_arguments = []
            if compiler in ("gcc", "g++"):
                frontend = subprocess.check_output(
                    [
                        executable,
                        "-print-prog-name=" + ("cc1plus" if compiler == "g++" else "cc1"),
                    ],
                    text=True,
                ).strip()
                # Relocated GCC drivers still need their installed frontend.
                compiler_arguments.append("-B" + str(Path(frontend).parent) + os.sep)
            for link_type in ("symlink", "hardlink"):
                with self.subTest(
                    compiler=compiler, link_type=link_type
                ), tempfile.TemporaryDirectory(prefix="hogd compiler alias ") as temporary:
                    root = Path(temporary)
                    source, build, binaries = (
                        root / "source",
                        root / "build",
                        root / "bin",
                    )
                    for directory in (source, build, binaries):
                        directory.mkdir()
                    # Copy first so the hardlink test works even when /tmp and
                    # the installed compiler are on different filesystems.
                    canonical = binaries / compiler
                    shutil.copy2(Path(executable).resolve(), canonical)
                    selected = binaries / alias
                    if link_type == "symlink":
                        selected.symlink_to(canonical)
                    else:
                        selected.hardlink_to(canonical)
                    if compiler.startswith("clang"):
                        other_driver = binaries / (
                            "clang" if compiler == "clang++" else "clang++"
                        )
                        other_driver.symlink_to(canonical)
                    config = root / "ccache.conf"
                    config.write_text("compiler_check = content\n")
                    env = {
                        key: value
                        for key, value in os.environ.items()
                        if not key.startswith("CCACHE_")
                    }
                    env.update(
                        CCACHE_CONFIGPATH=str(config),
                        CCACHE_DIR=str(root / "cache"),
                        PATH=str(binaries) + os.pathsep + os.environ.get("PATH", ""),
                    )
                    # Deliberately keep a .c suffix for C++: resolving a
                    # clang++ symlink to clang would silently select C mode.
                    (source / "probe.c").write_text(
                        "#ifndef __cplusplus\n#error C++ driver required\n#endif\n"
                        "template <int Value> int answer() { return Value; }\n"
                        "int main() { return answer<42>(); }\n"
                        if alias == "c++"
                        else "int main(void) { return 42; }\n"
                    )
                    command = [
                        sys.executable,
                        str(LAUNCHER),
                        str(source),
                        str(build),
                        shutil.which("ccache"),
                    ]
                    for driver in (selected, alias, canonical):
                        subprocess.run(
                            [
                                *command,
                                str(driver),
                                *compiler_arguments,
                                "-c",
                                str(source / "probe.c"),
                                "-o",
                                "probe.o",
                            ],
                            cwd=build,
                            env=env,
                            check=True,
                            capture_output=True,
                        )
                        self.assertTrue((build / "probe.o").is_file())
                        (build / "probe.o").unlink()
                    self.assertEqual(self.stats(env)["cache_miss"], 1)
                    self.assertEqual(self.stats(env)["direct_cache_hit"], 2)

    def test_compiler_wrappers_are_preserved(self):
        for alias, candidates, masquerade in (
            ("cc", ("gcc", "clang"), False),
            ("c++", ("g++", "clang++"), False),
            ("custom-cc", ("gcc", "clang"), False),
            ("cc", ("gcc", "clang"), True),
            ("c++", ("g++", "clang++"), True),
        ):
            with self.subTest(
                alias=alias, masquerade=masquerade
            ), tempfile.TemporaryDirectory(prefix="hogd compiler wrapper ") as temporary:
                root = Path(temporary)
                source, build = root / "source", root / "build"
                source.mkdir()
                build.mkdir()
                recorder = root / "record-compiler"
                recorder.write_text(
                    f"#!{sys.executable}\n"
                    "import json, pathlib, sys\n"
                    "pathlib.Path('compiler.json').write_text(json.dumps(sys.argv[1]))\n"
                )
                recorder.chmod(0o755)
                selected = root / alias
                if masquerade:
                    # ccache masquerade aliases dispatch by the original name.
                    selected.hardlink_to(recorder)
                else:
                    selected.write_text("#!/bin/sh\nexit 0\n")
                    selected.chmod(0o755)
                for candidate in candidates:
                    path = root / candidate
                    if alias == "custom-cc" or masquerade:
                        # These names are preserved even for the same file.
                        path.hardlink_to(selected)
                    else:
                        # Identical contents do not establish executable identity:
                        # a separate wrapper can behave differently by location.
                        shutil.copy2(selected, path)
                subprocess.run(
                    [
                        sys.executable,
                        str(LAUNCHER),
                        str(source),
                        str(build),
                        str(recorder),
                        str(selected),
                        "-c",
                        "probe.c",
                    ],
                    cwd=build,
                    check=True,
                    capture_output=True,
                )
                self.assertEqual(
                    json.loads((build / "compiler.json").read_text()), str(selected)
                )

    def test_response_macro_escapes(self):
        for compiler in ("gcc", "g++", "clang", "clang++"):
            if not shutil.which(compiler):
                continue
            with self.subTest(compiler=compiler), tempfile.TemporaryDirectory(
                prefix="hogd response escapes "
            ) as temporary:
                root = Path(temporary)
                source, build = root / "source", root / "build"
                source.mkdir()
                build.mkdir()
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
                (source / "probe.c").write_text(
                    "#include <stdio.h>\nint main(void) { fputs(MSG, stdout); }\n"
                )
                # Compiler response syntax, independent of the launcher's writer:
                # preserve newline/tab escapes, literal backslashes, both quote
                # characters, spaces, and a backslash at the end of the string.
                (build / "input.rsp").write_text(
                    r'''"-DMSG=\"a\\nb\\tC:\\\\tmp\\\\file\\\"it's quoted\\\" \\\\\""'''
                    + '\n-c\n"'
                    + str(source / "probe.c")
                    + '"\n-o\nprobe.o\n'
                )
                expected = b'a\nb\tC:\\tmp\\file"it\'s quoted" \\'

                def output():
                    subprocess.run(
                        [compiler, "probe.o", "-o", "probe"],
                        cwd=build,
                        check=True,
                        capture_output=True,
                    )
                    return subprocess.check_output([str(build / "probe")])

                # Confirm the input's meaning with the compiler itself first.
                subprocess.run(
                    [compiler, "@input.rsp"], cwd=build, check=True, capture_output=True
                )
                self.assertEqual(output(), expected)
                command = [
                    sys.executable,
                    str(LAUNCHER),
                    str(source),
                    str(build),
                    shutil.which("ccache"),
                    shutil.which(compiler),
                    "@input.rsp",
                ]
                for _ in range(2):
                    subprocess.run(
                        command, cwd=build, env=env, check=True, capture_output=True
                    )
                    self.assertEqual(output(), expected)
                self.assertEqual(self.stats(env)["direct_cache_hit"], 1)

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
