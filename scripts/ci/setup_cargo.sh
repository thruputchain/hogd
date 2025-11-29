echo 'setting up cargo'

# curl -sSf https://sh.rustup.rs | sh -s -- -y
ln -s "$USER_HOME/.cargo" "$HOME/.cargo"
source "$HOME/.cargo/env"

