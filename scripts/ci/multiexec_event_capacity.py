"""Measure and verify preallocated Event tmpfs capacity on the exact CI role."""
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time


ROOT = Path('/bench-event')
CAPACITY = 24 * 1024**3


def require(condition, message):
    if not condition:
        raise ValueError(message)


def scope(path=None):
    require(os.environ.get('HOG_HOST') == 'hog11' and os.environ.get('HOGD_CI_CONTAINER') == '1'
            and os.environ.get('HOGD_CI_OFFLINE') == '1'
            and os.environ.get('GIT_BRANCH') == 'codex/multiexec-event-capacity'
            and re.fullmatch(r'hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*',
                             os.environ.get('HOG_XID', '')),
            'unexpected Event role or run')
    fs = subprocess.check_output(
        ['findmnt', '-n', '-o', 'TARGET,FSTYPE', '-T', str(ROOT)], text=True, timeout=3,
    ).split()
    require(fs == [str(ROOT), 'tmpfs'], 'expected existing private Event tmpfs')
    if path is not None:
        require(path.parent == ROOT and re.fullmatch(r'multiexec-capacity\.[a-zA-Z0-9]{8}', path.name)
                and path.resolve(strict=True) == path and path.is_dir() and path.stat().st_uid == os.getuid()
                and path.stat().st_mode & 0o777 == 0o700
                and path.stat().st_dev == ROOT.stat().st_dev, 'unsafe trial directory')


def usage():
    st = ROOT.statvfs()
    total = st.f_blocks * st.f_frsize
    used = (st.f_blocks - st.f_bfree) * st.f_frsize
    require(total == CAPACITY and 0 <= used < total, 'unexpected Event mount capacity or full mount')
    return {'total_bytes': total, 'used_bytes': used, 'sample_ns': time.monotonic_ns()}


def allocate():
    scope()
    usage()
    directory = Path(subprocess.check_output(
        ['mktemp', '-d', '--', str(ROOT / 'multiexec-capacity.XXXXXXXX')],
        text=True, timeout=3,
    ).strip())
    scope(directory)
    (directory / 'event').mkdir(mode=0o700)
    print(directory)


def sample(path):
    scope(path)
    for _ in range(165):
        print(json.dumps(usage()), flush=True)
        if (path / 'stop').is_file():
            return
        time.sleep(2)
    raise TimeoutError('Event capacity sampler did not receive its stop marker')


def reclaim(path):
    scope(path)
    before = usage()
    subprocess.run(['rm', '-rf', '--one-file-system', '--', str(path)], check=True, timeout=20)
    require(not path.exists() and not path.is_symlink(), 'Event trial directory still exists')
    print(json.dumps({'path': str(path), 'before': before, 'after': usage(), 'removed': True}))


def validate(arm, executors, batch):
    roles = ['hog11', 'hog8', 'hog4', 'hog5', 'hog7', 'hog6'][:3 + executors]
    require((arm / 'roles.txt').read_text().splitlines() ==
            [token for host in roles for token in ('-s', host)], f'wrong role mapping in {arm}')
    log = (arm / 'runner.log').read_text()
    case = (f'txs=1000000 nexecutors={executors} nconnections={executors * 10} '
            f'ncoros_per_connection=100 nacc=100 batch_size={batch}')
    require(re.findall(r'^Start trial (\d+) with ' + case + '$', log, re.M) == list('01234'),
            f'wrong trial contract in {arm}')
    timings = re.findall(r'^time taken: (\S+) ns for ' + case + r' tps=(\S+)$', log, re.M)
    require(len(timings) == 5, f'incomplete timings in {arm}')
    for elapsed, tps in timings:
        require(math.isfinite(float(elapsed)) and float(elapsed) > 0 and
                math.isclose(float(tps), 1e15 / float(elapsed), rel_tol=1e-6),
                f'invalid elapsed/tps in {arm}')
    digests = re.findall(r'^multiexec_capacity_workload_sha256=([a-f0-9]{64})$', log, re.M)
    require(len(digests) == 1, f'missing corpus digest in {arm}')
    indexer = re.findall(r'^indexer_latency_summary: (.+)$', log, re.M)
    require(len(indexer) == 1 and indexer[0].startswith(
        f'label=nexec={executors},batch={batch} blocks=7 unobserved=0 '), f'incomplete committed blocks in {arm}')
    samples = [json.loads(line) for line in (arm / 'samples.jsonl').read_text().splitlines()]
    require(2 <= len(samples) <= 165 and all(x['total_bytes'] == CAPACITY and
            0 <= x['used_bytes'] < CAPACITY for x in samples), f'invalid occupancy samples in {arm}')
    require(all(a['sample_ns'] < b['sample_ns'] for a, b in zip(samples, samples[1:])),
            f'invalid sample chronology in {arm}')
    cleanup = json.loads((arm / 'cleanup.json').read_text())
    baseline = samples[0]['used_bytes']
    require(cleanup['path'] == (arm / 'path.txt').read_text().strip() and cleanup['removed'] is True
            and cleanup['after']['used_bytes'] == baseline
            and cleanup['before']['total_bytes'] == CAPACITY == cleanup['after']['total_bytes'],
            f'incomplete Event scratch reclamation in {arm}')
    peak = max(cleanup['before']['used_bytes'], *(x['used_bytes'] for x in samples))
    require(baseline < peak < CAPACITY, f'no valid scratch occupancy in {arm}')
    return {'executors': executors, 'batch': batch, 'blocks': 7, 'measured_blocks': 5,
            'corpus_sha256': digests[0], 'elapsed_ns': [float(t[0]) for t in timings],
            'cap_bytes': CAPACITY, 'baseline_bytes': baseline, 'peak_sampled_bytes': peak,
            'after_bytes': cleanup['after']['used_bytes'], 'samples': len(samples)}


def main():
    operation, *args = sys.argv[1:]
    if operation == 'allocate':
        require(not args, 'invalid allocate invocation')
        allocate()
    elif operation in ('sample', 'reclaim'):
        path, = args
        (sample if operation == 'sample' else reclaim)(Path(path))
    elif operation == 'validate':
        path, executors, batch = args
        validate(Path(path), int(executors), int(batch))
    elif operation == 'summarize':
        path, = args
        root = Path(path)
        result = [validate(root / f'{executors}-exec-batch-{batch}', executors, batch)
                  for executors in (1, 2, 3) for batch in (1, 5, 10)]
        print(json.dumps(result, indent=2))
    else:
        raise ValueError('invalid capacity operation')


if __name__ == '__main__':
    main()
