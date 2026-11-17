"""Check a Windows archive can be reopened by Core29 with its original LevelDB reader."""
from argparse import ArgumentParser
from hashlib import sha256
from pathlib import Path
from struct import unpack
import json
import os
import tempfile
import time

from native_grouped249_actual import Node, file_sha

def replay(node,path):
    with path.open("rb") as stream:
        assert stream.read(23)==b"PGN-PUBLIC-HISTORY-249\x00"
        expected=0
        while header:=stream.read(8):
            height,size=unpack("<II",header)
            identity=stream.read(32).hex()
            block=stream.read(size)
            assert height==expected and len(block)==size and 0<size<=4000000
            if height:
                assert node.rpc("submitblock",[block.hex()]) is None
            expected+=1
        assert expected==2550 and node.rpc("getbestblockhash")==identity
    return identity

def state(node,tip):
    board=node.rpc("getpgnleaderboard",[0,100])
    audit=node.rpc("getpgnauditactivity",[0,64])
    assert board["height"]==audit["state"]["height"]==2549
    assert board["bestblockhash"]==audit["state"]["bestblockhash"]==tip
    return board,audit

def main():
    cli=ArgumentParser()
    for field in ("core","released","history","receipt"):
        cli.add_argument("--"+field,type=Path,required=True)
    args=cli.parse_args()
    assert os.name=="nt" and file_sha(args.released)=="6e0d827229ab23fc36f0957f53e43548f6f05437d826b15e5ed98ea0825335ce"
    assert file_sha(args.history)=="09f318bfa826fad3d2af199ead9bb321dad039308864e549e3b92e6db6cfb7e4"
    begin=time.monotonic()
    with tempfile.TemporaryDirectory(prefix="pgn-archive-fs-") as tmp:
        data=Path(tmp)/"node"
        writer=Node(args.core.resolve(),data)
        try:
            tip=replay(writer,args.history)
            before=state(writer,tip)
        finally:
            writer.close()
        archive=data/"testnet/chainstate-pgn-request-history"
        assert archive.stat().st_file_attributes&0x800
        tables=sorted(archive.glob("*.ldb"))
        assert tables and all(p.stat().st_file_attributes&0x800 for p in tables)
        assert not (data/"testnet/chainstate").stat().st_file_attributes&0x800
        reader=Node(args.released.resolve(),data)
        try:
            after=state(reader,tip)
            assert after==before and reader.rpc("verifychain",[4,50]) is True
        finally:
            reader.close()
        result=dict(PASS=True,windows_archive_new_files_compressed=True,chainstate_compressed=False,
                    source_core=file_sha(args.core),rollback_core=file_sha(args.released),
                    current_branch_unchanged=True,identical_after_original_reader_reopen=True,
                    archive_tables=len(tables),projection_sha256=sha256(json.dumps(before,sort_keys=True).encode()).hexdigest(),
                    validated_height=2549,verifychain4_last50=True,seconds=time.monotonic()-begin)
    args.receipt.parent.mkdir(parents=True,exist_ok=True)
    args.receipt.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(result),flush=True)

if __name__=="__main__":
    main()
