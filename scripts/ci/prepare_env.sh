echo "prepare the environment for building hogd" >&2

echo 'setting up cargo' >&2
source ./scripts/ci/setup_cargo.sh

echo 'gcc version check' >&2
gcc --version

echo 'clang version check' >&2
clang --version

# echo 'add cargo to PATH'
# echo "$HOME/.cargo/bin" >> $GITHUB_PATH #XXX

echo 'path check' >&2
echo $PATH

echo 'cargo version check' >&2
cargo --version

echo 'cargo fetch' >&2
CARGO_NET_GIT_FETCH_WITH_CLI=true ./scripts/fetch.sh

echo 'set gcc-14 in check' >&2
echo "GCC_CC=gcc-14 GCC_CXX=g++-14" >./scripts/conf.sh && chmod u+x ./scripts/conf.sh
