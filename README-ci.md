# CI runner setup instructions
- Create the hogci home directory on the machine in question
- Follow the instructions to clone and register the runner as they appear on Github.
	- Make sure to add the runner overall to the groundhogchain organization, not an individual repository
	- Make sure that when you register the runner, register it with any tags you want to add (this can be done later
	by deregistering and re-adding the runner).  Probably should at least add the machine name as a tag.
- Copy the svc_nosudo.sh script from an existing runner, and edit the filenames/machine names at the top of that file
to the appropriate values for that machine.
- Edit the `bin/actions.runner.service.template` file: At the bottom, replate `multi-user.target` with `default.target`.
  This makes the machine relaunch the service after rebooting.
- `./svc_nosudo.sh install` (may need to mkdir the appropriate ~/config subdirectories) 
- Add github to the ssh known-hosts (otherwise the runner will fail to clone anything from github).
- Create a rust installation for the hogci account (much of the config for rust is kept locally)
	- curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
	- Follow the default instructions there
	- rustup +nightly target add wasm32-unknown-unknown
		- You might need to install the target with and without nightly (depending on whether the smart contracts will be built with or without nightly; that may change in the future).
	- `rustup component add rust-src --toolchain nightly-x86_64-unknown-linux-gnu`
	- `rustup component add rust-src --toolchain stable-x86_64-unknown-linux-gnu`
	- `cargo install --force cbindgen`
	- You do not need to add the ./cargo/env to a .bashrc for the runner; the CI scripts add, if necessary, .cargo/bin to the path
- Allow services to run after the shell closes.  From a sudoer account, `sudo loginctl enable-linger hogci`
- `./svc_nosudo.sh start`

