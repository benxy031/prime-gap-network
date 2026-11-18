from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tempfile import TemporaryDirectory
from threading import Event, Thread
from types import SimpleNamespace
import json
import sys

tree=Path(sys.argv[1])
out=Path(sys.argv[2])
sys.path[:0]=[str(tree/"src/engine/runtime_host"),str(tree/"src/protocol")]
from horizon_core_rpc import CoreRpc, CoreRpcUnavailable
from horizon_network_worker import NetworkWorker, save

genesis="7"*64
request="2"*64
ticket="1"*64
lease=dict(request=request,state="assigned",start="100",end="120",issued=1,
           deadline_clock="median_chain_time",issued_time=1000,deadline_time=87400,ticket_number="35")

def probe(median):
    calls=[]
    messages=[]
    class Replies(BaseHTTPRequestHandler):
        def do_POST(self):
            method=json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            assert self.path=="/" and method["method"]=="getpgnstate" and method["params"]==[0,1,request]
            calls.append(method["method"])
            result=dict(genesis=genesis,height=5,median_time_past=median,leases=[lease])
            status=503 if len(calls)==1 else 200
            payload=json.dumps(dict(id=method["id"],result=result,error=None)).encode()
            self.send_response(status)
            self.send_header("Content-Length",str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        def log_message(self,*args):pass
    with TemporaryDirectory(prefix="isolated-lease-reader-") as temp, ThreadingHTTPServer(("127.0.0.1",0),Replies) as server:
        relay=Thread(target=server.serve_forever);relay.start()
        try:
            worker=NetworkWorker.__new__(NetworkWorker)
            worker.root=Path(temp)
            worker.args=SimpleNamespace(genesis=genesis,wait_for_network_sync=False)
            worker.quitting=Event()
            worker.message=lambda text,phase:messages.append(phase)
            cookie=worker.root/"qa-cookie"
            cookie.write_text("fixture:local-fixture-cookie",encoding="utf8")
            worker.rpc=CoreRpc(server.server_port,cookie,genesis,"fixture")
            journal=worker.root/"controller.json"
            save(journal,dict(stage="ASSIGNED",sequence=4,work=dict(request_id=request,ticket=ticket,start="100",length="20")))
            before=journal.read_bytes()
            try:
                result=worker.assign(recover_only=True)
                outcome="resumed" if result and result["ticket"]==ticket and result["ticket_number"]=="35" else "unexpected"
            except CoreRpcUnavailable:
                outcome="stopped_on_unavailable"
            except ValueError as error:
                assert str(error)=="Saved ticket no longer matches its live computation lease"
                outcome="expired"
            finally:
                worker.rpc.cancel_all()
            return dict(median_time=median,outcome=outcome,calls=calls,phases=messages,journal_identical=journal.read_bytes()==before)
        finally:
            server.shutdown();relay.join()

cases=[probe(87399),probe(87400)]
passed=[case["outcome"] for case in cases]==["resumed","expired"] and all(
    item["calls"]==["getpgnstate","getpgnstate"] and item["phases"]==["Waiting for connection"] for item in cases)
report=dict(PASS=passed,loopback_http=True,no_mining=True,expired_journal_retained=cases[-1]["journal_identical"],cases=cases)
out.write_text(json.dumps(report,indent=2)+"\n",encoding="utf8")
assert report["expired_journal_retained"]
print(json.dumps(report,separators=(",",":")))
