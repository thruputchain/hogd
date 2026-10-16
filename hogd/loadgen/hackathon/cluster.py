#!/usr/bin/env python3
"""Run the hackathon corpus on one real validator, locally or on CI hosts."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import time
import urllib.request
import urllib.error

BENCHMARK_DIR = Path(__file__).resolve().parent
ROOT = BENCHMARK_DIR.parents[2]
HOSTS = {
    "coordinator": "hog4",
    "gateway": "hog4",
    "exec0": "hog5",
    "exec1": "hog6",
    "replay": "hog7",
    "bootstrap": "hog7",
    "auth": "hog8",
    "event": "hog9",
    "indexer": "hog9",
    "mempool": "hog10",
    "db": "hog11",
}
BINS = {
    "mempool": "hogd/hog_mempool/hog-mempool",
    "db": "hogd/hog_db/hog-db",
    "auth": "hogd/hog_auth/hog-auth",
    "event": "hogd/hog_event/hog-event",
    "indexer": "hogd/hog_indexer/hog-indexer",
    "exec0": "hogd/hog_exec/hog-exec",
    "exec1": "hogd/hog_exec/hog-exec",
    "coordinator": "chuckd.dev",
    "replay": "hackathon-replay",
    "bootstrap": "hackathon-replay",
}


class Cluster:
    def __init__(self, args, directory):
        self.args, self.directory = args, directory
        self.processes, self.logs = {}, {}
        self.hosts = (
            {name: "127.0.0.1" for name in HOSTS} if args.local else HOSTS
        )
        self.control = args.build / "hogd/hog_exec/bench/control_cli"
        self.port = str(args.control_port)
        self.ips = {}
        for host in set(self.hosts.values()):
            self.ips[host] = (
                "127.0.0.1"
                if args.local
                else subprocess.check_output(
                    [
                        "bash",
                        "-c",
                        'source "$1/scripts/hog-ip.sh"; hog_ip "$2"',
                        "bash",
                        str(ROOT),
                        host,
                    ],
                    text=True,
                ).strip()
            )

    def address(self, name, port):
        return f"{self.ips[self.hosts[name]]}:{port}"

    def remote(self, host, argv, **kwargs):
        command = list(map(str, argv))
        if not self.args.local:
            # hog_sched supplies its shell script on stdin through a PTY. A
            # Nested ssh must not consume the remaining script, including exit.
            command = [
                "ssh",
                "-n",
                "-oBatchMode=yes",
                "-oConnectTimeout=5",
                host,
                shlex.join(command),
            ]
        kwargs.setdefault("check", True)
        return subprocess.run(command, **kwargs)

    def stage(self, source, host, destination):
        self.remote(host, ["mkdir", "-p", str(destination.parent)])
        if self.args.local:
            if source.resolve() != destination.resolve():
                if source.is_dir():
                    shutil.copytree(source, destination)
                else:
                    shutil.copyfile(source, destination)
        else:
            subprocess.run(
                [
                    "scp",
                    "-q",
                    "-r",
                    "-oBatchMode=yes",
                    str(source),
                    f"{host}:{shlex.quote(str(destination))}",
                ],
                check=True,
            )

    def start(self, name, argv):
        host = self.hosts[name]
        command = list(map(str, argv))
        if self.args.local:
            output = self.directory / f"{name}.stdout.log"
            errors = self.directory / f"{name}.stderr.log"
            out, err = output.open("wb"), errors.open("wb")
            try:
                self.processes[name] = subprocess.Popen(
                    command, stdout=out, stderr=err, start_new_session=True
                )
            finally:
                out.close()
                err.close()
            self.logs[name] = (host, str(output), str(errors))
        else:
            # A separate session includes children such as scpd in our cleanup.
            # exec preserves the PID controlled by control_server. Recording it
            # lets us monitor every service, including colocated services that
            # its most-recent-process status RPC cannot individually query.
            pidfile = self.directory / f"{name}.pid"
            command = [
                "/usr/bin/setsid",
                "/bin/bash",
                "-c",
                'printf "%s\\n" "$$" > "$1"; shift; exec "$@"',
                "bash",
                str(pidfile),
                *command,
            ]
            result = subprocess.check_output(
                [str(self.control), host, self.port, "start", *command],
                text=True,
            )
            paths = [
                re.search(rf"CONTROL_{stream}_PATH=(.*)", result)
                for stream in ("STDOUT", "STDERR")
            ]
            if not all(paths):
                raise RuntimeError(
                    f"missing control log paths for {name}: {result}"
                )
            self.logs[name] = (host, *(p[1].strip() for p in paths))
            # start returns after fork, before the child necessarily writes its
            # PID. Wait for that handoff before checking process health.
            self.remote(
                host,
                [
                    "bash",
                    "-c",
                    'for ((i=0; i<100; i++)); do [[ -s "$1" ]] && exit 0; '
                    'sleep 0.05; done; exit 1',
                    "bash",
                    pidfile,
                ],
                timeout=15,
            )
        print(f"started {name} on {host}", flush=True)

    def check_services(self):
        if self.args.local:
            for name, proc in self.processes.items():
                if name != "replay" and proc.poll() is not None:
                    raise RuntimeError(
                        f"{name} exited: "
                        f"{Path(self.logs[name][2]).read_text()[-4000:]}"
                    )
        else:
            for host in set(self.hosts.values()):
                files = [
                    self.directory / f"{name}.pid"
                    for name, (running_host, *_) in self.logs.items()
                    if running_host == host and name != "replay"
                ]
                if files:
                    self.remote(
                        host,
                        [
                            "bash",
                            "-c",
                            'for file do read -r pid < "$file" && '
                            'kill -0 "$pid" 2>/dev/null || '
                            '{ echo "service exited: $file" >&2; exit 1; }; '
                            'done',
                            "bash",
                            *files,
                        ],
                        timeout=15,
                    )

    def wait_log(self, name, marker):
        deadline = time.monotonic() + self.args.ready_timeout
        next_check = 0
        host, stdout, stderr = self.logs[name]
        while time.monotonic() < deadline:
            if time.monotonic() >= next_check:
                self.check_services()
                next_check = time.monotonic() + 5
            match = self.remote(
                host,
                ["grep", "-F", "-m", "1", "--", marker, stdout, stderr],
                stdout=subprocess.DEVNULL,
                check=False,
            )
            if match.returncode == 0:
                return
            time.sleep(0.5)
        raise RuntimeError(f"readiness timeout: {name}: {marker}")

    def stop(self):
        # Stop only this run's local process groups or its allocated CI control
        # servers. Never killall on unrelated hosts or clear their work dirs.
        if self.args.local:
            for proc in reversed(list(self.processes.values())):
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            deadline = time.monotonic() + 10
            for proc in self.processes.values():
                try:
                    proc.wait(timeout=max(0.1, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        else:
            # control_server stops only its direct children. In particular, a
            # crashed coordinator can leave scpd listening on the next run's
            # port. Signal our sessions, including their descendants.
            for host in set(self.hosts.values()):
                self.signal_remote_groups(host, "TERM")
            for host in set(self.hosts.values()):
                try:
                    subprocess.run(
                        [str(self.control), host, self.port, "stop"],
                        timeout=10,
                        check=False,
                    )
                except subprocess.TimeoutExpired:
                    print(f"forcing service cleanup on {host}", flush=True)
                finally:
                    self.signal_remote_groups(host, "KILL")
            for name, (host, stdout, stderr) in self.logs.items():
                for source, suffix in ((stdout, "stdout"), (stderr, "stderr")):
                    subprocess.run(
                        [
                            "scp",
                            "-q",
                            "-oBatchMode=yes",
                            f"{host}:{source}",
                            str(self.directory / f"{name}.{suffix}.log"),
                        ],
                        check=False,
                    )

    def signal_remote_groups(self, host, sig, names=None):
        files = [
            self.directory / f"{name}.pid"
            for name, (running_host, *_) in self.logs.items()
            if running_host == host and (names is None or name in names)
        ]
        if files:
            self.remote(
                host,
                [
                    "bash",
                    "-c",
                    'sig=$1; shift; for file do if read -r pid < "$file" && '
                    '[[ "$pid" =~ ^[1-9][0-9]*$ ]]; then '
                    'kill -"$sig" -- -"$pid" 2>/dev/null || true; fi; done',
                    "bash",
                    sig,
                    *files,
                ],
                timeout=15,
            )

    def launch(self, corpus):
        a, d = self.args, self.directory
        addr = self.address
        for host in set(self.hosts.values()):
            if not a.local:
                self.remote(host, ["mkdir", "-p", d / "state"])
        (d / "state").mkdir(exist_ok=True)
        self.stage(corpus, self.hosts["replay"], d / "corpus")
        if not a.local:
            (d / "corpus").mkdir(exist_ok=True)
            shutil.copyfile(
                corpus / "manifest.json", d / "corpus/manifest.json"
            )
        config = d / "coordinator.json"
        # The reviewed CI fixture contains exactly one validator.
        config_data = json.loads(
            (ROOT / "scripts/config-chuckd.json").read_text()
        )
        if len(config_data["validator_config"]["known_validators"]) != 1:
            raise RuntimeError(
                "hackathon benchmark requires exactly one validator"
            )
        config.write_text(json.dumps(config_data))
        config.chmod(0o600)
        self.stage(config, self.hosts["coordinator"], config)

        def binary(name):
            return a.build / BINS[name]

        network = "0" * 63 + "1"
        self.start(
            "mempool",
            [
                binary("mempool"),
                "--bind-ip",
                self.ips[self.hosts["mempool"]],
                "--network-hash",
                network,
                "--num-db-shards",
                "1",
                "--num-exec-shards",
                "2",
                "--coordinator-port",
                "8070",
                "--executor-port",
                "8071",
                "--db-port",
                "8072",
                "--event-port",
                "8073",
                "--auth-port",
                "8074",
                "--metrics-port",
                "8075",
                "--gateway-port",
                "8076",
            ],
        )
        self.wait_log("mempool", "PORT mempool.db=8072")
        self.start(
            "db",
            [
                binary("db"),
                "--db-path",
                d / "state/db",
                "--ram-bytes",
                "10000000000",
                "--bind-ip",
                self.ips[self.hosts["db"]],
                "--coordinator-addr",
                addr("mempool", 8072),
                "--executor-port",
                "8080",
                "--mempool-port",
                "8081",
                "--auth-port",
                "8082",
                "--metrics-port",
                "8083",
                "--gateway-port",
                "8084",
            ],
        )
        self.wait_log("mempool", "PORT mempool.auth=8074")
        self.remote(self.hosts["event"], ["mkdir", "-p", d / "state/event"])
        self.start(
            "event",
            [
                binary("event"),
                "--mempool-addr",
                addr("mempool", 8073),
                "--bind-ip",
                self.ips[self.hosts["event"]],
                "--work-dir",
                d / "state/event",
                "--executor-port",
                "8090",
                "--mempool-port",
                "8091",
                "--indexer-port",
                "8092",
                "--metrics-port",
                "8093",
            ],
        )
        self.start(
            "auth",
            [
                binary("auth"),
                "--mempool-addr",
                addr("mempool", 8074),
                "--bind-ip",
                self.ips[self.hosts["auth"]],
                "--db-port",
                "8050",
                "--mempool-port",
                "8051",
                "--metrics-port",
                "8052",
            ],
        )
        self.wait_log("event", "PORT event.indexer=8092")
        self.start(
            "indexer",
            [
                binary("indexer"),
                "--event-addr",
                addr("event", 8092),
                "--db-path",
                d / "state/indexer",
                "--bind-ip",
                self.ips[self.hosts["indexer"]],
                "--block-explorer-port",
                "8061",
                "--metrics-port",
                "8062",
            ],
        )
        self.wait_log("mempool", "PORT mempool.coordinator=8070")
        self.start(
            "bootstrap",
            [
                binary("bootstrap"),
                "bootstrap",
                "--corpus",
                d / "corpus",
                "--listen",
                addr("bootstrap", 44400),
            ],
        )
        self.wait_log("bootstrap", "hackathon genesis server ready")
        self.start(
            "coordinator",
            [
                binary("coordinator"),
                "-l",
                "info",
                "--stack",
                "--config",
                config,
                "--instance",
                "970c9b",
                "--seed",
                "1410441218572558875",
                "--metrics",
                ":2223",
                "bench",
                "coordinator_loadgen",
                "--in-addr",
                addr("coordinator", 7888),
                "--debug-port",
                "5500",
                "--network-hash",
                network,
                "--storage-dir",
                d / "state/chuckd",
                "--out-addr",
                ":50000",
                "--sched-addr",
                addr("mempool", 8070),
                "--scpd-path",
                a.scpd,
                "--scp-handle-addr",
                "127.0.0.1:3300",
                "--scp-control-addr",
                "127.0.0.1:4400",
                "--loadgen-addr",
                addr("bootstrap", 44400),
                "--num-blocks",
                "1",
            ],
        )
        self.wait_log("event", "event initialization complete")
        for i in range(2):
            name = f"exec{i}"
            self.start(
                name,
                [
                    binary(name),
                    "--coordinator-addr",
                    addr("mempool", 8071),
                    "--bind-ip",
                    self.ips[self.hosts[name]],
                    "--mempool-port",
                    str(8040 + i),
                    "--metrics-port",
                    str(8042 + i),
                    "--db-executor-addr",
                    addr("db", 8080),
                    "--event-executor-addr",
                    addr("event", 8090),
                    "--view-gateway-port",
                    str(8044 + i),
                ],
            )
        self.wait_log(
            "coordinator", "waiting for work tx batch 0 to arrive from loadgen"
        )
        for name in ("exec0", "exec1", "indexer", "mempool", "db", "auth"):
            self.wait_log(
                name,
                {
                    "exec0": "hog_exec",
                    "exec1": "hog_exec",
                    "db": "hog_db",
                    "auth": "auth server",
                }.get(name, name)
                + " initialization complete",
            )
        # Match the gateway's connection budget to the replay client instead
        # of benchmarking its small default TCP connection cap.
        gateway_config = d / "gateway.toml"
        gateway_config.write_text(
            f"max_connections = {a.outstanding + a.receipt_workers + 128}\n"
        )
        self.stage(gateway_config, self.hosts["gateway"], gateway_config)
        gateway_prefix = [a.gateway, "--config", gateway_config]
        if a.gateway_protocol:
            gateway_prefix += ["--hogd-protocol", a.gateway_protocol]
        self.start(
            "gateway",
            [
                *gateway_prefix,
                "--network-hash",
                network,
                "--listen",
                addr("gateway", 9546),
                "--observability-listen",
                addr("gateway", 9547),
                "--scheduler",
                addr("mempool", 8076),
                "--indexer",
                addr("indexer", 8061),
                "--db-gateway",
                addr("db", 8084),
                "--view-gateway",
                addr("exec0", 8044),
                "--health-required-backends",
                "scheduler,indexer,db_gateway,view_gateway",
            ],
        )
        self.wait_gateway()

    def wait_gateway(self):
        deadline = time.monotonic() + self.args.ready_timeout
        next_check = 0
        url = f"http://{self.address('gateway', 9547)}/readyz"
        while time.monotonic() < deadline:
            if time.monotonic() >= next_check:
                self.check_services()
                next_check = time.monotonic() + 5
            try:
                with urllib.request.urlopen(url, timeout=5) as response:
                    if response.status == 200:
                        return
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(0.5)
        raise RuntimeError("gateway readiness timeout; inspect gateway logs")

    def replay(self):
        a, d = self.args, self.directory
        metrics = {
            "mempool": 8075,
            "auth": 8052,
            "db": 8083,
            "exec0": 8042,
            "exec1": 8043,
            "event": 8093,
            "indexer": 8062,
            "gateway": 9547,
        }
        endpoints = ",".join(
            f"{name}=http://{self.address(name, port)}/metrics"
            for name, port in metrics.items()
        )
        self.start(
            "replay",
            [
                a.build / "hackathon-replay",
                "run",
                "--corpus",
                d / "corpus",
                "--output",
                d / "replay",
                "--url",
                f"http://{self.address('gateway', 9546)}",
                "--metrics",
                endpoints,
                "--drain",
                f"{a.drain}s",
                "--outstanding",
                str(a.outstanding),
                "--receipt-workers",
                str(a.receipt_workers),
            ],
        )
        duration = json.loads(
            (
                d / "corpus/manifest.json"
                if a.local
                else a.corpus / "manifest.json"
            ).read_text()
        )["durationSeconds"]
        waiter = (
            self.processes["replay"]
            if a.local
            else subprocess.Popen(
                [str(self.control), self.hosts["replay"], self.port, "wait"]
            )
        )
        deadline = (
            time.monotonic() + a.ready_timeout + duration + 2 * a.drain + 120
        )
        try:
            while True:
                self.check_services()
                try:
                    status = waiter.wait(timeout=5)
                    break
                except subprocess.TimeoutExpired:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("replay deadline exceeded")
            self.check_services()
        finally:
            if not a.local:
                try:
                    if waiter.poll() is None:
                        # Let Run cancel submissions and flush results before
                        # collecting them or stopping the remaining services.
                        self.signal_remote_groups(
                            self.hosts["replay"], "TERM", names=("replay",)
                        )
                        try:
                            waiter.wait(timeout=30)
                        except subprocess.TimeoutExpired:
                            self.signal_remote_groups(
                                self.hosts["replay"], "KILL", names=("replay",)
                            )
                            waiter.wait(timeout=10)
                finally:
                    if waiter.poll() is None:
                        waiter.terminate()
                        waiter.wait(timeout=10)
                    # Preserve partial metrics even if graceful shutdown fails.
                    subprocess.run(
                        [
                            "scp",
                            "-q",
                            "-r",
                            "-oBatchMode=yes",
                            f"{self.hosts['replay']}:"
                            f"{shlex.quote(str(d / 'replay'))}",
                            str(d),
                        ],
                        check=False,
                    )
        if status:
            raise RuntimeError(
                f"replay failed with exit status {status}; inspect replay logs"
            )
        if not (d / "replay/results.jsonl").exists():
            raise RuntimeError("replay results missing; inspect replay logs")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--gateway", type=Path, required=True)
    p.add_argument("--gateway-sha256", required=True)
    p.add_argument(
        "--gateway-protocol",
        help="explicit handshake token for a verified "
        "wire-compatible gateway build",
    )
    p.add_argument("--scpd", type=Path, required=True)
    p.add_argument("--corpus", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument(
        "--local",
        action="store_true",
        help="one real validator with all services on this host; smoke only",
    )
    p.add_argument("--control-port", type=int, default=0)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--ready-timeout", type=int, default=600)
    p.add_argument("--drain", type=int, default=120)
    p.add_argument("--outstanding", type=int, default=16384)
    p.add_argument("--receipt-workers", type=int, default=16)
    args = p.parse_args()
    for name in ("build", "gateway", "scpd", "corpus", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if (
        min(args.repeats, args.drain, args.outstanding, args.receipt_workers)
        < 1
    ):
        p.error(
            "repeats, drain, outstanding and receipt-workers must be positive"
        )
    if not args.local and not args.control_port:
        p.error(
            "CI replay requires --control-port and "
            "exclusive allocated benchmark hosts"
        )
    if args.local:
        actual = hashlib.sha256(args.gateway.read_bytes()).hexdigest()
    else:
        actual = subprocess.check_output(
            [
                "ssh",
                "-n",
                "-oBatchMode=yes",
                "hog4",
                shlex.join(["sha256sum", str(args.gateway)]),
            ],
            text=True,
        ).split()[0]
    if actual != args.gateway_sha256:
        p.error("gateway checksum mismatch")
    args.output.mkdir(parents=True, exist_ok=False)
    # The caller supplies explicit binaries. Save the exact checkout and corpus
    # identities; uncommitted changes are retained as patches for diagnosis.
    (args.output / "hogd.patch").write_bytes(
        subprocess.check_output(["git", "diff", "HEAD"], cwd=ROOT)
    )
    identity = {
        "gatewaySha256": actual,
        "gatewayProtocolOverride": args.gateway_protocol,
        "hogdRevision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "chuckdRevision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT / "groundhog-go/chuckd",
            text=True,
        ).strip(),
        "singleValidator": True,
        "localSmoke": args.local,
        "diagnostics": True,
        "ingress": "rpc-gateway",
    }
    identity["binaries"] = {}
    for name, relative in BINS.items():
        path = args.build / relative
        if args.local:
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
        else:
            digest = subprocess.check_output(
                [
                    "ssh",
                    "-n",
                    "-oBatchMode=yes",
                    HOSTS[name],
                    shlex.join(["sha256sum", str(path)]),
                ],
                text=True,
            ).split()[0]
        identity["binaries"][name] = digest
    identity["scpSha256"] = (
        hashlib.sha256(args.scpd.read_bytes()).hexdigest()
        if args.local
        else subprocess.check_output(
            [
                "ssh",
                "-n",
                "-oBatchMode=yes",
                "hog4",
                shlex.join(["sha256sum", str(args.scpd)]),
            ],
            text=True,
        ).split()[0]
    )
    with (args.corpus / "transactions.jsonl").open("rb") as corpus_file:
        identity["corpusSha256"] = hashlib.file_digest(
            corpus_file, "sha256"
        ).hexdigest()
    (args.output / "identity.json").write_text(json.dumps(identity, indent=2))
    for repeat in range(args.repeats):
        directory = args.output / f"gateway-{repeat}"
        directory.mkdir()
        cluster = Cluster(args, directory)
        successful = False
        try:
            if not args.local:
                for host in set(HOSTS.values()):
                    subprocess.run(
                        [str(cluster.control), host, cluster.port, "ready"],
                        check=True,
                    )
            cluster.launch(args.corpus)
            cluster.replay()
            successful = True
        finally:
            cluster.stop()
            if (directory / "replay/results.jsonl").exists():
                report = subprocess.run(
                    [
                        "python3",
                        str(BENCHMARK_DIR / "report.py"),
                        str(directory),
                    ],
                    check=False,
                )
                if successful and report.returncode:
                    raise RuntimeError(
                        "report validation failed; raw results retained"
                    )


if __name__ == "__main__":
    main()
