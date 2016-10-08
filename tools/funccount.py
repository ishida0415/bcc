#!/usr/bin/python
# @lint-avoid-python-3-compatibility-imports
#
# funccount Count functions, tracepoints, and USDT probes.
#           For Linux, uses BCC, eBPF.
#
# USAGE: funccount [-h] [-p PID] [-i INTERVAL] [-T] [-r] pattern
#
# The pattern is a string with optional '*' wildcards, similar to file
# globbing. If you'd prefer to use regular expressions, use the -r option.
#
# Copyright (c) 2015 Brendan Gregg.
# Licensed under the Apache License, Version 2.0 (the "License")
#
# 09-Sep-2015   Brendan Gregg       Created this.
# 18-Oct-2016   Sasha Goldshtein    Generalized for uprobes, tracepoints, USDT.

from __future__ import print_function
from bcc import BPF, USDT
from time import sleep, strftime
import argparse
import os
import re
import signal
import sys
import traceback

debug = False

class Probe(object):
    def __init__(self, pattern, use_regex=False, pid=None):
        """Init a new probe.

        Init the probe from the pattern provided by the user. The supported
        patterns mimic the 'trace' and 'argdist' tools, but are simpler because
        we don't have to distinguish between probes and retprobes.

            func            -- probe a kernel function
            lib:func        -- probe a user-space function in the library 'lib'
            p::func         -- same thing as 'func'
            p:lib:func      -- same thing as 'lib:func'
            t:cat:event     -- probe a kernel tracepoint
            u:lib:probe     -- probe a USDT tracepoint
        """
        parts = pattern.split(':')
        if len(parts) == 1:
            parts = ["p", "", parts[0]]
        elif len(parts) == 2:
            parts = ["p", parts[0], parts[1]]
        elif len(parts) == 3:
            if parts[0] == "t":
                parts = ["t", "", "%s:%s" % tuple(parts[1:])]
            if parts[0] not in ["p", "t", "u"]:
                raise Exception("Type must be 'p', 't', or 'u', but got %s" %
                                parts[0])
        else:
            raise Exception("Too many ':'-separated components in pattern %s" %
                            pattern)

        (self.type, self.library, self.pattern) = parts
        if not use_regex:
            self.pattern = self.pattern.replace('*', '.*')
            self.pattern = '^' + self.pattern + '$'

        if (self.type == "p" and self.library) or self.type == "u":
            libpath = BPF.find_library(self.library)
            if libpath is None:
                # This might be an executable (e.g. 'bash')
                libpath = BPF.find_exe(self.library)
            if libpath is None or len(libpath) == 0:
                raise Exception("unable to find library %s" % self.library)
            self.library = libpath

        self.pid = pid
        self.matched = 0
        self.trace_functions = {}   # map location number to function name

    def is_kernel_probe(self):
        return self.type == "t" or (self.type == "p" and self.library == "")

    def attach(self):
        if self.type == "p" and not self.library:
            for index, function in self.trace_functions.items():
                self.bpf.attach_kprobe(
                        event=function,
                        fn_name="trace_count_%d" % index,
                        pid=self.pid or -1)
        elif self.type == "p" and self.library:
            for index, function in self.trace_functions.items():
                self.bpf.attach_uprobe(
                        name=self.library,
                        sym=function,
                        fn_name="trace_count_%d" % index,
                        pid=self.pid or -1)
        elif self.type == "t":
            for index, function in self.trace_functions.items():
                self.bpf.attach_tracepoint(
                        tp=function,
                        fn_name="trace_count_%d" % index,
                        pid=self.pid or -1)
        elif self.type == "u":
            pass    # Nothing to do -- attach already happened in `load`

        if self.matched == 0:
            raise Exception("No functions matched by pattern %s" %
                            self.pattern)

    def _add_function(self, template, probe_name):
        new_func = "trace_count_%d" % self.matched
        text = template.replace("PROBE_FUNCTION", new_func)
        text = text.replace("LOCATION", str(self.matched))
        self.trace_functions[self.matched] = probe_name
        self.matched += 1
        return text

    def _generate_functions(self, template):
        self.usdt = None
        text = ""
        if self.type == "p" and not self.library:
            for function in BPF.get_kprobe_functions(self.pattern):
                text += self._add_function(template, function)
        elif self.type == "p" and self.library:
            # uprobes are tricky because the same function may have multiple
            # addresses, and the same address may be mapped to multiple
            # functions. We aren't allowed to create more than one uprobe
            # per address, so track unique addresses and ignore functions that
            # map to an address that we've already seen. Also ignore functions
            # that may repeat multiple times with different addresses.
            addresses, functions = (set(), set())
            for function, address in BPF.get_user_functions_and_addresses(
                                        self.library, self.pattern):
                if address in addresses or function in functions:
                    continue
                addresses.add(address)
                functions.add(function)
                text += self._add_function(template, function)
        elif self.type == "t":
            for tracepoint in BPF.get_tracepoints(self.pattern):
                text += self._add_function(template, tracepoint)
        elif self.type == "u":
            self.usdt = USDT(path=self.library, pid=self.pid)
            for probe in self.usdt.enumerate_probes():
                if not self.pid and (probe.bin_path != self.library):
                    continue
                if re.match(self.pattern, probe.name):
                    new_func = "trace_count_%d" % self.matched
                    text += self._add_function(template, probe.name)
                    self.usdt.enable_probe(probe.name, new_func)
            if debug:
                print(self.usdt.get_text())
        return text

    def load(self):
        trace_count_text = """
int PROBE_FUNCTION(void *ctx) {
    FILTER
    u64 loc = LOCATION;
    u64 *val = counts.lookup(&loc);     // prepopulated on Python side
    if (val) {
        (*val)++;
    }
    return 0;
}
        """
        bpf_text = """#include <uapi/linux/ptrace.h>

BPF_HASH(counts, u64, u64);     // map location number to number of calls

        """

        # We really mean the tgid from the kernel's perspective, which is in
        # the top 32 bits of bpf_get_current_pid_tgid().
        if self.pid:
            trace_count_text = trace_count_text.replace('FILTER',
                """u32 pid = bpf_get_current_pid_tgid() >> 32;
                   if (pid != %d) { return 0; }""" % self.pid)
        else:
            trace_count_text = trace_count_text.replace('FILTER', '')

        bpf_text += self._generate_functions(trace_count_text)
        if debug:
            print(bpf_text)

        self.bpf = BPF(text=bpf_text,
                       usdt_contexts=[self.usdt] if self.usdt else [])

        # Initialize all map entries to zero
        counts = self.bpf["counts"]
        for location, function in self.trace_functions.items():
            counts[counts.Key(location)] = counts.Leaf()

class Tool(object):
    def __init__(self):
        examples = """examples:
    ./funccount 'vfs_*'             # count kernel fns starting with "vfs"
    ./funccount -r '^vfs.*'         # same as above, using regular expressions
    ./funccount -Ti 5 'vfs_*'       # output every 5 seconds, with timestamps
    ./funccount -p 185 'vfs_*'      # count vfs calls for PID 181 only
    ./funccount t:sched:sched_fork  # count calls to the sched_fork tracepoint
    ./funccount -p 185 u:node:gc*   # count all GC USDT probes in node
    ./funccount c:malloc            # count all malloc() calls in libc
    """
        parser = argparse.ArgumentParser(
            description="Count functions, tracepoints, and USDT probes",
            formatter_class=argparse.RawDescriptionHelpFormatter,
            epilog=examples)
        parser.add_argument("-p", "--pid", type=int,
            help="trace this PID only")
        parser.add_argument("-i", "--interval", default=99999999,
            help="summary interval, seconds")
        parser.add_argument("-T", "--timestamp", action="store_true",
            help="include timestamp on output")
        parser.add_argument("-r", "--regexp", action="store_true",
            help="use regular expressions. Default is \"*\" wildcards only.")
        parser.add_argument("-d", "--debug", action="store_true",
            help="print BPF program before starting (for debugging purposes)")
        parser.add_argument("pattern",
            help="search expression for events")
        self.args = parser.parse_args()
        global debug
        debug = self.args.debug
        self.probe = Probe(self.args.pattern, self.args.regexp, self.args.pid)

    @staticmethod
    def _signal_ignore(signal, frame):
        print()

    def run(self):
        self.probe.load()
        self.probe.attach()
        print("Tracing %d functions for \"%s\"... Hit Ctrl-C to end." %
              (self.probe.matched, self.args.pattern))
        exiting = 0 if self.args.interval else 1
        while True:
            try:
                sleep(int(self.args.interval))
            except KeyboardInterrupt:
                exiting = 1
                # as cleanup can take many seconds, trap Ctrl-C:
                signal.signal(signal.SIGINT, Tool._signal_ignore)

            print()
            if self.args.timestamp:
                print("%-8s\n" % strftime("%H:%M:%S"), end="")

            print("%-36s %8s" % ("FUNC", "COUNT"))
            counts = self.probe.bpf["counts"]
            for k, v in sorted(counts.items(),
                               key=lambda counts: counts[1].value):
                if v.value == 0:
                    continue
                print("%-36s %8d" %
                      (self.probe.trace_functions[k.value], v.value))
            counts.zero()

            if exiting:
                print("Detaching...")
                exit()

if __name__ == "__main__":
    try:
        Tool().run()
    except Exception:
        if debug:
            traceback.print_exc()
        elif sys.exc_info()[0] is not SystemExit:
            print(sys.exc_info()[1])
