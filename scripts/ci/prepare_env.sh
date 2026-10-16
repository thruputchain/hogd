echo "prepare the environment for building hogd"

set -ex

echo 'setting up cargo'
source ./scripts/ci/setup_cargo.sh

# Fetch private workspace sources before any potentially lengthy tool builds.
echo 'cargo fetch'
CARGO_NET_GIT_FETCH_WITH_CLI=true ./scripts/fetch.sh || return $?

echo 'setting up ccache'
ln -s "$USER_HOME/.cache" "$HOME/.cache"

CBINDGEN_VERSION=0.29.2
if command -v cbindgen >/dev/null 2>&1 && cbindgen --version | grep -qx "cbindgen ${CBINDGEN_VERSION}"; then
  echo "cbindgen ${CBINDGEN_VERSION} already installed"
else
  echo "installing cbindgen ${CBINDGEN_VERSION}"
  cargo install --locked --force --version "${CBINDGEN_VERSION}" cbindgen
fi

# Benchmark jobs use a stable toolchain; cache warmup can follow the host default.
ci_gcc_cc="${HOGD_CI_GCC_CC:-gcc-15}"
ci_gcc_cxx="${HOGD_CI_GCC_CXX:-g++-15}"

echo "$ci_gcc_cc version check"
"$ci_gcc_cc" --version

echo "$ci_gcc_cxx version check"
"$ci_gcc_cxx" --version

echo 'clang version check'
clang --version

# echo 'add cargo to PATH'
# echo "$HOME/.cargo/bin" >> $GITHUB_PATH #XXX

echo 'path check'
echo $PATH

echo 'cargo version check'
cargo --version

echo "set GCC compilers: $ci_gcc_cc / $ci_gcc_cxx"
printf 'GCC_CC=%q\nGCC_CXX=%q\n' "$ci_gcc_cc" "$ci_gcc_cxx" >./scripts/conf.sh && chmod u+x ./scripts/conf.sh
