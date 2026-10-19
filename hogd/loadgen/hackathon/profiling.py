"""Bounded, opt-in CPU sampling of this benchmark's own service processes."""

import json
from pathlib import Path
import shlex
import subprocess
import time

PROFILE_TARGETS = ("gateway", "mempool", "replay")

# Attaching avoids changing the service PID monitored by the control server.
# Software CPU-clock sampling works on hosts without hardware PMU access.
PROFILE_SCRIPT = r"""
set -u
pid=$1
output=$2
mkdir -p "$output"
if ! [[ "$pid" =~ ^[1-9][0-9]*$ ]] || ! kill -0 "$pid"; then
  echo "Invalid or exited profile target" >&2
  exit 1
fi
perf --version > "$output/perf-version.txt" 2>&1 || exit 1
cat /proc/sys/kernel/perf_event_paranoid > "$output/perf-event-paranoid.txt"
snapshot() {
  python3 - "$pid" "$output/$1.json" <<'PYSNAPSHOT'
import json, pathlib, sys, time
root = pathlib.Path('/proc') / sys.argv[1]
tasks = {}
for path in root.glob('task/*/stat'):
    try:
        stat = path.read_text()
        fields = stat[stat.rfind(')') + 2:].split()
        tasks[path.parent.name] = {
            'comm': stat[stat.find('(') + 1:stat.rfind(')')],
            'userTicks': int(fields[11]), 'systemTicks': int(fields[12]),
        }
    except (FileNotFoundError, ProcessLookupError):
        pass
pathlib.Path(sys.argv[2]).write_text(json.dumps({
    'unixNs': time.time_ns(), 'clockTicksPerSecond': __import__('os').sysconf('SC_CLK_TCK'),
    'tasks': tasks,
}))
PYSNAPSHOT
}
snapshot threads-start
date +%s%N > "$output/started-unix-ns.txt"
timeout --signal=INT --kill-after=5s 35s perf record \
  -e cpu-clock -F 99 --call-graph dwarf,8192 -p "$pid" \
  -o "$output/perf.data" > "$output/record.log" 2>&1
status=$?
date +%s%N > "$output/finished-unix-ns.txt"
snapshot threads-finish
printf '%s\n' "$status" > "$output/record-status.txt"
if [[ "$status" != 0 && "$status" != 124 ]]; then exit "$status"; fi
perf report --stdio --no-children --percent-limit 0.5 --sort symbol,dso \
  -i "$output/perf.data" > "$output/self.txt" 2> "$output/report.log" || exit 1
perf report --stdio --children --percent-limit 1 --call-graph graph,1,caller \
  -i "$output/perf.data" > "$output/callers.txt" 2>> "$output/report.log"
"""


class CPUProfiles:
    def __init__(self, cluster):
        self.cluster = cluster
        self.jobs = []
        self.started = False
        self.trigger_elapsed = None

    def maybe_start(self):
        if self.started:
            return
        c = self.cluster
        metadata = c.directory / "replay/run.json"
        try:
            if c.args.local:
                elapsed = (
                    time.time_ns()
                    - json.loads(metadata.read_text())["startedUnixNs"]
                ) / 1e9
            else:
                # Use the replay host's own clock, avoiding cross-host skew.
                script = """import json,pathlib,sys,time
try:
    start=json.loads(pathlib.Path(sys.argv[1]).read_text())["startedUnixNs"]
    print((time.time_ns()-start)/1e9)
except (FileNotFoundError,json.JSONDecodeError):
    print(-1)
"""
                elapsed = float(
                    c.remote(
                        c.hosts["replay"],
                        ["python3", "-c", script, metadata],
                        capture_output=True,
                        text=True,
                        timeout=10,
                    ).stdout
                )
        except (OSError, ValueError, KeyError, subprocess.SubprocessError):
            # Startup can precede run.json. Retry without shifting the target
            # window to an arbitrary time since process launch.
            return
        if elapsed < 220:
            return
        self.trigger_elapsed = elapsed
        if elapsed >= 235:
            self.started = True
            output = c.directory / "profiles"
            output.mkdir(parents=True, exist_ok=True)
            (output / "status.json").write_text(
                json.dumps(
                    {
                        "error": "missed peak profiling window",
                        "replayElapsedSeconds": elapsed,
                    }
                )
            )
            return
        self.start()

    def start(self):
        self.started = True
        c = self.cluster
        for name in PROFILE_TARGETS:
            host = c.hosts[name]
            output = c.directory / "profiles" / name
            output.mkdir(parents=True, exist_ok=True)
            try:
                pid = (
                    str(c.processes[name].pid)
                    if c.args.local
                    else c.remote(
                        host,
                        ["cat", c.directory / f"{name}.pid"],
                        capture_output=True,
                        text=True,
                        timeout=10,
                    ).stdout.strip()
                )
                if not pid.isdecimal() or int(pid) < 1:
                    raise ValueError("Invalid service PID")
                command = [
                    "bash",
                    "-c",
                    PROFILE_SCRIPT,
                    "bash",
                    pid,
                    str(output),
                ]
                if not c.args.local:
                    command = [
                        "ssh",
                        "-n",
                        "-oBatchMode=yes",
                        "-oConnectTimeout=5",
                        host,
                        shlex.join(command),
                    ]
                with (output / "controller.log").open("wb") as log:
                    proc = subprocess.Popen(
                        command,
                        stdin=subprocess.DEVNULL,
                        stdout=log,
                        stderr=log,
                    )
                self.jobs.append((name, host, output, proc))
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                (output / "status.json").write_text(
                    json.dumps({"error": str(error)})
                )

    def finish(self):
        c = self.cluster
        for name, host, output, proc in self.jobs:
            try:
                status = proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                # The remote timeout bounds perf itself even if SSH gets stuck.
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                status = "controller timeout"
            if not c.args.local:
                try:
                    copied = subprocess.run(
                        [
                            "scp",
                            "-q",
                            "-r",
                            "-oBatchMode=yes",
                            "-oConnectTimeout=5",
                            f"{host}:{shlex.quote(str(output))}",
                            str(output.parent),
                        ],
                        timeout=120,
                        check=False,
                    ).returncode
                except subprocess.TimeoutExpired:
                    copied = "timeout"
            else:
                copied = 0
            (output / "status.json").write_text(
                json.dumps(
                    {
                        "service": name,
                        "exitStatus": status,
                        "copyStatus": copied,
                        "frequencyHz": 99,
                        "durationSeconds": 35,
                        "triggerReplayElapsedSeconds": self.trigger_elapsed,
                    }
                )
            )
            print(
                f"CPU profile {name}: exit={status}, copy={copied}", flush=True
            )
