# CI Debug information

- Core dumps are saved to `../core_dumps`
	- On the hogN machines: this is `$WORK_DIR/hogd/core_dumps`.  Usually `$WORK_DIR=/home/hogci/actions-runner/_work`, although on `hog11` this is `/home/hogci/general_runner/_work`

# Cloning on the hog hosts

Every job in `scripts/schedules/*.toml` starts by cloning `hogd` onto each hog
host it holds a lock for. All of those hosts run their `git fetch` in the same
instant, and `hog_sched` releases queued jobs in a batch, so a backlog draining
means several jobs' worth of hosts open SSH sessions to `github.com` within a
few seconds of each other. GitHub caps concurrent SSH sessions and drops the
excess:

```
Connection closed by 140.82.116.3 port 22
fatal: Could not read from remote repository.

Please make sure you have the correct access rights
and the repository exists.
```

That is a transient rejection, not an auth failure -- the hosts that get
through are using the same deploy key -- but it exits 128, which kills the
whole run before a single line is built. Which hosts get dropped varies from
run to run, so a failure like this is *not* evidence that a particular machine
is unhealthy.

The clone preamble therefore routes its network-facing git steps (`git fetch`
and `git submodule update`, the latter fanning out to every submodule) through
a `hog_git_retry` helper: five retries with linear backoff, offset by `$$ % 7`
so hosts that were dropped together do not retry in lockstep. A dropped
connection costs seconds instead of the run. Keep new clone preambles going
through `hog_git_retry` for the same reason.

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

