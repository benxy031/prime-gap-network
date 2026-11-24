#!/usr/bin/env python3
"""Check the pay-once rule over a local chain: one range number, one block.

This is the qualification check for the phase 1 rule, not part of consensus. It
reads connected blocks over RPC, decodes every PGNGOLD-COUNTS-1 envelope in each
coinbase, and fails if a range number appears twice, if an envelope is not
canonical, or if a block declares more than one.

The encoding is pinned to the C++ codec in pgn/gold_counts_envelope.h:
OP_RETURN, one push of the tag, one push of 107 payload bytes (version 1, then a
six-byte little-endian range number and 100 one-byte partition counts), nothing
else, value zero.

Usage:
  tools/pgn_gold_counts_check.py --datadir /tmp/pgn-p1/A --rpcport 58952
  tools/pgn_gold_counts_check.py --datadir ... --rpcport ... --from-height 1
"""

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pgn_local_blocks import Rpc, cookie_for  # noqa: E402

TAG = b"PGNGOLD-COUNTS-1"
VERSION = 1
PAYLOAD = 1 + 6 + 100
MAX_SCRIPT = 4096


def pushed(raw, at):
    """Read one canonical data push; returns (data, next offset)."""
    if at >= len(raw):
        raise ValueError("truncated push")
    op = raw[at]
    at += 1
    if op < 76:
        size = op
    elif op == 76:
        if at + 1 > len(raw):
            raise ValueError("truncated push length")
        size = raw[at]
        at += 1
        if size < 76:
            raise ValueError("non-minimal push")
    elif op == 77:
        if at + 2 > len(raw):
            raise ValueError("truncated push length")
        size = int.from_bytes(raw[at:at + 2], "little")
        at += 2
        if size <= 0xff:
            raise ValueError("non-minimal push")
    else:
        raise ValueError("push opcode outside the canonical range")
    if at + size > len(raw):
        raise ValueError("truncated push body")
    return raw[at:at + size], at + size


def decode(script_hex, value):
    """Return (range_number, counts) or None when this is not a gold envelope."""
    raw = bytes.fromhex(script_hex)
    if len(raw) > MAX_SCRIPT or not raw or raw[0] != 0x6A:
        return None
    tag, at = pushed(raw, 1)
    if tag != TAG:
        return None
    payload, at = pushed(raw, at)
    if at != len(raw):
        raise ValueError("trailing bytes after the gold payload")
    if value != 0:
        raise ValueError("gold declaration is not a zero-value output")
    if len(payload) != PAYLOAD:
        raise ValueError("gold payload is not " + str(PAYLOAD) + " bytes")
    if payload[0] != VERSION:
        raise ValueError("unknown gold payload version")
    number = int.from_bytes(payload[1:7], "little")
    if len(payload[7:]) != 100:
        raise ValueError("gold payload does not carry 100 counts")
    return number, list(payload[7:])


def main(argv=None):
    parser = argparse.ArgumentParser(description="Verify gold range numbers appear at most once.")
    parser.add_argument("--datadir", required=True, help="node data directory")
    parser.add_argument("--rpcport", required=True, type=int, help="node RPC port")
    parser.add_argument("--cookie", help="RPC cookie file (default: <datadir>/testnet/.cookie)")
    parser.add_argument("--from-height", type=int, default=0, help="first height to check")
    parser.add_argument("--to-height", type=int, help="last height to check, default chain tip")
    arguments = parser.parse_args(argv)

    rpc = Rpc(arguments.rpcport, cookie_for(arguments.datadir, arguments.cookie))
    tip = rpc("getblockcount")
    first = arguments.from_height
    last = tip if arguments.to_height is None else arguments.to_height
    if not 0 <= first <= last <= tip:
        raise ValueError("height range must be inside 0.." + str(tip))

    owners, gold, malformed, duplicates = {}, 0, [], []
    for height in range(first, last + 1):
        block = rpc("getblock", [rpc("getblockhash", [height]), 2])
        coinbase = block["tx"][0]
        found = []
        for output in coinbase["vout"]:
            try:
                decoded = decode(output["scriptPubKey"]["hex"], output["value"])
            except ValueError as failure:
                malformed.append({"height": height, "reason": str(failure)})
                continue
            if decoded is not None:
                found.append(decoded)
        if len(found) > 1:
            malformed.append({"height": height, "reason": "more than one gold declaration"})
            continue
        if not found:
            continue
        gold += 1
        number, counts = found[0]
        if number in owners:
            duplicates.append({"range_number": number, "height": height,
                               "first_height": owners[number]["height"],
                               "counts_total": sum(counts)})
            continue
        owners[number] = {"height": height, "block": block["hash"], "counts_total": sum(counts)}

    result = {"schema": "PGN_GOLD_COUNTS_CHECK_V1", "tip": tip,
              "heights_checked": last - first + 1, "gold_blocks": gold,
              "distinct_range_numbers": len(owners), "duplicates": duplicates,
              "malformed": malformed,
              "PASS": not duplicates and not malformed}
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0 if result["PASS"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, KeyError, OSError) as failure:
        print("pgn_gold_counts_check: " + str(failure), file=sys.stderr)
        sys.exit(2)
