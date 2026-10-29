set -x
echo 'setting up cargo'

# curl -sSf https://sh.rustup.rs | sh -s -- -y
# -n replaces a link left by an earlier run instead of nesting a new one in it.
ln -sfn "$USER_HOME/.cargo" "$HOME/.cargo"
ln -sfn "$USER_HOME/.rustup" "$HOME/.rustup"
source "$HOME/.cargo/env"

rustup toolchain list
