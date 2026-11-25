#!/usr/bin/env python3
"""Check the pay-once rule over a local chain: one range number, one block.

This is the qualification check for the phase 1 rule, not part of consensus. It
reads connected blocks over RPC, decodes every PGNGOLD-COUNTS-1 envelope in each
coinbase, and fails if a range number appears twice, if an envelope is not
canonical, or if a block declares more than one.

The declaration is the 218-byte counts header: nVersion with bit 0x40000000,
then the usual 80 bytes, then a six-byte little-endian range number and 100
one-byte partition counts, then nNonce. A coinbase envelope, when present, must
agree with it byte for byte.

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


def declare(block, raw):
    """(range_number, counts) from the header section, or None.

    `raw` is the serialized block; verbosity 2 does not return it, so the caller
    fetches it only for blocks whose version selects the section.
    """
    header = decode_header(block, raw)
    coinbase = None
    for output in block["tx"][0]["vout"]:
        decoded = decode(output["scriptPubKey"]["hex"], output["value"])
        if decoded is None:
            continue
        if coinbase is not None:
            raise ValueError("more than one gold declaration in the coinbase")
        coinbase = decoded
    if header is None:
        if coinbase is not None:
            # On an activated chain a declaration without a header has no
            # committed home; the node refuses it, so a chain that contains one
            # was not mined by this rule.
            raise ValueError("coinbase declaration without a counts header")
        return None
    if coinbase is not None and list(coinbase[1]) != list(header[1]):
        raise ValueError("header counts differ from the coinbase envelope")
    return header


def has_counts_header(block):
    return bool(int(block["versionHex"], 16) & 0x40000000)


def decode_header(block, raw):
    if not has_counts_header(block):
        return None
    if len(raw) < 218:
        raise ValueError("counts header truncated")
    section = raw[80:80 + 106]
    return int.from_bytes(section[0:6], "little"), list(section[6:])


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
        try:
            raw = bytes.fromhex(rpc("getblock", [block["hash"], 0])) if has_counts_header(block) else b""
            declared = declare(block, raw)
        except ValueError as failure:
            malformed.append({"height": height, "reason": str(failure)})
            continue
        if declared is None:
            continue
        gold += 1
        number, counts = declared
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
