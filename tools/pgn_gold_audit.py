#!/usr/bin/env python3
"""Exercise the counts audit end to end against a running node.

Two read-only RPCs back this: `getpgngoldaudit` returns the assignment for a
paid range number, and `verifypgngoldaudit` checks one answer against the counts
in the gold header. The verifier itself lives in pgn/gold_audit.h and is what
phase 2 will call from admission; this drives the same code over HTTP so it can
be proven on a live chain rather than only in a unit test.

The tool is a qualification harness, not an auditor: it reads the declared counts
from the header instead of rescanning the range, because rescanning needs the
native scanner over 2e14 positions. What it proves is the part that matters for
consensus - the selection, the comparison, and that neither can be faked.

Usage:
  tools/pgn_gold_audit.py --datadir /tmp/pgn-a/A --rpcport 58952 --range 100
  tools/pgn_gold_audit.py --datadir ... --rpcport ... --range 100 --show
"""

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pgn_local_blocks import Rpc, cookie_for  # noqa: E402

PASS = "verified"


def target(rpc, number):
    return rpc("getpgngoldaudit", [str(number)])


def verify(rpc, number, slot, partitions, counts):
    return rpc("verifypgngoldaudit", [str(number), slot, list(partitions), list(counts)])["verdict"]


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check a counts audit target and its verdicts.")
    parser.add_argument("--datadir", required=True, help="node data directory")
    parser.add_argument("--rpcport", required=True, type=int, help="node RPC port")
    parser.add_argument("--cookie", help="RPC cookie file (default: <datadir>/testnet/.cookie)")
    parser.add_argument("--range", required=True, type=int, help="paid range number")
    parser.add_argument("--show", action="store_true", help="print the assignment")
    arguments = parser.parse_args(argv)

    rpc = Rpc(arguments.rpcport, cookie_for(arguments.datadir, arguments.cookie))
    declared = target(rpc, arguments.range)
    if arguments.show:
        print(json.dumps(declared, sort_keys=True, indent=2))
    if not declared["active"]:
        raise ValueError("this chain does not commit the counts rule")
    if not declared["auditable"]:
        raise ValueError("range " + str(arguments.range) + " is unpaid or its seed block is missing")

    cases, failures = [], []
    for slot in (0, 1):
        partitions = declared["slot{}_partitions".format(slot)]
        counts = declared["slot{}_counts".format(slot)]
        cases.append(("honest slot " + str(slot), slot, partitions, counts, PASS))
        wrong_count = list(counts)
        wrong_count[0] = (wrong_count[0] + 1) % 256
        cases.append(("wrong count slot " + str(slot), slot, partitions, wrong_count, "wrong_count"))
        wrong_partition = list(partitions)
        wrong_partition[0] = (wrong_partition[0] + 1) % 100
        cases.append(("wrong partition slot " + str(slot), slot, wrong_partition, counts,
                      "wrong_partitions"))
        other = 1 - slot
        cases.append(("crossed slot " + str(slot), slot,
                      declared["slot{}_partitions".format(other)], counts, "wrong_partitions"))

    for name, slot, partitions, counts, expected in cases:
        actual = verify(rpc, arguments.range, slot, partitions, counts)
        if actual != expected:
            failures.append({"case": name, "expected": expected, "actual": actual})

    # Neither slot may answer with a partition set that was not assigned to it.
    overlap = set(declared["slot0_partitions"]) & set(declared["slot1_partitions"])
    if overlap:
        failures.append({"case": "slot overlap", "shared": sorted(overlap)})

    # The recorded state comes from the same replay admission will use, so it has to
    # agree with itself: a slot is answered exactly when it carries a block, and the
    # range is audited exactly when both slots are answered. An answer the replay
    # refused must be invisible here, not counted.
    answers = [declared["slot{}_answer".format(slot)] for slot in (0, 1)]
    recorded = sum(1 for answer in answers if answer)
    if recorded != declared["answered_slots"]:
        failures.append({"case": "answered slots", "declared": declared["answered_slots"],
                         "recorded": recorded})
    if declared["audited"] != (recorded == 2):
        failures.append({"case": "audited flag", "declared": declared["audited"],
                         "recorded": recorded})
    for slot, answer in enumerate(answers):
        if answer and (len(answer.get("block", "")) != 64 or answer.get("height", 0) <= 0):
            failures.append({"case": "slot " + str(slot) + " answer record", "answer": answer})

    result = {"schema": "PGN_GOLD_AUDIT_CHECK_V1", "range_number": arguments.range,
              "gold_height": declared["gold_height"], "seed": declared["seed"],
              "answered_slots": declared["answered_slots"], "audited": declared["audited"],
              "cases": len(cases) + 2, "failures": failures, "PASS": not failures}
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0 if result["PASS"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, KeyError, OSError, IndexError) as failure:
        print("pgn_gold_audit: " + str(failure), file=sys.stderr)
        sys.exit(2)
