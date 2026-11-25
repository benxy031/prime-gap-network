#!/usr/bin/env python3
"""Publish audit answers on a local chain and prove which ones the chain refuses.

The unit tests cover the ledger and the replay; this covers the admission call
that makes an answer consensus rather than a comment. Every case is published as
a real block, so a case that must be refused is proven refused by the chain and
not by a mocked call.

The answer travels as a zero-value PGNGOLDAUDIT-1 output, which is exactly what
getpgnbootstraptemplate copies into the coinbase, so it can be mined with the
GPU-free local generator.

Usage:
  tools/pgn_gold_audit_live.py --datadir /tmp/pgnl/A --rpcport 58952 --range 42
"""

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pgn_gold_audit import target  # noqa: E402
from pgn_local_blocks import Rpc, cookie_for, mine  # noqa: E402

GOLD_SECTION_COUNTS = bytes([1]) * 100


def audit_script(number, slot, partitions, counts):
    payload = (bytes([1]) + number.to_bytes(6, "little") + bytes([slot]) +
               bytes(partitions) + bytes(counts))
    assert len(payload) == 18
    script = bytes([0x6A, 0x0E]) + b"PGNGOLDAUDIT-1" + bytes([0x12]) + payload
    return script.hex()


def gold_section(number):
    return number.to_bytes(6, "little") + GOLD_SECTION_COUNTS


def case(name, expect, publish):
    """Run one case and record whether the chain agreed."""
    outcome = publish()
    accepted = outcome is not None
    return {"case": name, "expected": expect, "accepted": accepted,
            "block": outcome, "PASS": accepted == (expect == "accepted")}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--datadir", required=True, help="node data directory")
    parser.add_argument("--rpcport", required=True, type=int, help="node RPC port")
    parser.add_argument("--cookie", help="RPC cookie file (default: <datadir>/testnet/.cookie)")
    parser.add_argument("--range", required=True, type=int, help="range number to pay and audit")
    arguments = parser.parse_args(argv)

    rpc = Rpc(arguments.rpcport, cookie_for(arguments.datadir, arguments.cookie))
    number = arguments.range
    cases = []

    def submit(script):
        try:
            return mine(rpc, requests=[script])["block"]
        except RuntimeError as failure:
            if "rejected" not in str(failure):
                raise
            return None

    def publish(script):
        return submit(script)

    # The gold block and one successor, so the target exists at all.
    gold = mine(rpc, gold_section=gold_section(number))["block"]
    mine(rpc)
    declared = target(rpc, number)
    if not declared.get("auditable"):
        raise ValueError("range " + str(number) + " is not auditable after its seed block")

    partitions = declared["slot0_partitions"]
    counts = declared["slot0_counts"]
    wrong = list(counts)
    wrong[0] = (wrong[0] + 1) % 256

    cases.append(case("wrong count", "rejected", lambda: publish(
        audit_script(number, 0, partitions, wrong))))
    cases.append(case("unpaid range", "rejected", lambda: publish(
        audit_script(number + 1, 0, partitions, counts))))
    cases.append(case("honest slot 0", "accepted", lambda: publish(
        audit_script(number, 0, partitions, counts))))
    cases.append(case("slot 0 again", "rejected", lambda: publish(
        audit_script(number, 0, partitions, counts))))
    cases.append(case("honest slot 1", "accepted", lambda: publish(
        audit_script(number, 1, declared["slot1_partitions"], declared["slot1_counts"]))))

    recorded = target(rpc, number)
    failures = [entry for entry in cases if not entry["PASS"]]
    if not recorded["audited"]:
        failures.append({"case": "audited after both slots", "declared": recorded["audited"]})
    if recorded["answered_slots"] != 2:
        failures.append({"case": "answered slots", "declared": recorded["answered_slots"]})

    result = {"schema": "PGN_GOLD_AUDIT_LIVE_V1", "range_number": number, "gold_block": gold,
              "audited": recorded["audited"], "answered_slots": recorded["answered_slots"],
              "cases": cases, "failures": failures, "PASS": not failures}
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0 if result["PASS"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, KeyError, OSError, IndexError) as failure:
        print("pgn_gold_audit_live: " + str(failure), file=sys.stderr)
        sys.exit(2)
