set -x
echo 'setting up cargo'

# Clear anything sitting at $HOME/.cargo so the link below lands. The repo
# used to check a .cargo in; it no longer does, so -f, or the missing
# directory fails the set -e of the script that sources this one.
rm -rf "${HOME}/.cargo"

# curl -sSf https://sh.rustup.rs | sh -s -- -y
ln -s "$USER_HOME/.cargo" "$HOME/.cargo"
ln -s "$USER_HOME/.rustup" "$HOME/.rustup"
source "$HOME/.cargo/env"

rustup toolchain list
