"""Bounded stock Combined workload receipts. Never export process arguments."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import time
from itertools import islice

# Only observed executable names below these individual build outputs count.
PROGRAMS = {
    "exec": "hogd/hog_exec/hog-exec",
    "event": "hogd/hog_event/hog-event",
    "indexer": "hogd/hog_indexer/hog-indexer",
    "auth": "hogd/hog_auth/hog-auth",
    "db": "hogd/hog_db/hog-db",
    "mempool": "hogd/hog_mempool/hog-mempool",
    "loadgen": "hogd/loadgen/loadgen_server",
    "chuckd": "chuckd.dev",
    "scpd": "scpd.dev",
    "db_bench": "hogd/hog_exec/bench/bench_server",
    "payments": "hogd/hog_exec/bench/payments_bench",
    "fibonacci": "hogd/hog_exec/bench/fibonacci_rv32only",
}
ROLES = {
    "single": {"hog4": ("chuckd", "scpd"), "hog5": ("exec",), "hog6": ("exec",),
               "hog7": ("loadgen",), "hog8": ("auth",), "hog9": ("event", "indexer"),
               "hog10": ("mempool",), "hog11": ("db",)},
    "multi": {**dict.fromkeys(("hog4", "hog8"), ("loadgen", "auth", "chuckd", "scpd")),
              **dict.fromkeys(("hog5", "hog9"), ("exec", "exec")),
              **dict.fromkeys(("hog6", "hog10"), ("event", "indexer")),
              **dict.fromkeys(("hog7", "hog11"), ("mempool", "db"))},
    "fibonacci": {"hog10": ("fibonacci",), "hog11": ("db_bench",), "hog8": ("event",)},
    "payments-2": {"hog10": ("payments",), "hog11": ("db_bench",), "hog8": ("event",)},
    "payments-10": {"hog10": ("payments",), "hog11": ("db_bench",), "hog8": ("event",)},
}
CONFIG_KEYS = {
    "CMAKE_BUILD_TYPE", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_C_FLAGS",
    "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS_RELEASE", "CMAKE_CXX_FLAGS_RELEASE",
    "BUILD_BENCHMARKS",
}
MASK = re.compile(r"[0-9,-]{1,2048}")


def bounded_read(path, limit=1 << 16):
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"oversize {path.name}")
    return data


def placement(pid):
    fields = {}
    for line in bounded_read(pid / "status").decode().splitlines():
        key, _, value = line.partition(":")
        if key in {"Cpus_allowed_list", "Mems_allowed_list"} and MASK.fullmatch(value.strip()):
            fields[key] = value.strip()
    return fields


def mount_type(path, mounts):
    depth, value = 0, None
    for mount, kind in mounts:
        if len(mount.parts) >= depth and path.is_relative_to(mount):
            depth, value = len(mount.parts), kind
    return value


def mounts_at(pid):
    mounts = []
    for line in bounded_read(pid / "mountinfo").decode().splitlines():
        before, sep, after = line.partition(" - ")
        if sep:
            fields = before.split()
            if len(fields) > 4 and "\\" not in fields[4]:
                mounts.append((Path(fields[4]), after.split()[0]))
    return mounts


def cgroup_limits(pid):
    """Resolve v2 membership against the observed task's namespace mount."""
    try:
        membership = next(line[3:] for line in bounded_read(pid / "cgroup").decode().splitlines()
                          if line.startswith("0::/"))
        path = Path(membership)
        if ".." in path.parts:
            return {"status": "outside_visible_cgroup_namespace"}
        for line in bounded_read(pid / "mountinfo").decode().splitlines():
            before, sep, after = line.partition(" - ")
            fields = before.split()
            if not sep or not after.startswith("cgroup2 ") or len(fields) < 5:
                continue
            root, mount = map(Path, fields[3:5])
            if any(chr(92) in str(item) for item in (root, mount)) or not path.is_relative_to(root):
                continue
            resolved = pid / "root" / mount.relative_to("/") / path.relative_to(root)
            limits = {}
            for key in ("cpu.max", "cpuset.cpus.effective", "cpuset.mems.effective", "memory.max"):
                try:
                    limits[key] = bounded_read(resolved / key, 4096).decode().strip()
                except OSError:
                    continue
            return {"status": "observed" if limits else "unreadable", "membership": membership,
                    "mount_root": str(root), "mount_point": str(mount), "limits": limits}
    except (OSError, RuntimeError, ValueError, StopIteration):
        pass
    return {"status": "unavailable"}


def start_ticks(pid):
    return int(bounded_read(pid / "stat").decode().rpartition(") ")[2].split()[19])


def task_snapshot(pid):
    limit = 256
    try:
        tasks = list(islice((pid / "task").iterdir(), limit + 1))
    except OSError:
        return {"status": "unavailable"}
    masks = Counter()
    for task in tasks[:limit]:
        try:
            fields = placement(task)
            if len(fields) == 2:
                masks[fields["Cpus_allowed_list"], fields["Mems_allowed_list"]] += 1
        except (OSError, ValueError):
            continue
    readable = sum(masks.values())
    return {"status": "snapshot", "sampled": readable, "enumerated_lower_bound": len(tasks),
            "partial": readable != len(tasks),
            "distinct": [{"cpus_allowed": cpus, "mems_allowed": mems, "count": count}
                         for (cpus, mems), count in sorted(masks.items())]}


def actual_storage(pid, workspace, mounts):
    observed = {}
    try:
        cmdline = bounded_read(pid / "cmdline").decode(errors="replace").split("\0")
        cwd = Path(os.readlink(pid / "cwd"))
        for index, flag in enumerate(cmdline[:-1]):
            if flag not in ("--db-path", "--work-dir", "--storage-dir"):
                continue
            path = cwd / cmdline[index + 1]
            if path.is_relative_to(workspace) and ".." not in path.parts:
                kind = mount_type(path, mounts)
                if kind:
                    observed[flag] = kind
        # RAM DB benchmark may map its temp file then close its descriptor.
        # Retain only backing types for paths under the approved temp roots.
        if Path(cmdline[0]).name == "bench_server":
            roots = (Path("/tmp"), workspace / ".ci/tmp")
            paths = []
            try:
                for fd in list((pid / "fd").iterdir())[:128]:
                    try:
                        paths.append(Path(os.readlink(fd)))
                    except OSError:
                        pass
                for line in bounded_read(pid / "maps", 1 << 18).decode(errors="replace").splitlines():
                    parts = line.split(maxsplit=5)
                    if len(parts) == 6 and parts[5].startswith("/"):
                        paths.append(Path(parts[5]))
            except OSError:
                pass
            result = sorted({kind for path in paths if any(path.is_relative_to(r) and path != r for r in roots)
                             if (kind := mount_type(path, mounts))})
            if result:
                observed["mapped_temp_filesystem_types"] = result
    except (OSError, ValueError):
        pass
    return observed


def binary_identity(stat):
    return [stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns]


def process_rows(proc, workspace, manifest, roles, seen, first_seen=None):
    paths = {workspace / "release" / PROGRAMS[key]: key for key in roles}
    pids = list(islice((path for path in proc.iterdir() if path.name.isdecimal()), 1025))
    if len(pids) > 1024:
        raise ValueError("unexpectedly large workload PID namespace")
    for pid in pids:
        try:
            exe = Path(os.readlink(pid / "exe"))
            if exe not in paths:
                continue
            key = paths[exe]
            stat = (pid / "exe").stat()
            if binary_identity(stat) != manifest["binary"][key]["identity"]:
                continue
            life = start_ticks(pid)
            tid = (pid.name, life)
            if tid in seen:
                continue
            fields = placement(pid)
            if len(fields) != 2:
                continue
            threads = {"status": "unobserved"}
            if first_seen is not None:
                now = time.monotonic()
                if len(first_seen) >= 128 and tid not in first_seen:
                    continue
                if now - first_seen.setdefault(tid, now) < 3:
                    continue
                threads = task_snapshot(pid)
            mounts = mounts_at(pid)
            same_cgroup = bounded_read(pid / "cgroup") == bounded_read(proc / "self/cgroup")
            cgroup = cgroup_limits(pid)
            storage = actual_storage(pid, workspace, mounts)
            if (binary_identity((pid / "exe").stat()) != binary_identity(stat)
                or start_ticks(pid) != life):
                continue
            seen.add(tid)
            yield {"role": key, "pid": int(pid.name), "start_ticks": life, "allow": fields,
                   "same_as_observer_cgroup": same_cgroup, "cgroup": cgroup, "task_masks": threads,
                   "workspace_fstype": mount_type(workspace, mounts),
                   "storage_fstype": storage}
        except (OSError, IndexError, ValueError):
            continue


def initialize(workspace, host):
    keys = {key for roles in ROLES.values() for key in roles.get(host, ())}
    binaries = {}
    for key in sorted(keys):
        path = workspace / "release" / PROGRAMS[key]
        with path.open("rb") as stream:
            stat = os.fstat(stream.fileno())
            sha = hashlib.file_digest(stream, "sha256").hexdigest()
            after = os.fstat(stream.fileno())
        if (stat.st_size, stat.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError(f"binary changed during hashing: {key}")
        binaries[key] = {"sha256": sha, "identity": binary_identity(stat)}
    cache = {}
    for line in bounded_read(workspace / "release/CMakeCache.txt", 1 << 20).decode().splitlines():
        key, sep, rest = line.partition(":")
        if sep and key in CONFIG_KEYS:
            cache[key] = rest.partition("=")[2]
    if cache.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("expected existing stock Release build")
    observer = Path("/proc/self")
    return {"binary": binaries, "cmake": cache, "cgroup": cgroup_limits(observer),
            "observer_allow": placement(observer)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("init", "sample"))
    parser.add_argument("host", choices=tuple(f"hog{i}" for i in range(4, 12)))
    parser.add_argument("run")
    parser.add_argument("source")
    parser.add_argument("stage", choices=ROLES, nargs="?")
    args = parser.parse_args()
    workspace = Path("/workspace")
    if (os.environ.get("HOGD_CI_CONTAINER") != "1" or Path.cwd() != workspace
        or not re.fullmatch(r"hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*", args.run)
        or not re.fullmatch(r"[a-f0-9]{40}", args.source)):
        parser.error("expected isolated Combined provenance scope")
    out = workspace / ".ci/dumps/combined-transport-provenance"
    out.mkdir(mode=0o700, parents=True, exist_ok=True)
    coordination = workspace / ".ci/combined-transport-provenance"
    coordination.mkdir(mode=0o700, exist_ok=True)
    receipt = out / "host.json"
    metadata = {"host": args.host, "run": args.run, "source": args.source}
    if args.mode == "init":
        staged = json.loads(bounded_read(workspace / ".ci/input-manifest.json", 4096))
        if staged.get("sha") != args.source or not re.fullmatch(r"[^\s]+@sha256:[a-f0-9]{64}", staged.get("image", "")):
            parser.error("staged source/image receipt mismatch")
        payload = initialize(workspace, args.host) | metadata | {"image": staged["image"]}
        with receipt.open("x") as stream:
            json.dump(payload, stream)
        return
    if args.stage not in ROLES or args.host not in ROLES[args.stage]:
        parser.error("invalid stage for host")
    manifest = json.loads(bounded_read(receipt))
    if any(manifest.get(key) != value for key, value in metadata.items()):
        parser.error("host manifest does not match this run")
    seen = set()
    first_seen = {}
    deadline = time.monotonic() + 1800
    started_cpu = time.process_time_ns()
    max_scan_ns = 0
    stop = coordination / f"{args.stage}.stop"
    roles = ROLES[args.stage][args.host]
    observed = Counter()
    with (out / f"{args.stage}.jsonl").open("x", buffering=1) as stream:
        print(json.dumps(metadata | {"stage": args.stage, "mask_scope": "bounded_task_snapshot_after_3s",
                                     "pinning_and_cpu_residency": "unobserved"}), file=stream)
        (coordination / f"{args.stage}.ready").touch(exist_ok=False)
        while time.monotonic() < deadline and not stop.exists() and len(seen) < 64:
            scan_start = time.monotonic_ns()
            for row in process_rows(Path("/proc"), workspace, manifest, roles, seen, first_seen):
                observed[row["role"]] += 1
                print(json.dumps(row), file=stream)
            max_scan_ns = max(max_scan_ns, time.monotonic_ns() - scan_start)
            time.sleep(1)
        print(json.dumps({"sampled_processes": len(seen), "stopped_by_controller": stop.exists(),
                          "missing_instances": dict(Counter(roles) - observed),
                          "observer_cpu_ns": time.process_time_ns() - started_cpu,
                          "max_scan_wall_ns": max_scan_ns}),
              file=stream)


if __name__ == "__main__":
    main()
