#!/usr/bin/env python3
"""Sums the Switch host's profile (port/switch/host/host_profile.c) by function.

The host logs, every 10 seconds, the addresses the game's main thread was
found at most. This reads those lines from a log (host.log, or the output of
tools/switch_docker.sh nxlink), symbolizes them - addresses below 4 GB in the
guest image, the others in the host program, from the base the log gives -
and prints the functions by their share of the samples.

    python3 tools/switch_profile.py build/switch-logs/boot-4.txt [--last N]

Run it where llvm-symbolizer and llvm-nm are (the build container), after
the build that made the log.
"""

import argparse
import collections
import re
import subprocess
import sys

GUEST = "build/switch/halo_guest.elf"
HOST = "build/switch/halo.elf"


def symbol_address(elf: str, name: str) -> int:
    output = subprocess.run(["llvm-nm", elf], capture_output=True, text=True, check=True).stdout
    for line in output.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit(f"{name} is not in {elf}")


def symbolize(elf: str, addresses: list) -> dict:
    if not addresses:
        return {}
    request = "\n".join(f"0x{address:x}" for address in addresses) + "\n"
    output = subprocess.run(["llvm-symbolizer", f"--obj={elf}", "--functions=linkage", "--no-inlines"],
                            input=request, capture_output=True, text=True, check=True).stdout
    names = [block.splitlines()[0] for block in output.strip().split("\n\n")]
    return dict(zip(addresses, names))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--last", type=int, default=0, help="only the last N reports")
    args = parser.parse_args()

    text = open(args.log, "rb").read().decode("utf-8", "replace")
    reports = []
    for line in text.splitlines():
        header = re.search(r"profile: (\d+) samples, (\d+)% waiting in the kernel; host main at ([0-9a-f]+)", line)
        if header:
            reports.append({"samples": int(header.group(1)), "waiting": int(header.group(2)),
                            "main": int(header.group(3), 16), "counts": collections.Counter(),
                            "callers": collections.Counter()})
            continue
        table = re.search(r"profile( callers)?:(( [0-9a-f]+:\d+)+)\s*$", line)
        if reports and table:
            target = reports[-1]["callers" if table.group(1) else "counts"]
            for address, count in re.findall(r" ([0-9a-f]+):(\d+)", table.group(2)):
                target[int(address, 16)] += int(count)
    if args.last:
        reports = reports[-args.last:]
    if not reports:
        sys.exit("no profile in the log")

    host_offset = reports[-1]["main"] - symbol_address(HOST, "main")
    counts = collections.Counter()
    for report in reports:
        counts.update(report["counts"])
    guest_addresses = [a for a in counts if a < 1 << 32]
    host_addresses = [a for a in counts if a >= 1 << 32]
    names = symbolize(GUEST, guest_addresses)
    host_names = symbolize(HOST, [a - host_offset for a in host_addresses])
    for address in host_addresses:
        names[address] = "host:" + host_names.get(address - host_offset, "?")

    by_function = collections.Counter()
    for address, count in counts.items():
        by_function[names.get(address, "?")] += count
    samples = sum(report["samples"] for report in reports)
    waiting = sum(report["samples"] * report["waiting"] // 100 for report in reports)
    sampled = sum(counts.values())
    print(f"{len(reports)} reports, {samples} samples, {waiting} waiting in the kernel; "
          f"{sampled} in the top addresses")
    for name, count in by_function.most_common(40):
        print(f"{100.0 * count / samples:5.1f}%  {name}")

    callers = collections.Counter()
    for report in reports:
        callers.update(report["callers"])
    caller_names = symbolize(GUEST, [a for a in callers if a > 1 and a < 1 << 32])
    by_caller = collections.Counter()
    for address, count in callers.items():
        by_caller[caller_names.get(address, "(no guest caller)")] += count
    print("\nsamples in the host, by the guest function that called it:")
    for name, count in by_caller.most_common(25):
        print(f"{100.0 * count / samples:5.1f}%  {name}")


if __name__ == "__main__":
    main()
