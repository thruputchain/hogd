# CI Debug information

- Core dumps are saved to `../core_dumps`
	- On the hogN machines: this is `$WORK_DIR/hogd/core_dumps`.  Usually `$WORK_DIR=/home/hogci/actions-runner/_work`, although on `hog11` this is `/home/hogci/general_runner/_work`

# CI runner setup instructions

## Private source authentication

CI calls `.github/actions/private-sources` after checking out hogd with
`persist-credentials: false`, before initializing submodules or fetching Cargo
dependencies. Set repository variable `COMPONENT_READER_APP_ID` and secret
`COMPONENT_READER_PRIVATE_KEY` for the `thruput-component-reader` GitHub App.
Its installation needs Contents: read on hogd, chuckd, hogx, thruput-protocol,
thruput-protocol-go, thruput-protocol-rs, lib-go, thruput-go,
thruput-integration-test-contracts, scp, and thruput-sdk-rs. No thruput-tools
source reference is present in this tree, so it is outside the requested token
scope.

The action maps legacy and canonical repository URLs to authenticated HTTPS.
It creates a private config under `RUNNER_TEMP` and exports `GIT_CONFIG_GLOBAL`,
so neither the App token nor its rewrites enter the runner's `~/.gitconfig`.
Each caller removes the config in an `always()` cleanup step; the token action
revokes the installation token at job completion. Existing pre-checkout
`.gitconfig` cleanup steps still remove stale runner configuration. Cargo uses
the Git CLI. Go module paths remain unchanged; this tree has no go.mod/go.work
or workflow GOPRIVATE/GOFLAGS settings to migrate.

Deploy-key inputs are retained only for repositories still in groundhogchain:
abort_rs (`HOGD_TO_ABORT_RS_KEY`), bcs (`HOGD_TO_BCS_ACCESS_KEY`),
metrics_history (`HOGD_TO_METRICS_HISTORY_KEY`), serde_arrays
(`HOGD_TO_SERDE_ARRAYS`), xdr_common (`HOGD_TO_XDR_COMMON_ACCESS_KEY`), xdr_rpc
(`HOGD_TO_XDR_RPC`), and xdrs (`HOGD_XDRS_ACCESS_KEY`). metrics_history is fetched
and written by the metrics scripts. The other six keys preserve access to
nested private sources: their manifests are unavailable in an uninitialized
checkout, so qualify their continued necessity in CI before deleting secrets.
The obsolete bcs_rs key and all transferred-repository keys, including the hogd
self key, are no longer referenced. Do not delete stored secrets until CI is
qualified.

Release publication and protocol-rs workflow dispatch keep their existing
write credentials. The read-only component token never rewrites metrics_history.
The hog_sched lanes require qualification that worker processes inherit
`GIT_CONFIG_GLOBAL` and can access its job-local file, alongside the existing
SSH agent. Remote workers cannot use a runner-local path; do not copy tokens
into persistent home configuration to accommodate them. Long-running fetches
also need qualification against the installation token's expiry.

## Runner provisioning
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
