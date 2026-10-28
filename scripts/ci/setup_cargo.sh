set -x
echo 'setting up cargo'

# The repo no longer checks in a .cargo directory, but a job directory left
# over from an earlier run still holds the symlink planted below, and linking
# onto an existing symlink-to-a-directory would nest the link inside it.
rm -rf "${HOME}/.cargo"

# curl -sSf https://sh.rustup.rs | sh -s -- -y
ln -s "$USER_HOME/.cargo" "$HOME/.cargo"
ln -s "$USER_HOME/.rustup" "$HOME/.rustup"
source "$HOME/.cargo/env"

rustup toolchain list
