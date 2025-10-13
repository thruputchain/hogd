#!/usr/bin/env python3

# If you have a huge number of threads, mostly waiting in some
# dispatch loop, you may want to identify a single problem thread that
# is deadlocked or doing something different.  
#
# For GDB, run the command:
#    thread apply all bt
# (press c to continue to print all the output).  
#
# LLDB is a pain to get proper file output from within lldb.
# Easiest way to do this is to run
# lldb -p 66311 -o "thread backtrace all" -o "quit" 2>&1 | tee <logfile>
#
# Then save the output to a file and run this script on that file.  
# It will group threads by backtrace so you only need to examine one 
# thread from each group.

import sys
import re
from collections import defaultdict

# Match the thread header line and the function name part of a stack frame
# GDB format: "Thread 1 " 
gdb_thread_re = re.compile(r"^Thread (\d+) ")
gdb_frame_re = re.compile(r"^#\d+\s+(?:0x[0-9a-f]+)?\s*in\s+([^\s(]+)")

# LLDB format: "  thread #1" or "* thread #1, queue = '...', stop reason = ..."
lldb_thread_re = re.compile(r"^\s*\*?\s*thread #(\d+)")
# LLDB frame format: "    frame #0: 0x... binary_name`function_name + offset"
# Extract function name, which can contain :: and other characters, up to ( or + or space
lldb_frame_re = re.compile(r"^\s*\*?\s*frame #\d+:.*?`([^`(+\s]+)")

def parse_bt_file(filename):
    groups = defaultdict(list)
    current_thread = None
    current_trace = []
    lldb_backtrace_started = False

    def record_current():
        if current_thread is not None and current_trace:
            key = tuple(current_trace)
            groups[key].append(current_thread)

    with open(filename) as f:
        for line in f:
            line = line.rstrip()
            
            # Check if we've reached the LLDB backtrace all command
            if "thread backtrace all" in line:
                lldb_backtrace_started = True
                continue

            # Try GDB thread format first
            m = gdb_thread_re.match(line)
            if m:
                record_current()
                current_trace = []
                current_thread = int(m.group(1))
                continue
            
            # Try LLDB thread format - but only after backtrace all command
            m = lldb_thread_re.match(line)
            if m and lldb_backtrace_started:
                record_current()
                current_trace = []
                current_thread = int(m.group(1))
                continue
            
            # Try GDB frame format
            m = gdb_frame_re.match(line)
            if m:
                func = m.group(1)
                current_trace.append(func)
                continue
                
            # Try LLDB frame format
            m = lldb_frame_re.match(line)
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
