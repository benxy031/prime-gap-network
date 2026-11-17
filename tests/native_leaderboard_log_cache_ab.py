"""Replay one authenticated history on released and candidate Core, then compare exact RPCs."""
from argparse import ArgumentParser
from contextlib import ExitStack
from hashlib import sha256
from pathlib import Path
from statistics import median, quantiles
from struct import unpack
import json
import tempfile
import time

from native_grouped249_actual import Node, file_sha

WINDOWS = ((480,512),(1168,1200),(2517,2549))

def blocks(path):
    with path.open("rb") as stream:
        assert stream.read(23) == b"PGN-PUBLIC-HISTORY-249\x00"
        expected = 0
        while header := stream.read(8):
            height, size = unpack("<II", header)
            identity = stream.read(32).hex()
            raw = stream.read(size)
            assert height == expected and len(raw) == size and 0 < size <= 4000000
            yield height, identity, raw
            expected += 1
        assert expected == 2550

def exercise(frame, binary, folder):
    times = []
    digest = sha256()
    with ExitStack() as cleanup:
        node = Node(binary,folder)
        cleanup.callback(node.close)
        end = None
        for height,identity,raw in frame:
            if height:
                assert node.rpc("submitblock",[raw.hex()]) is None
            window=next((w for w in WINDOWS if w[0]<=height<=w[1]),None)
            if window is None:continue
            start = time.perf_counter_ns()
            page = node.rpc("getpgnleaderboard",[0,100])
            elapsed = (time.perf_counter_ns()-start) / 1e6
            assert page["schema"]=="PGN_LEADERBOARD_V1" and page["height"]==height and page["bestblockhash"]==identity
            digest.update(json.dumps(page,sort_keys=True,separators=(",",":")).encode("utf-8")+b"\n")
            if height > window[0]:
                times.append(elapsed)
            end = page
        assert end is not None and end["height"]==2549
        node.close()
        node.start()
        assert node.rpc("getpgnleaderboard",[0,100])==end
        assert node.rpc("verifychain",[4,50]) is True
    return {"blocks":len(frame),"windows":WINDOWS,"warm_sample":len(times),"median_ms":median(times),
            "mean_ms":sum(times)/len(times),"p95_ms":quantiles(times,n=100,method="inclusive")[94],
            "full_projection_sha256":digest.hexdigest(),"cold_same":True,"verifychain_level4_last50":True,
            "height":end["height"],"final_rows":len(end["rows"])}

def main():
    args = ArgumentParser()
    for name in ("baseline","candidate","history","receipt"):
        args.add_argument("--"+name,type=Path,required=True)
    conf = args.parse_args()
    assert file_sha(conf.history)=="09f318bfa826fad3d2af199ead9bb321dad039308864e549e3b92e6db6cfb7e4"
    baseline = conf.baseline.resolve()
    candidate = conf.candidate.resolve()
    assert file_sha(baseline)=="a6187b4af5f7c164eeaa8b3a5976823aeb03543ca75c5a4feb452091df5bb6d4"
    assert file_sha(candidate)!=file_sha(baseline)
    frame=list(blocks(conf.history))
    sequence = [("baseline",baseline),("candidate",candidate),("candidate",candidate),("baseline",baseline)]
    results=[]
    with tempfile.TemporaryDirectory(prefix="pgn-audit-ab-") as folder:
        for ordinal,(name,binary) in enumerate(sequence):
            result=exercise(frame,binary,Path(folder)/str(ordinal))
            report=dict(result,name=name,core_sha256=file_sha(binary))
            results.append(report)
            print(json.dumps(report),flush=True)
    assert len({row["full_projection_sha256"] for row in results})==1
    old=median(row["median_ms"] for row in results if row["name"]=="baseline")
    new=median(row["median_ms"] for row in results if row["name"]=="candidate")
    proof={"PASS":True,"runs":results,"baseline_rpc_median_ms":old,"candidate_rpc_median_ms":new,
           "median_latency_delta_percent":100*(new/old-1),
           "scope":"Core29 versus exact log memo on authentic 2550-block history, three paired direct-child windows. Time covers total leaderboard RPC and excludes block submission. All projections and final cold repeat must match."}
    conf.receipt.parent.mkdir(parents=True,exist_ok=True)
    conf.receipt.write_text(json.dumps(proof,indent=2)+"\n",encoding="utf-8")

if __name__=="__main__":
    main()
