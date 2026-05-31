echo "prepare the environment for building hogd"

set -ex

echo 'setting up cargo'
source ./scripts/ci/setup_cargo.sh

echo 'setting up ccache'
ln -s "$USER_HOME/.cache" "$HOME/.cache"

CBINDGEN_VERSION=0.29.2
if command -v cbindgen >/dev/null 2>&1 && cbindgen --version | grep -qx "cbindgen ${CBINDGEN_VERSION}"; then
  echo "cbindgen ${CBINDGEN_VERSION} already installed"
else
  echo "installing cbindgen ${CBINDGEN_VERSION}"
  cargo install --locked --force --version "${CBINDGEN_VERSION}" cbindgen
fi

echo 'gcc version check'
gcc --version

echo 'clang version check'
clang --version

# echo 'add cargo to PATH'
# echo "$HOME/.cargo/bin" >> $GITHUB_PATH #XXX

echo 'path check'
echo $PATH

echo 'cargo version check'
cargo --version

echo 'cargo fetch'
CARGO_NET_GIT_FETCH_WITH_CLI=true ./scripts/fetch.sh

echo 'set gcc in check'
echo "GCC_CC=gcc GCC_CXX=g++" >./scripts/conf.sh && chmod u+x ./scripts/conf.sh
