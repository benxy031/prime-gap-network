#!/usr/bin/env python3
"""Mine bootstrap blocks on a running local chain without a GPU.

Phase 0 of the coordination redesign gives us an isolated chain, but a chain
with only genesis cannot exercise a format change. Bootstrap headers are cheap
on purpose (bits 0x207fffff, nonce family (n & 0xffff) == 2 && (n >> 80) == 0),
so one block is a handful of SHA256d calls in Python instead of a GPU search.

The RPC always returns the same counter-zero template; the only field that
changes between attempts is the 64-bit counter inside nNonce. The block id of a
non-legacy header is SHA256d of its 112 serialized bytes, which is exactly the
prefix the template already contains, so patching the counter in place and
rehashing the header is the whole miner.

Usage:
  tools/pgn_local_blocks.py --datadir /tmp/pgn-local-phase0/A --rpcport 58952 --count 5
  tools/pgn_local_blocks.py --datadir ... --rpcport ... --dry-run
  tools/pgn_local_blocks.py --datadir ... --rpcport ... --request <canonical-script-hex>

This tool mines coordination/bootstrap blocks and, with --gold-section, the
counts-carrying header that declares a gold range. It cannot produce a report,
an audit or a spend. The declaration lives in the header only: the coinbase
envelope is no longer needed, and on an activated chain it is refused.
"""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import sys
import urllib.error
import urllib.request
import uuid

SCHEMA = "PGN_NATIVE112_BOOTSTRAP_TEMPLATE1"
HEADER_BYTES = 112
COUNTER_OFFSET = 82          # 4 version + 32 prev + 32 merkle + 8 time + 4 bits + 2 nonce tag
COUNTER_BYTES = 8
NONCE_TAG = 2                # (nonce & 0xffff) == 2 selects the canonical hash branch
MAX_COUNTER = 1 << 32        # ~2^-32 exhaustion probability against a 0x7fffff... target
GOLD_COUNTS_BIT = 0x40000000 # nVersion bit selecting the 218-byte header
GOLD_SECTION_BYTES = 106     # 6 range-number bytes + 100 partition counts


def cookie_for(datadir, explicit):
    if explicit:
        path = Path(explicit)
        if not path.is_file() or path.stat().st_size > 4096:
            raise ValueError("--cookie must name a bounded RPC cookie file")
        return path
    for candidate in (Path(datadir) / "testnet" / ".cookie", Path(datadir) / ".cookie"):
        if candidate.is_file() and candidate.stat().st_size <= 4096:
            return candidate
    raise ValueError("no RPC cookie under " + str(datadir) + "; pass --cookie")


class Rpc:
    def __init__(self, port, cookie):
        if not 1 <= port <= 65535:
            raise ValueError("--rpcport must be a TCP port")
        self.url = "http://127.0.0.1:" + str(port) + "/"
        self.authorization = base64.b64encode(cookie.read_bytes().strip()).decode("ascii")

    def __call__(self, method, parameters=None):
        body = json.dumps({"jsonrpc": "2.0", "id": str(uuid.uuid4()), "method": method,
                           "params": parameters or []}).encode("utf-8")
        request = urllib.request.Request(
            self.url, data=body,
            headers={"Content-Type": "application/json",
                     "Authorization": "Basic " + self.authorization},
        )
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                reply = json.loads(response.read())
        except urllib.error.HTTPError as failure:
            detail = failure.read().decode("utf-8", "replace")
            raise RuntimeError(method + " refused: " + detail[:400]) from None
        if reply.get("error") is not None:
            raise RuntimeError(method + " failed: " + json.dumps(reply["error"])[:400])
        return reply["result"]


def checked_template(template):
    """Reject anything that is not the fixed counter-zero bootstrap family."""
    if not isinstance(template, dict) or template.get("schema") != SCHEMA:
        raise ValueError("unexpected bootstrap template schema")
    header = bytes.fromhex(template["header"])
    block = bytearray(bytes.fromhex(template["block"]))
    target = int(template["numeric_target"], 16)
    if len(header) != HEADER_BYTES or not block.startswith(header):
        raise ValueError("template is not a 112-byte native header")
    if template["nonce_offset"] != COUNTER_OFFSET or template["nonce_bytes"] != COUNTER_BYTES:
        raise ValueError("template counter is not the expected 64-bit family")
    if header[80:82] != NONCE_TAG.to_bytes(2, "little"):
        raise ValueError("template nonce is outside the canonical family")
    if int.from_bytes(header[82:], "little") != 0:
        raise ValueError("template carries non-zero counter bits")
    if not 0 < target < 1 << 256:
        raise ValueError("template target is not a 256-bit number")
    return block, target


def with_gold_section(block, section):
    """Rebuild the block with a 218-byte counts header.

    The section goes between nBits and nNonce, so the counter keeps its family and
    only its offset moves. Nothing else in the block changes: the merkle root
    covers transactions, not the header.
    """
    if len(section) != GOLD_SECTION_BYTES:
        raise ValueError("--gold-section must be exactly " + str(GOLD_SECTION_BYTES) + " bytes")
    if len(block) < HEADER_BYTES:
        raise ValueError("template block is shorter than its header")
    version = int.from_bytes(block[0:4], "little") | GOLD_COUNTS_BIT
    header = (version.to_bytes(4, "little") + bytes(block[4:80]) +
              bytes(section) + bytes(block[80:HEADER_BYTES]))
    return bytearray(header + bytes(block[HEADER_BYTES:]))


def block_hash(block, header_bytes=HEADER_BYTES):
    """A gold header hashes from its 218 serialized bytes, everything else from 112."""
    return hashlib.sha256(hashlib.sha256(bytes(block[:header_bytes])).digest()).digest()


def search(block, target, limit, nonce_offset=COUNTER_OFFSET, header_bytes=HEADER_BYTES):
    for counter in range(limit):
        block[nonce_offset:nonce_offset + COUNTER_BYTES] = (
            (counter << 16 | NONCE_TAG).to_bytes(COUNTER_BYTES, "little"))
        digest = block_hash(block, header_bytes)
        if int.from_bytes(digest, "little") <= target:
            return counter, digest
    return None


def mine(rpc, requests=(), gold_section=None, max_counter=MAX_COUNTER, dry_run=False):
    """Mine one block from the current tip and, unless dry_run, submit it.

    A refusal comes back as RuntimeError carrying the node's own verdict, which is
    how a caller proves the chain rejected something rather than assuming it.
    """
    template = rpc("getpgnbootstraptemplate", [list(requests)] if requests else [])
    block, target = checked_template(template)
    nonce_offset, header_bytes = COUNTER_OFFSET, HEADER_BYTES
    if gold_section is not None:
        block = with_gold_section(block, gold_section)
        nonce_offset, header_bytes = COUNTER_OFFSET + GOLD_SECTION_BYTES, HEADER_BYTES + GOLD_SECTION_BYTES
    found = search(block, target, max_counter, nonce_offset, header_bytes)
    if found is None:
        raise RuntimeError("counter family exhausted at height " + str(template["height"]))
    counter, digest = found
    record = {"height": template["height"], "parent": template["parent"],
              "counter": counter, "block": digest[::-1].hex(),
              "bytes": len(block), "header_bytes": header_bytes,
              "submitted": not dry_run}
    if not dry_run:
        rejection = rpc("submitblock", [block.hex()])
        key = digest[::-1].hex()
        if rejection is not None:
            raise RuntimeError("block " + key + " rejected: " + str(rejection)[:200])
        if rpc("getblockhash", [template["height"]]) != key:
            raise RuntimeError("block " + key + " was not connected at height "
                               + str(template["height"]))
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description="Mine bootstrap blocks on a local PGN chain.")
    parser.add_argument("--datadir", required=True, help="node data directory")
    parser.add_argument("--rpcport", required=True, type=int, help="node RPC port")
    parser.add_argument("--cookie", help="RPC cookie file (default: <datadir>/testnet/.cookie)")
    parser.add_argument("--count", type=int, default=1, help="blocks to mine, default 1")
    parser.add_argument("--request", action="append", default=[], metavar="SCRIPT_HEX",
                        help="canonical signed request script, repeatable")
    parser.add_argument("--gold-section", metavar="HEX",
                        help="106-byte counts section (12 hex range number + 200 hex counts); "
                             "builds the 218-byte counts header")
    parser.add_argument("--dry-run", action="store_true", help="find the nonce, do not submit")
    parser.add_argument("--max-counter", type=int, default=MAX_COUNTER,
                        help="counter search bound, default 2^32")
    arguments = parser.parse_args(argv)

    if not 1 <= arguments.count <= 1000:
        raise ValueError("--count must be 1..1000")
    if not 1 <= arguments.max_counter <= MAX_COUNTER:
        raise ValueError("--max-counter must be 1..2^32")
    rpc = Rpc(arguments.rpcport, cookie_for(arguments.datadir, arguments.cookie))
    for _ in range(arguments.count):
        record = mine(rpc, requests=arguments.request,
                      gold_section=arguments.gold_section and bytes.fromhex(arguments.gold_section),
                      max_counter=arguments.max_counter, dry_run=arguments.dry_run)
        print(json.dumps(record, sort_keys=True, separators=(",", ":")), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, KeyError, OSError) as failure:
        print("pgn_local_blocks: " + str(failure), file=sys.stderr)
        sys.exit(1)
