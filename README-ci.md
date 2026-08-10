# CI Debug information

- Core dumps are saved to `../core_dumps`
	- On the hogN machines: this is `$WORK_DIR/hogd/core_dumps`.  Usually `$WORK_DIR=/home/hogci/actions-runner/_work`, although on `hog11` this is `/home/hogci/general_runner/_work`

- `check_bench_hosts: 1 of 8 hosts cannot run combined_metrics.toml: hog5` means
  the named host answered ssh but gave the session no forwarded ssh-agent. Every
  `hog_sched` job begins with a prologue that runs `hog_fwd`, which needs that
  agent to map the private groundhogchain repos to an identity, so such a host
  fails every job it is given with `Could not open a connection to your
  authentication agent.` and takes the schedule down with it. It is a fault on
  that host, not in the commit under test; `ssh -A <host> ssh-add -l` reproduces
  it, and the host needs an operator (agent forwarding disabled in its `sshd`
  config, or a `/tmp` too full for `sshd` to create the forwarding socket, will
  both do it). `scripts/ci/check_bench_hosts.sh` reports this before a run
  starts, since afterwards the one line that says so is buried among the ~800
  the eight hosts emit while forwarding keys.

# CI runner setup instructions
- Create the hogci home directory on the machine in question. You should create it at `/home/local/hogci` and then create a symlink from `/home/hogci` to `/home/local/hogci`: `ln -s /home/hogci /home/local/hogci`.
- Log in as the `hogci` user before completing the remaining steps in this file. You should add you SSH public key to `~hogci/.ssh/authorized_keys` and then ssh in as `hogci`.
- Follow the instructions to clone and register the runner as they appear on Github.
	- Make sure to add the runner overall to the groundhogchain organization, not an individual repository
	- Make sure that when you register the runner, register it with any tags you want to add (this can be done later
	by deregistering and re-adding the runner).  Probably should at least add the machine name as a tag.
- Copy the svc_nosudo.sh script from an existing runner, and edit the filenames/machine names at the top of that file
to the appropriate values for that machine.
- Edit the `bin/actions.runner.service.template` file: At the bottom, replate `multi-user.target` with `default.target`.
  This makes the machine relaunch the service after rebooting.
- `./svc_nosudo.sh install` (may need to mkdir the appropriate ~/config subdirectories) 
- Add github to the ssh known-hosts (otherwise the runner will fail to clone anything from github). You can do this by running `ssh git@github.com` and then choosing to trust the remote host.
- Create a rust installation for the hogci account (much of the config for rust is kept locally)
	- `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`
	- Follow the default instructions there
	- `cargo install --force cbindgen`
	- You do not need to add the ./cargo/env to a .bashrc for the runner; the CI scripts add, if necessary, .cargo/bin to the path
	- The CI scripts will, where relevant, fetch any dependencies or toolchain components
- Allow services to run after the shell closes.  From a sudoer account, `sudo loginctl enable-linger hogci`
- `./svc_nosudo.sh start`

