#!/usr/bin/env python3
"""Run-local receipts from stock Event client/server."""
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time


ROOT = Path('/workspace')
WORK = Path('/bench-event/ci_work_dir')
CAPACITY = 24 << 30
BINARIES = {
    'hog10': ROOT / 'release/hogd/hog_event/hog_event_server_bench',
    'hog11': ROOT / 'release/hogd/hog_event/hog-event',
}
BRANCH = 'codex/event-standard-transport-abba'
CGROUP_FIELDS = ('cpu.max', 'cpu.weight', 'cpuset.cpus.effective', 'cpuset.mems.effective',
                 'memory.max', 'memory.high', 'memory.swap.max', 'pids.max')
BLOCK = re.compile(
    r'^sent 1000000 events in (\d+) ns \(([\d.]+) inner events/sec\) '
    r'opened block in (\d+) ns closed block in (\d+) ns '
    r'total time: (\d+) ns \(([\d.]+) total events/sec\)$',
)


def check(condition, message):
    if not condition:
        raise ValueError(message)


def run(*args):
    return subprocess.check_output(args, text=True, timeout=5).strip()


def gate():
    host = os.environ.get('HOG_HOST')
    sha = os.environ.get('GIT_COMMIT', '')
    xid = os.environ.get('HOG_XID', '')
    check(host in BINARIES and os.environ.get('GIT_BRANCH') == BRANCH
          and os.environ.get('HOGD_CI_CONTAINER') == '1'
          and os.environ.get('HOGD_CI_OFFLINE') == '1'
          and re.fullmatch(r'hogd-event-bench-[1-9][0-9]*-[1-9][0-9]*', xid)
          and re.fullmatch(r'[a-f0-9]{40}', sha)
          and run('git', '-C', str(ROOT), 'rev-parse', 'HEAD') == sha,
          'requires the pinned isolated standalone Event diagnostic')
    return host, sha, xid


def resolve(host):
    addresses = {row[4][0] for row in socket.getaddrinfo(host, None, socket.AF_INET)}
    check(len(addresses) == 1, 'Event alias must resolve to one IPv4 address')
    return addresses.pop()


def network(host):
    peer = resolve('hog11' if host == 'hog10' else 'hog10')
    route, = json.loads(run('ip', '-j', '-4', 'route', 'get', peer))
    source = route['prefsrc']
    subnet = ipaddress.IPv4Network(f'{source}/24', strict=False)
    check(ipaddress.IPv4Address(peer) in subnet and
          subnet.subnet_of(ipaddress.IPv4Network('172.24.0.0/14'))
          and not route.get('gateway') and not run('ip', '-4', 'route', 'show', 'default'),
          'expected direct private Event route without IPv4 default')
    links = json.loads(run('ip', '-j', '-d', 'link', 'show'))
    link, = [entry for entry in links if entry['ifname'] == route['dev']]
    kind = link.get('linkinfo', {}).get('info_kind')
    mtu = link['mtu']
    check((kind == 'wireguard' and mtu == 1420) or
          (kind == 'veth' and 0 < mtu <= 1500 and
           all(entry.get('linkinfo', {}).get('info_kind') != 'wireguard' for entry in links)),
          'unexpected Event interface or MTU')
    return {'host': host, 'source': source, 'peer': peer, 'subnet': str(subnet),
            'mode': 'wireguard' if kind == 'wireguard' else 'overlay', 'kind': kind, 'mtu': mtu}


def storage():
    check(WORK.resolve(strict=True) == WORK and WORK.is_dir() and WORK.stat().st_uid == os.getuid(),
          'expected an owned Event work directory')
    check(run('findmnt', '-rn', '-o', 'TARGET,FSTYPE', '-T', str(WORK)).split()
          == ['/bench-event', 'tmpfs'], 'bad Event storage backing')
    stat = WORK.statvfs()
    cap = stat.f_blocks * stat.f_frsize
    check(cap == CAPACITY, 'wrong Event tmpfs cap')
    return {'path': str(WORK), 'filesystem': 'tmpfs', 'capacity': cap,
            'used_bytes': (stat.f_blocks - stat.f_bfree) * stat.f_frsize,
            'fsid': run('stat', '-f', '-c', '%i', str(WORK))}


def find_process(binary):
    found = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            if (entry / 'exe').readlink() == binary:
                found.append(entry)
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return found


def process_start(proc):
    return (proc / 'stat').read_text().rsplit(') ', 1)[1].split()[19]


def masks(proc):
    status = dict(line.split(':', 1) for line in (proc / 'status').read_text().splitlines())
    values = [status[key].strip() for key in ('Cpus_allowed_list', 'Mems_allowed_list')]
    check(all(re.fullmatch(r'[0-9]+(?:-[0-9]+)?(?:,[0-9]+(?:-[0-9]+)?)*', item) for item in values),
          'invalid process CPU or NUMA mask')
    return values


def topology(cpu_set):
    cpus = []
    for item in cpu_set.split(','):
        bounds = list(map(int, item.split('-')))
        cpus.extend(range(bounds[0], bounds[-1] + 1))
    result = []
    for cpu in cpus:
        path = Path(f'/sys/devices/system/cpu/cpu{cpu}')
        layout = path / 'topology'
        result.append([cpu, (layout / 'physical_package_id').read_text().strip(),
                       (layout / 'core_id').read_text().strip(),
                       sorted(node.name for node in path.glob('node[0-9]*'))])
    return result


def cgroup(proc):
    text = (proc / 'cgroup').read_text().strip()
    check(text == Path('/proc/self/cgroup').read_text().strip()
          and text.startswith('0::/') and '\n' not in text, 'unexpected process cgroup scope')
    relative = Path(text[4:])
    check(not relative.is_absolute() and '..' not in relative.parts, 'unsafe process cgroup path')
    visible = Path('/sys/fs/cgroup') / relative
    # Docker can hide the ancestor chain. Record only controls this namespace can see.
    values = {key: (visible / key).read_text().strip() if (visible / key).is_file() else None
              for key in CGROUP_FIELDS}
    check(values['cpu.max'] and values['memory.max'], 'no readable CPU or memory constraint')
    return values


def binary_hash(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def process(host):
    binary = BINARIES[host]
    deadline = time.monotonic() + 12
    while not (entries := find_process(binary)) and time.monotonic() < deadline:
        time.sleep(.05)
    proc, = entries
    started = process_start(proc)
    argv = (proc / 'cmdline').read_bytes().decode().rstrip('\0').split('\0')
    route = network(host)
    if host == 'hog10':
        check(argv[1:] == ['-e', route['peer'] + ':8090', '-s', route['peer'] + ':8091',
                           '-n', '1000000', '-c', '40', '-o', '100'],
              'not the exact stock NODELAY client')
    else:
        flags = dict(zip(argv[1::2], argv[2::2], strict=True))
        check(flags == {'--mempool-addr': route['peer'] + ':8095', '--bind-ip': route['source'],
                        '--work-dir': flags.get('--work-dir'), '--executor-port': '8090',
                        '--mempool-port': '8091', '--indexer-port': '8092'}
              and Path(flags['--work-dir']) == WORK, 'not the stock Event server and scratch path')
    allowed = masks(proc)
    thread_masks = set()
    count = 0
    for thread in (proc / 'task').iterdir():
        try:
            thread_masks.add(tuple(masks(thread)))
            count += 1
        except (FileNotFoundError, ProcessLookupError):
            continue
    check(count > 0 and thread_masks == {tuple(allowed)}, 'unexpected Event thread placement')
    limits = cgroup(proc)
    digest = binary_hash(proc / 'exe')
    check(started == process_start(proc) and (proc / 'exe').readlink() == binary,
          'Event process changed during capture')
    return {'host': host, 'pid': int(proc.name), 'start_ticks': started,
            'allowed': allowed, 'topology': topology(allowed[0]), 'cgroup_visible': limits,
            'threads_sampled': count, 'binary_sha256': digest, 'argv': argv}


def snapshot(host):
    result = {'network': network(host)}
    if host == 'hog11':
        result['storage'] = storage()
    return result


def reclaim():
    before = storage()
    check(not find_process(BINARIES['hog11']), 'Event process survived checked stop')
    for child in WORK.iterdir():
        check(child.lstat().st_uid == os.getuid() and not child.is_symlink()
              and child.stat().st_dev == WORK.stat().st_dev, 'unsafe Event scratch child')
        if child.is_dir():
            check(shutil.rmtree.avoids_symlink_attacks, 'unsafe directory reclamation support')
            shutil.rmtree(child)
        else:
            check(child.is_file(), 'unexpected nonregular Event scratch child')
            child.unlink()
    check(not list(WORK.iterdir()), 'Event scratch was not empty after reclamation')
    return {'checked_event_exited': True, 'before': before, 'after': storage()}


def load(root, name):
    return json.loads((root / name).read_text())


def validate(root, sha, xid):
    facts = {host: [load(root, f'{host}-{phase}.json') for phase in ('before', 'after')]
             for host in BINARIES}
    links = {host: before['network'] for host, (before, _) in facts.items()}
    a, b = map(links.get, ('hog10', 'hog11'))
    check(a['source'] == b['peer'] and b['source'] == a['peer']
          and a['subnet'] == b['subnet'] and a['mode'] == b['mode'] and a['mtu'] == b['mtu'],
          'inconsistent Event route pair')
    for host, (before, after) in facts.items():
        check(before['network'] == after['network'], f'{host} route or MTU changed')
    before, after = [row['storage'] for row in facts['hog11']]
    cleanup = load(root, 'reclaim.json')
    check(before['fsid'] == after['fsid'] == cleanup['after']['fsid'] and
          after['used_bytes'] > before['used_bytes'] == cleanup['after']['used_bytes']
          and cleanup['checked_event_exited'] is True, 'storage changed or was not reclaimed')
    client = load(root, 'hog10-process.json')
    server = load(root, 'hog11-process-before.json')
    end = load(root, 'hog11-process-after.json')
    for key in ('host', 'argv', 'pid', 'start_ticks', 'binary_sha256', 'allowed', 'topology', 'cgroup_visible'):
        check(server[key] == end[key], f'Event server {key} changed')
    stderr = (root / 'client.stderr').read_text()
    check(re.findall(r'git commit hash: ([a-f0-9]{40})', stderr) == [sha], 'wrong client build source')
    lines = [line for line in (root / 'blocks.log').read_text().splitlines() if line.startswith('sent ')]
    blocks = []
    for line in lines:
        match = BLOCK.fullmatch(line)
        check(match, 'wrong event count or timing format')
        send, inner, opening, closing, total, throughput = match.groups()
        send, opening, closing, total = map(int, (send, opening, closing, total))
        speed = float(throughput)
        check(0 < send <= total and closing <= total and opening <= total
              and 0 < speed <= float(inner) and math.isclose(speed, 1e15 / total, rel_tol=1e-6),
              'invalid Event duration or throughput')
        blocks.append({'send_ns': send, 'open_ns': opening, 'close_ns': closing,
                       'total_ns': total, 'events_per_s': speed})
    check(len(blocks) == 20, 'stock Event benchmark did not complete 20 blocks')
    return {'source': sha, 'run': xid, 'transport': a['mode'], 'network': links,
            'storage': {'before': before, 'after': after, 'reclaim': cleanup},
            'client': client, 'server': server, 'blocks': blocks}


if __name__ == '__main__':
    host, sha, xid = gate()
    action, *args = sys.argv[1:]
    if action in ('before', 'after') and not args:
        result = snapshot(host)
    elif action == 'process' and not args:
        result = process(host)
    elif action == 'reclaim' and host == 'hog11' and not args:
        result = reclaim()
    elif action == 'validate' and host == 'hog10' and len(args) == 1:
        result = validate(Path(args[0]), sha, xid)
    else:
        raise ValueError('invalid Event diagnostic action')
    print(json.dumps(result, sort_keys=True, indent=2))
