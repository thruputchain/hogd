# Convenience wrapper around scripts/. The scripts stay authoritative; every
# target here just invokes one with this repo's conventions applied.
#
#   make config     first-time (or post-pull) setup: submodules, cargo deps, cmake
#   make devnode    build just the hog-devnode executable
#   make build      build everything
#   make help       list every target
#
# Build parallelism is derived by scripts/build-jobs.sh from performance cores
# and RAM. Do not pass -j to make expecting it to reach the compiler; set
# BUILD_JOBS instead, and only with a reason.

SHELL := /bin/bash
# Ignored by the GNU make 3.81 Apple ships; every recipe below is one command
# per line, so failures stop the build with or without it.
.SHELLFLAGS := -eu -o pipefail -c
.DEFAULT_GOAL := help

# Recipes drive one build system each; running them concurrently only fights
# over the same build directory.
.NOTPARALLEL:

BUILD_DIR ?= build
BUILD_TYPE ?= Release
# make install destination; DESTDIR stages under another root for packaging.
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=
# 'gcc' or 'clang'; empty leaves the choice to scripts/env.sh.
COMPILER ?=
# Extra targets for `make build`, e.g. `make build TARGETS="hog-gateway scpd"`.
TARGETS ?=
BUILD_JOBS ?=

# Homebrew rolls its unversioned llvm and lld formulas independently of the
# clang-21 this repo targets, and scripts/macos-env.sh points CC/CXX at the
# rolling keg. Set these to pin a toolchain without editing the tracked env
# script, e.g.
#   make config LLVM_PREFIX=/opt/homebrew/opt/llvm@21 LLD_PREFIX=/opt/homebrew/opt/lld@21
# Both are needed together: CMakeLists.txt links with -fuse-ld=lld, so a
# clang-21 object tree handed to a newer lld runs ThinLTO through the wrong
# LLVM backend and aborts with "Broken module found" at link time.
#
# Pass them to config, not to every build: config records the compiler and the
# linker in the cmake cache, after which plain `make build` stays on them.
LLVM_PREFIX ?=
LLD_PREFIX ?=

ifeq ($(shell uname -s),Darwin)
LLD_BIN := ld64.lld
else
LLD_BIN := ld.lld
endif

ifneq ($(LLVM_PREFIX),)
export CC := $(LLVM_PREFIX)/bin/clang
export CXX := $(LLVM_PREFIX)/bin/clang++
export PATH := $(LLVM_PREFIX)/bin:$(PATH)
endif
ifneq ($(LLD_PREFIX),)
export PATH := $(LLD_PREFIX)/bin:$(PATH)
endif

BUILD_FLAGS := -d $(BUILD_DIR) -t $(BUILD_TYPE)
ifneq ($(COMPILER),)
BUILD_FLAGS += -c $(COMPILER)
endif
ifneq ($(BUILD_JOBS),)
BUILD_FLAGS += -j $(BUILD_JOBS)
endif

# cmake caches the compiler path but never the linker: -fuse-ld=lld is resolved
# from PATH on every link, so an exported PATH only holds for the invocation
# that set it. --ld-path names the binary outright and lives in the cache,
# which makes the pin survive later builds. It also outranks -fuse-ld=lld in
# the clang driver, so the repo's own link option needs no change.
CONFIG_FLAGS := $(BUILD_FLAGS)
ifneq ($(LLD_PREFIX),)
CONFIG_FLAGS += -x -DCMAKE_EXE_LINKER_FLAGS=--ld-path=$(LLD_PREFIX)/bin/$(LLD_BIN)
CONFIG_FLAGS += -x -DCMAKE_SHARED_LINKER_FLAGS=--ld-path=$(LLD_PREFIX)/bin/$(LLD_BIN)
endif

DEVNODE_BIN := $(BUILD_DIR)/hogd/devnode/hog-devnode
VERSION_C := $(BUILD_DIR)/hogd/version.c
# An unbalanced literal paren inside $(shell ...) breaks make's own parsing, so
# match the project line with grep and pull the version out separately.
PROJECT_VERSION := $(shell grep -m1 '^project' CMakeLists.txt | sed -n 's/.*VERSION \([0-9][0-9A-Za-z.-]*\).*/\1/p')

.PHONY: help
help:
	@echo "Setup"
	@echo "  config        submodules + cargo deps + cmake configure ($(BUILD_DIR), $(BUILD_TYPE))"
	@echo "  submodules    git submodule update --init --recursive (needs an SSH agent)"
	@echo "  fetch         scripts/fetch.sh, the cargo side of the tree"
	@echo "  reconfigure   discard the cmake cache and configure again"
	@echo ""
	@echo "Build"
	@echo "  build         everything; add TARGETS=\"a b\" to narrow"
	@echo "  devnode       just $(DEVNODE_BIN)"
	@echo ""
	@echo "Verify"
	@echo "  check         build, lint, and test the way CI does"
	@echo "  test          ctest in $(BUILD_DIR)"
	@echo "  lint          clang-tidy over hogd/"
	@echo "  fmt           rewrite formatting; fmt-check reports instead"
	@echo ""
	@echo "  version       what a build here embeds, and what the tree already has"
	@echo ""
	@echo "Install"
	@echo "  install       copy every built hog-* binary to $(BINDIR)"
	@echo ""
	@echo "Clean"
	@echo "  clean         build system clean inside $(BUILD_DIR)"
	@echo "  distclean     remove $(BUILD_DIR) outright"
	@echo ""
	@echo "Variables: BUILD_DIR BUILD_TYPE COMPILER TARGETS BUILD_JOBS LLVM_PREFIX LLD_PREFIX"

# Submodules come first because a fresh clone cannot configure without them,
# and several remotes are SSH-only, so this fails early and loudly when no
# agent is running rather than midway through cmake.
.PHONY: submodules
submodules:
	git submodule update --init --recursive

.PHONY: fetch
fetch:
	./scripts/fetch.sh

.PHONY: config
config: submodules fetch
	./scripts/build.sh -n $(CONFIG_FLAGS)

# A cmake cache records absolute toolchain paths, including versioned Homebrew
# keg directories that disappear on upgrade. Removing CMakeFiles/ as well is
# required: the compiler-detection files there are reused even when the cache
# is gone, and they carry the same stale paths.
.PHONY: reconfigure
reconfigure:
	rm -rf $(BUILD_DIR)/CMakeCache.txt $(BUILD_DIR)/CMakeFiles
	./scripts/build.sh -n $(CONFIG_FLAGS)

.PHONY: build
build:
	./scripts/build.sh $(BUILD_FLAGS) $(TARGETS)

# hog_devnode without the hyphen is the static library, not the binary.
.PHONY: devnode
devnode:
	./scripts/build.sh $(BUILD_FLAGS) hog-devnode
	@echo "export THRUPUT_DEVNODE_BINARY=$(abspath $(DEVNODE_BIN))"

# Reports rather than computes: cmake/write_version_c.cmake owns the real
# resolution (HOGD_DISTRIBUTION_VERSION, else an exact tag, else the cmake
# project version) and re-runs on every build, so the embedded block below is
# the authority and the source block only shows what feeds it.
#
# The dirty check refreshes the index first, as that cmake script does. Without
# it a clean tree reads as dirty wherever stat data does not match the index,
# which is every bind-mounted container and every fresh clone. The binary line
# runs the binary rather than testing -x, so a build for another platform is
# reported as absent instead of erroring.
.PHONY: version
version:
	@echo "source tree"
	@echo "  cmake project              $(PROJECT_VERSION)"
	@echo "  exact tag                  $$(git describe --tags --exact-match 2>/dev/null || echo '(none)')"
	@echo "  HOGD_DISTRIBUTION_VERSION  $${HOGD_DISTRIBUTION_VERSION:-(unset)}"
	@echo "  commit                     $$(git rev-parse --short=12 HEAD)$$(git update-index --refresh >/dev/null 2>&1; git diff-index --quiet HEAD -- 2>/dev/null || echo '+dirty')"
	@echo "  protocol submodule         $$(git -C protocol rev-parse --short=12 HEAD 2>/dev/null || echo '(not checked out)')"
	@if [ -f $(VERSION_C) ]; then \
		echo "embedded in $(BUILD_DIR)"; \
		sed -n 's/^const char hogd_version\[\] = "\(.*\)";/  version                    \1/p;s/^const char hogd_commit\[\] = "\(.*\)";/  commit                     \1/p;s/^const char protocol_commit\[\] = u8"\(.*\)";/  protocol                   \1/p' $(VERSION_C); \
	else \
		echo "embedded in $(BUILD_DIR): nothing built yet"; \
	fi
	@if out=$$($(DEVNODE_BIN) --version 2>/dev/null); then \
		echo "binaries"; \
		echo "  $$out"; \
	fi

# Installs by convention rather than from a list: every executable this repo
# ships is named hog-*, so nothing here needs updating when one is added. The
# repo has no cmake install() rules for these targets, so copy them directly.
# Deliberately does not depend on build - an unbuilt tree should say so, not
# start a half-hour compile from an install command.
.PHONY: install
install:
	@bins=$$(find $(BUILD_DIR) -type f -perm -111 -name 'hog-*' -not -path '*/CMakeFiles/*' 2>/dev/null | sort); \
	if [ -z "$$bins" ]; then \
		echo "no hog-* binaries in $(BUILD_DIR); run 'make build' first" >&2; \
		exit 1; \
	fi; \
	mkdir -p "$(DESTDIR)$(BINDIR)"; \
	for b in $$bins; do \
		install -m 755 "$$b" "$(DESTDIR)$(BINDIR)/" || exit 1; \
		echo "  $(DESTDIR)$(BINDIR)/$$(basename $$b)"; \
	done

.PHONY: test
test:
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# scripts/check.sh is this repo's own build-plus-tidy-plus-test run, so check
# calls it rather than chaining fmt-check, lint and test as the sibling repos do.
.PHONY: check
check:
	./scripts/check.sh

.PHONY: lint
lint:
	./scripts/tidy.sh -d $(BUILD_DIR)

.PHONY: fmt
fmt:
	./scripts/format.sh

.PHONY: fmt-check
fmt-check:
	./scripts/format.sh --check

.PHONY: clean
clean:
	@if [ -d $(BUILD_DIR) ]; then \
		cmake --build $(BUILD_DIR) --target clean; \
	else \
		echo "no $(BUILD_DIR) to clean"; \
	fi

.PHONY: distclean
distclean:
	rm -rf $(BUILD_DIR)
