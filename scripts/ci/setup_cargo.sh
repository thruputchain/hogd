echo 'setting up cargo'

# curl -sSf https://sh.rustup.rs | sh -s -- -y
ln -s "$USER_HOME/.cargo" "$HOME/.cargo"
. "$HOME/.cargo/env"

rustup default stable
