set -x
echo 'setting up cargo'

# Get rid of any .cargo in the job directory before symlinking the real one
# into place. The repo no longer checks one in, so it is usually absent; -f
# keeps that from failing the sourcing script, which runs under `set -e`.
rm -rf ${HOME}/.cargo

# curl -sSf https://sh.rustup.rs | sh -s -- -y
ln -s "$USER_HOME/.cargo" "$HOME/.cargo"
ln -s "$USER_HOME/.rustup" "$HOME/.rustup"
source "$HOME/.cargo/env"

rustup toolchain list
