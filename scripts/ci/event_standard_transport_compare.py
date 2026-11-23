#!/usr/bin/env python3
"""Check four complete standalone Event overlay/WireGuard arms."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path, PurePosixPath
import re
from statistics import median
import tarfile
import tempfile

from event_standard_transport import validate


HARNESS = 'f50503452f34148396346d04b5c77ee572967d01'
IMAGE = ('ghcr.io/thruputchain/benchmarks/hogd-ci@sha256:'
         '55eae86474698c796aafd426d76ab8512a3228511727d5a9a4f55e8fb4862283')
PREFIX = 'hog10/.ci/dumps/event-standard-transport/'
FILES = {
    'hog10-before.json', 'hog10-after.json', 'hog11-before.json', 'hog11-after.json',
    'hog10-process.json', 'hog11-process-before.json', 'hog11-process-after.json',
    'blocks.log', 'client.stderr', 'reclaim.json', 'stop.log', 'summary.json',
}
SEQUENCE = ('overlay', 'wireguard', 'wireguard', 'overlay')


def check(condition, message):
    if not condition:
        raise ValueError(message)


def read_arm(archive, log, source, mode):
    seen = {}
    with tarfile.open(archive, 'r|*') as tar:
        for item in tar:
            name = item.name.removeprefix('./')
            check('.publish-requested' not in PurePosixPath(name).parts, 'unexpected metric request')
            if name == 'event-transport.txt' or name.startswith(PREFIX) and item.isfile():
                check(name not in seen and item.isfile() and item.size <= 1 << 20, f'bad receipt {name}')
                seen[name] = tar.extractfile(item).read()
    manifest = dict(line.split('=', 1) for line in seen.pop('event-transport.txt').decode().splitlines())
    xid = manifest.get('run', '')
    check(re.fullmatch(r'hogd-event-bench-[1-9][0-9]*-[1-9][0-9]*', xid) and manifest == {
        'transport': mode, 'hogd_sha': source, 'benchmarks_sha': HARNESS, 'image': IMAGE, 'run': xid,
    }, 'wrong trusted Event transport manifest')
    check(seen.keys() == {PREFIX + name for name in FILES}, 'missing or unknown Event receipt')
    with tempfile.TemporaryDirectory(prefix='event-transport-compare.') as directory:
        root = Path(directory)
        for name in FILES:
            (root / name).write_bytes(seen[PREFIX + name])
        data = validate(root, source, xid)
        check(data == json.loads(seen[PREFIX + 'summary.json']) and data['transport'] == mode,
              'recomputed evidence differs from source summary or intended transport')
    check(log.stat().st_size <= 32 << 20, 'oversized job log')
    text = re.sub(r'\x1b\[[0-9;]*m', '', log.read_text())
    for required in (HARNESS, source, IMAGE,
                     f'map[bench_1:hog11 metrics_1:hog10] acquired as {xid}, starting jobs',
                     f'Artifact hogd-container-results-{xid.removeprefix("hogd-event-bench-")}.zip successfully finalized'):
        check(required in text, f'missing trusted job receipt {required}')
    check('##[error]' not in text and text.count('Container network isolation and authenticated peer transport passed') == 2,
          'failed job or missing two workload isolation receipts')
    return data


def metrics(arm):
    rows = arm['blocks']
    result = {'throughput_median': median(row['events_per_s'] for row in rows)}
    for field in ('send_ns', 'close_ns', 'total_ns'):
        values = sorted(row[field] for row in rows)
        result[field] = {'p50': median(values), 'p95': values[math.ceil(.95 * len(values)) - 1],
                         'max': values[-1]}
    return result


def summarize(arms):
    check(tuple(arm['transport'] for arm in arms) == SEQUENCE
          and len({arm['run'] for arm in arms}) == 4, 'need four distinct ABBA runs')
    keys = ('host', 'allowed', 'topology', 'cgroup_visible', 'binary_sha256')
    for role in ('client', 'server'):
        for key in keys:
            first = arms[0][role][key]
            check(all(arm[role][key] == first for arm in arms), f'{role} {key} differs')
    storages = Counter((storage['path'], storage['filesystem'], storage['capacity'])
                       for arm in arms for storage in (arm['storage']['before'], arm['storage']['after']))
    check(len(storages) == 1, 'storage path, filesystem, or cap differs')
    stats = [metrics(arm) for arm in arms]
    overlay_low = min(stats[0]['throughput_median'], stats[3]['throughput_median'])
    drift = abs(stats[0]['throughput_median'] - stats[3]['throughput_median']) / overlay_low
    threshold = max(.15, 2 * drift)
    pairs = []
    for overlay, wg in ((stats[0], stats[1]), (stats[3], stats[2])):
        gain = wg['throughput_median'] / overlay['throughput_median'] - 1
        tail = all(wg[key][quantile] <= 1.05 * overlay[key][quantile]
                   for key in ('send_ns', 'total_ns') for quantile in ('p95', 'max'))
        close = wg['close_ns']['p95'] <= max(1.20 * overlay['close_ns']['p95'],
                                            overlay['close_ns']['p95'] + 10_000_000)
        pairs.append({'wg_throughput_gain_fraction': gain, 'send_and_total_tail_ok': tail,
                      'close_tail_ok': close, 'practical_gain': gain >= threshold and tail and close})
    return {'runs': {arm['run']: stats[index] for index, arm in enumerate(arms)},
            'overlay_control_drift_fraction': drift, 'required_gain_fraction': threshold,
            'pairs': pairs, 'accept_workload_specific_gain': all(pair['practical_gain'] for pair in pairs),
            'limits': 'Actual process masks and visible cgroup only; host ancestors and contention unobserved. '
                      'With 20 blocks the largest value is shown instead of a population p99.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-sha', required=True)
    parser.add_argument('--arm', nargs=2, metavar=('ARCHIVE', 'JOB_LOG'), action='append', required=True)
    args = parser.parse_args()
    check(re.fullmatch(r'[a-f0-9]{40}', args.source_sha) and len(args.arm) == 4,
          'require pinned source and four explicit ABBA arms')
    result = summarize([read_arm(Path(tar), Path(log), args.source_sha, mode)
                        for (tar, log), mode in zip(args.arm, SEQUENCE, strict=True)])
    print(json.dumps(result, indent=2))
