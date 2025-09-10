#!/usr/bin/env python3

# If you have a huge number of threads, mostly waiting in some
# dispatch loop, you may want to identify a single problem thread that
# is deadlocked or doing something different.  In gdb, run the
# command:
#
#    thread apply all bt
#
# (press c to continue to print all the output).  Then save the output
# of the thread apply all command to a file and run this script on
# that file.  It will group threads by backtrace so you only need to
# examine one thread from each group.

import sys
import re
from collections import defaultdict

# Match the thread header line and the function name part of a stack frame
thread_re = re.compile(r"^Thread (\d+) ")
frame_re = re.compile(r"^#\d+\s+(?:0x[0-9a-f]+)?\s*in\s+([^\s(]+)")

def parse_bt_file(filename):
    groups = defaultdict(list)
    current_thread = None
    current_trace = []

    def record_current():
        if current_thread is not None and current_trace:
            key = tuple(current_trace)
            groups[key].append(current_thread)

    with open(filename) as f:
        for line in f:
            line = line.rstrip()

            m = thread_re.match(line)
            if m:
                record_current()
                current_trace = []
                current_thread = int(m.group(1))
            else:
                m = frame_re.match(line)
                if m:
                    func = m.group(1)
                    current_trace.append(func)

        # Final thread
        record_current()

    return groups

def main():
    if len(sys.argv) != 2:
        print("Usage: group_threads_by_bt.py thread_bt.txt", file=sys.stderr)
        sys.exit(1)

    groups = parse_bt_file(sys.argv[1])
    for i, (trace, threads) in enumerate(groups.items(), 1):
        print(f"=== Group {i} ===")
        print(f"Threads: {', '.join(map(str, threads))}")
        print()

if __name__ == "__main__":
    main()
