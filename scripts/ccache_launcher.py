#!/usr/bin/env python3
"""Give GCC/Clang and ccache stable paths without moving the build tree.

Invoked by CMake as: python3 ccache_launcher.py SOURCE BUILD CCACHE COMPILER ...
Each compilation directory gets private source/build symlinks. Keeping the
working directory unchanged lets compiler depfiles work with both Make and Ninja.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ALIAS_DIR = ".hogd-cache"
PATH_OPTIONS = (
    "-iwithprefixbefore",
    "-iwithprefix",
    "-idirafter",
    "-isystem",
    "-isysroot",
    "-iquote",
    "-include-pch",
    "-include",
    "-imacros",
    "-iprefix",
    "-I",
    "-B",
)
OUTPUT_OPTIONS = ("-o", "-MF", "-MT", "-MQ", "-MJ")


def canonical_compiler(compiler, ccache):
    # ccache hashes the driver name even when compiler_check=content. CMake's
    # generic cc/c++ drivers should share entries with explicit GCC/Clang builds.
    # Check identity instead of resolving every symlink: clang++ may resolve to
    # clang, but its name selects C++ semantics. Preserve distinct wrappers and
    # toolchains that do not provide an identical sibling driver.
    path = Path(shutil.which(compiler) or compiler)
    try:
        # In ccache's masquerade mode, cc and gcc can both be links to ccache
        # while dispatching to different compilers according to their names.
        if path.samefile(shutil.which(ccache) or ccache):
            return compiler
    except OSError:
        return compiler
    candidates = {"cc": ("gcc", "clang"), "c++": ("g++", "clang++")}
    for name in candidates.get(path.name, ()):
        candidate = path.with_name(name)
        try:
            if path.samefile(candidate):
                return str(candidate)
        except OSError:
            continue
    return compiler


def quote_response_argument(argument):
    # Response files are parsed by the compiler, not a shell. In particular,
    # single quotes do not reliably protect backslashes as shlex.quote assumes.
    return '"' + argument.replace("\\", "\\\\").replace('"', '\\"') + '"'


def ensure_alias(path, target):
    try:
        path.symlink_to(target, target_is_directory=True)
    except FileExistsError:
        if not path.is_symlink() or os.readlink(path) != target:
            raise RuntimeError(f"{path} already exists with a different target")


def expand_response_files(arguments, depth=0):
    if depth > 32:
        raise RuntimeError("compiler response files are nested too deeply")
    result = []
    for argument in arguments:
        if argument.startswith("@"):
            with open(argument[1:]) as response:
                result.extend(
                    expand_response_files(shlex.split(response.read()), depth + 1)
                )
        else:
            result.append(argument)
    return result


def normalize(arguments, roots):
    def path_argument(value):
        # GCC expands these against --sysroot; they are not cwd-relative paths.
        if value.startswith(("=", "$SYSROOT")):
            return value
        absolute = os.path.abspath(value)
        for root, alias in roots:
            if absolute == root:
                return alias
            if absolute.startswith(root + os.sep):
                return alias + absolute[len(root) :]
        return value

    result = []
    arguments = iter(arguments)
    for argument in arguments:
        if argument in OUTPUT_OPTIONS:
            # Preserve output names and depfile targets as requested by CMake.
            result.extend((argument, next(arguments)))
        elif argument in PATH_OPTIONS:
            result.extend((argument, path_argument(next(arguments))))
        elif argument.startswith("--sysroot="):
            result.append("--sysroot=" + path_argument(argument[len("--sysroot=") :]))
        elif argument.startswith("-"):
            for option in PATH_OPTIONS:
                if (
                    argument.startswith(option)
                    and len(argument) > len(option)
                    and argument[len(option)] != "-"
                ):
                    argument = option + path_argument(argument[len(option) :])
                    break
            result.append(argument)
        else:
            # Source files, including relative ones from Make's subdirectories.
            result.append(
                path_argument(argument)
                if os.path.isabs(argument) or os.path.isfile(argument)
                else argument
            )
    return result


def main():
    source, build, ccache, compiler, *arguments = sys.argv[1:]
    compiler = canonical_compiler(compiler, ccache)
    cwd = os.getcwd()
    alias_dir = Path(ALIAS_DIR)
    alias_dir.mkdir(exist_ok=True)
    ensure_alias(alias_dir / "source", source)
    ensure_alias(alias_dir / "build", build)
    roots = sorted(
        [(build, ALIAS_DIR + "/build"), (source, ALIAS_DIR + "/source")],
        key=lambda item: len(item[0]),
        reverse=True,
    )
    use_response = any(argument.startswith("@") for argument in arguments)
    arguments = normalize(expand_response_files(arguments), roots)

    # Source/header arguments already use stable relative aliases. Normalize
    # the remaining absolute path: the compilation directory in debug info.
    arguments = [f"-ffile-prefix-map={cwd}=."] + arguments

    # Paths are already normalized. BASEDIR would resolve our symlinks and
    # reintroduce layout-dependent relative paths. Other cache settings remain
    # configurable, including the shared storage and namespace.
    os.environ["CCACHE_BASEDIR"] = ""
    os.environ.setdefault("CCACHE_COMPILERCHECK", "content")
    if use_response:
        # Keep response-file builds below ARG_MAX after normalization too.
        with tempfile.NamedTemporaryFile(
            mode="w", dir=alias_dir, suffix=".rsp"
        ) as response:
            response.write(
                "\n".join(quote_response_argument(argument) for argument in arguments)
            )
            response.flush()
            status = subprocess.call([ccache, compiler, "@" + response.name])
        return status if status >= 0 else 128 - status
    os.execv(ccache, [ccache, compiler, *arguments])


if __name__ == "__main__":
    sys.exit(main())
