echo "prepare the environment for building hogd"

set -ex

echo 'setting up cargo'
source ./scripts/ci/setup_cargo.sh

echo 'setting up ccache'
ln -s "$USER_HOME/.cache" "$HOME/.cache"

echo 'installing cbindgen'
cargo install --force cbindgen

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

echo 'set gcc-14 in check'
echo "GCC_CC=gcc-14 GCC_CXX=g++-14" >./scripts/conf.sh && chmod u+x ./scripts/conf.sh
