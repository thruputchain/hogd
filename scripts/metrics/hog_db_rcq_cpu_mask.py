"""Select the qualified RCQ mask when all 24 cores are available."""

from collections import defaultdict
import os
from pathlib import Path
import sys


def cpu_list(path):
    cpus = set()
    for part in path.read_text().strip().split(","):
        start, _, end = part.partition("-")
        cpus.update(range(int(start), int(end or start) + 1))
    return frozenset(cpus)


def compact_mask(root, allowed):
    if allowed != set(range(128)):
        raise ValueError("expected the full 128-CPU mask")

    compact = set(range(24))
    groups = defaultdict(set)
    nodes = set()
    sibling_sets = set()
    for cpu in compact:
        path = root / f"cpu{cpu}"
        [node] = path.glob("node[0-9]*")
        [llc] = [entry for entry in (path / "cache").glob("index*")
                 if (entry / "level").read_text().strip() == "3"]
        siblings = cpu_list(path / "topology/thread_siblings_list")
        shared = cpu_list(llc / "shared_cpu_list")
        if cpu not in siblings or not siblings <= shared <= allowed or siblings & compact != {cpu}:
            raise ValueError("compact CPUs must be distinct physical cores in their LLC")
        nodes.add(node.name)
        groups[shared].update(siblings)
        sibling_sets.add(siblings)

    single_numa_node = len(nodes) == 1
    three_llcs = len(groups) == 3
    one_cpu_per_core = len(sibling_sets) == 24
    if not (single_numa_node and three_llcs and one_cpu_per_core):
        raise ValueError("expected 24 cores across three LLCs in one NUMA node")

    full_eight_core_llcs = all(
        used == shared and sum(core <= shared for core in sibling_sets) == 8
        for shared, used in groups.items()
    )
    if not full_eight_core_llcs:
        raise ValueError("expected eight physical cores per LLC")
    return "0-23"


if __name__ == "__main__":
    try:
        print(compact_mask(Path("/sys/devices/system/cpu"), os.sched_getaffinity(0)))
    except (AttributeError, OSError, ValueError) as error:
        sys.exit(f"RCQ CPU placement unavailable: {error}; using stock placement")
