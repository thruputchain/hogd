#!/usr/bin/env python3
"""Read run-local proof and reset only the dedicated Event CPU experiment leaf."""
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys

WORKSPACE = Path("/workspace")
MOUNT = Path("/bench-event")
WORK = MOUNT / "ci_work_dir" / "affinity-arm"
CPU_ROOT = Path("/sys/devices/system/cpu")
CONTROL = WORKSPACE / "release/hogd/hog_exec/bench/control_server"
EVENT = WORKSPACE / "release/hogd/hog_event/hog-event"
WIDTH = 24


def require(condition, message):
    if not condition:
        raise ValueError(message)


def gate():
    require(os.environ.get("GIT_BRANCH") == "codex/event-server-cpu-abba" and
            os.environ.get("HOGD_CI_CONTAINER") == "1" and
            os.environ.get("HOGD_CI_OFFLINE") == "1" and
            re.fullmatch(r"hogd-event-bench-[0-9]+-[0-9]+", os.environ.get("HOG_XID", "")),
            "requires the isolated Event CPU diagnostic")
    host = os.environ.get("HOG_HOST")
    require(host in ("hog10", "hog11"), "wrong Event role")
    return host


def ip(*args):
    return json.loads(subprocess.check_output(["ip", "-j", *args], text=True))


def peer_ip(host):
    addresses = {x[4][0] for x in socket.getaddrinfo(host, None, socket.AF_INET)}
    require(len(addresses) == 1, "Event alias is ambiguous")
    return addresses.pop()


def network(host):
    other = "hog11" if host == "hog10" else "hog10"
    peer = peer_ip(other)
    route, = ip("-4", "route", "get", peer)
    local = route.get("prefsrc", "")
    subnet = ipaddress.IPv4Network(f"{local}/24", strict=False)
    require(subnet.subnet_of(ipaddress.IPv4Network("172.24.0.0/14")) and
            ipaddress.IPv4Address(peer) in subnet and not route.get("gateway"),
            "requires direct run-local routing")
    links = ip("-d", "link", "show")
    require(all(link.get("linkinfo", {}).get("info_kind") != "wireguard" for link in links),
            "WireGuard is outside the CPU comparison")
    link, = [link for link in links if link["ifname"] == route.get("dev")]
    require(link.get("linkinfo", {}).get("info_kind") == "veth" and
            0 < link["mtu"] <= 1500, "expected ordinary overlay endpoint")
    return {"host": host, "source": local, "peer": peer, "subnet": str(subnet),
            "link_kind": "veth", "mtu": link["mtu"]}


def pids(executable):
    matches = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "exe").readlink() == executable:
                matches.append(int(entry.name))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return matches


def cpus(value):
    result = set()
    for item in value.strip().split(","):
        limits = tuple(map(int, item.split("-")))
        require(len(limits) in (1, 2), "invalid CPU list")
        result.update(range(limits[0], limits[-1] + 1))
    return result


def choose_mask(allowed):
    domains = {}
    siblings_seen = set()
    for cpu in sorted(allowed):
        root = CPU_ROOT / f"cpu{cpu}"
        siblings = (root / "topology/thread_siblings_list").read_text().strip()
        if siblings in siblings_seen:
            continue
        require(cpu in cpus(siblings), "wrong SMT topology")
        siblings_seen.add(siblings)
        node, = [entry.name for entry in root.glob("node[0-9]*")]
        caches = [entry for entry in (root / "cache").glob("index*")
                  if (entry / "type").read_text().strip() == "Unified"]
        cache = max(caches, key=lambda entry: int((entry / "level").read_text()))
        require(int((cache / "level").read_text()) >= 3, "missing LLC topology")
        llc = (cache / "shared_cpu_list").read_text().strip()
        require(cpu in cpus(llc), "wrong LLC topology")
        domains.setdefault(node, {}).setdefault(llc, []).append(cpu)
    for node, llcs in sorted(domains.items()):
        full = sorted((cores for cores in llcs.values() if len(cores) == 8))
        if len(full) >= 3:
            return {"mask": sorted(sum(full[:3], [])), "numa": node, "llcs": full[:3]}
    raise ValueError("need three eight-core LLCs on one NUMA node")


def mount_check():
    require(WORK.parent.resolve(strict=True) == WORK.parent, "unsafe Event tmpfs path")
    filesystem = subprocess.check_output(
        ["findmnt", "-n", "-o", "FSTYPE", "--mountpoint", MOUNT], text=True
    ).strip()
    require(filesystem == "tmpfs", "expected dedicated Event tmpfs")


def stock_mask():
    controller, = pids(CONTROL)
    return os.sched_getaffinity(controller)


def reset():
    mount_check()
    require(not pids(EVENT), "Event process still running")
    require(shutil.rmtree.avoids_symlink_attacks, "unsafe reset implementation")
    if WORK.exists() or WORK.is_symlink():
        require(WORK.is_dir() and not WORK.is_symlink()
                and WORK.stat().st_dev == MOUNT.stat().st_dev, "unsafe experiment leaf")
        shutil.rmtree(WORK)
    WORK.mkdir(mode=0o700)


def proof(mode):
    mount_check()
    expected = stock_mask()
    if mode == "packed":
        expected = set(choose_mask(expected)["mask"])
    pid, = pids(EVENT)
    argv = (Path(f"/proc/{pid}") / "cmdline").read_bytes().split(b"\0")
    require(str(WORK).encode() in argv, "unexpected Event work directory")
    actual = {}
    for task in (Path(f"/proc/{pid}") / "task").iterdir():
        try:
            actual[int(task.name)] = sorted(os.sched_getaffinity(int(task.name)))
        except ProcessLookupError:
            continue
    require(pid in actual and all(set(mask) == expected for mask in actual.values()),
            "Event thread affinity mismatch")
    # Read only the matched Event child's own startup log. No environment or
    # credential-bearing metadata is read.
    stderr = (Path(f"/proc/{pid}") / "fd/2").resolve(strict=True)
    require(stderr.parent in (Path("/tmp"), WORKSPACE / ".ci/tmp")
            and stderr.name.startswith("hogd_stderr_"), "unexpected Event startup log")
    with stderr.open() as stream:
        startup = stream.read(65536)
    workers, = re.findall(rf"pid:{pid}, os:Linux, arch:[^,]+, cores:(\d+)", startup)
    require(int(workers) == len(expected) <= len(actual), "Event worker width mismatch")
    commit, = re.findall(r"git commit hash: ([a-f0-9]{40})", startup)
    require(commit == os.environ.get("GIT_COMMIT"), "Event server source differs")
    return {"mode": mode, "pid": pid, "pool_width": int(workers), "source_sha": commit,
            "tids": actual, "network": network("hog11"), "filesystem": "tmpfs"}


def main():
    host = gate()
    action = sys.argv[1:]
    if action == ["network"]:
        value = network(host)
    else:
        require(host == "hog11", "Event server action requires hog11")
        if action == ["mask"]:
            value = choose_mask(stock_mask())
        elif action == ["reset"]:
            reset()
            value = {"reset": str(WORK)}
        elif action in (["proof", "stock"], ["proof", "packed"]):
            value = proof(action[1])
        else:
            raise ValueError("invalid Event diagnostic action")
    print(json.dumps(value, sort_keys=True))


if __name__ == "__main__":
    main()
