#!/usr/bin/env python3
"""Collect bounded full-validation observations from a stopped role log."""

import json
import os
from pathlib import Path
import re
import stat
import sys

PREFIX = 'HISTORICAL_VALIDATE_'
TMP_LOG = re.compile(r'/tmp/hogd_stderr_[0-9]{1,24}_[0-9]{1,12}_[A-Za-z0-9]{6}')
ANSI = re.compile(rb'\x1b\[[0-9;]*m')
FIELDS = ('handler_ns', 'before_dag_ns', 'decode_ns', 'chunks_ns', 'db_close_ns',
          'event_close_ns', 'result_ns', 'check_closed_ns', 'proposal_hash_ns')
RECEIPTS = {
    'client': re.compile(r'HISTORICAL_VALIDATE_CLIENT height=(?P<height>\d+)'
                         r' value=(?P<value>[a-f0-9]{12}) block=(?P<block>\d+)'
                         r' txs=1000000 attempt=(?P<attempt>\d+) call_ns=(?P<call_ns>\d+)'
                         r' trace=(?P<trace>[a-z]{1,16})$'),
    'server': re.compile(r'HISTORICAL_VALIDATE_SERVER height=(?P<height>\d+)'
                         r' block=(?P<block>\d+) txs=1000000 ' +
                         ' '.join(fr'{key}=(?P<{key}>\d+)' for key in FIELDS) + '$'),
}
ROLES = {('client', '0'): 'hog4', ('client', '1'): 'hog8',
         ('server', '0'): 'hog7', ('server', '1'): 'hog11'}


def collect(path, kind):
    if not TMP_LOG.fullmatch(path) or os.path.realpath(path) != path:
        raise ValueError('unexpected control stderr path')
    rows = {}
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as stream:
        before = os.fstat(stream.fileno())
        if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid()
                or before.st_nlink != 1 or not 0 < before.st_size <= 512 << 20):
            raise ValueError('invalid control stderr')
        remaining = before.st_size
        while remaining:
            raw = stream.readline(min(65537, remaining))
            if not raw or len(raw) > 65536 or not raw.endswith(b'\n'):
                raise ValueError('oversized or incomplete control log line')
            remaining -= len(raw)
            if raw.startswith(b'[truncated by ci_truncate_log_to_tail:'):
                raise ValueError('expected the complete stopped log')
            if PREFIX.encode() not in raw:
                continue
            found = RECEIPTS[kind].search(ANSI.sub(b'', raw).decode(errors='replace').rstrip())
            if found is None or len(rows) == 128:
                raise ValueError('malformed receipt or sample limit')
            record = {key: value if key in ('value', 'trace') else int(value)
                      for key, value in found.groupdict().items()}
            key = record['block']
            if key in rows or key <= 0 or record['height'] <= 0:
                raise ValueError('duplicate or invalid validation key')
            if kind == 'client':
                if not 0 < record['attempt'] <= 128 or record['call_ns'] <= 0:
                    raise ValueError('invalid client receipt')
            elif any(not 0 < record[field] <= record['handler_ns'] for field in FIELDS):
                raise ValueError('invalid server receipt')
            rows[key] = record
        after = os.fstat(stream.fileno())
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError('control stderr changed during collection')
    if not rows:
        raise ValueError('missing validation observations')
    return list(rows.values())


def main():
    role, node, path = sys.argv[1:]
    host = ROLES[role, node]
    run = os.environ['HOG_XID']
    sha = os.environ['GIT_COMMIT']
    if not (os.environ.get('HOG_HOST') == host
            and os.environ.get('HOGD_CI_CONTAINER') == '1'
            and os.environ.get('HOGD_CI_OFFLINE') == '1'
            and os.environ.get('GIT_BRANCH') == 'codex/combined-native-multi-validation-phases'
            and re.fullmatch(r'hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*', run)
            and re.fullmatch(r'[a-f0-9]{40}', sha)):
        raise ValueError('unexpected historical validation role or mode')
    output = Path('/workspace/.ci/dumps')
    if output.resolve(strict=True) != output or output.stat().st_uid != os.getuid():
        raise ValueError('invalid diagnostic output directory')
    values = collect(path, role)
    dest = output / f'historical-validation-{role}.json'
    pending = dest.with_suffix('.pending')
    with pending.open('x') as stream:
        json.dump(dict(schema=1, host=host, node=int(node), kind=role, run=run,
                       source_sha=sha, rows=values), stream, separators=(',', ':'))
        stream.write('\n')
    try:
        os.link(pending, dest, follow_symlinks=False)
    finally:
        pending.unlink()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        detail = str(error) if isinstance(error, ValueError) else error.__class__.__name__
        print(f'Historical validation archive: {detail}', file=sys.stderr)
        sys.exit(1)
