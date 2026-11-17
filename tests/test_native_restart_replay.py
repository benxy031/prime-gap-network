"""A real released-layout Core can finish retained validation after two shutdowns."""
import argparse
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path, PurePosixPath
import socket
import subprocess
import time
import zipfile

args=argparse.ArgumentParser()
args.add_argument('--core',required=True,type=Path)
args.add_argument('--fixture',required=True,type=Path)
args.add_argument('--work',required=True,type=Path)
args.add_argument('--receipt',required=True,type=Path)
p=args.parse_args()
core=p.core.resolve(strict=True)
root=p.work.resolve()
assert not root.exists()
assert hashlib.sha256(p.fixture.read_bytes()).hexdigest()=='33f5d5a206786715725ffbed770bfeba3b014898bf6c0e734419fbbb62a050c6'
root.mkdir(parents=True)
with zipfile.ZipFile(p.fixture)as z:
    assert sum(info.file_size for info in z.infolist())<128*1024*1024
    for i in z.infolist():
        name=PurePosixPath(i.filename)
        assert not name.is_absolute()and '..'not in name.parts and 'wallets'not in name.parts and ':'not in i.filename and chr(92)not in i.filename
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(z.read(i))
fixture=json.loads((root/'FIXTURE.json').read_bytes())
for file,digest in fixture['files'].items():
    assert hashlib.sha256((root/file).read_bytes()).hexdigest()==digest
assert fixture['public_data_only'] and fixture['no_wallets'] and fixture['legacy_height']==4245
def free():
    with socket.socket()as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]
def rpc(port,method,params=(),timeout=3):
    auth=base64.b64encode((root/'testnet/.cookie').read_bytes().strip()).decode()
    c=http.client.HTTPConnection('127.0.0.1',port,timeout=timeout)
    try:
        c.request('POST','/',json.dumps(dict(jsonrpc='2.0',id='native-replay',method=method,params=params)),
                  {'Authorization':'Basic '+auth,'Content-Type':'application/json'})
        answer=json.loads(c.getresponse().read(2*1024*1024))
        assert answer.get('id')=='native-replay'
        if answer.get('error'):raise ValueError(answer['error']['code'])
        return answer['result']
    finally:c.close()
def start(name):
    port=free()
    cmd=[str(core),'-testnet=1',f'-datadir={root}',f'-rpcport={port}',f'-port={free()}',
         '-server=1','-connect=0','-listen=0','-dnsseed=0','-fixedseeds=0','-discover=0','-par=1','-dbcache=64','-keypool=2']
    log=root/(name+'.log')
    with log.open('wb')as out:
        process=subprocess.Popen(cmd,stdout=out,stderr=subprocess.STDOUT,cwd=core.parent,
             creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    return process,port,log
def stop(node):
    proc,port,_=node
    if proc.poll() is not None:return
    try:
        rpc(port,'stop',timeout=10);proc.wait(timeout=25)
    finally:
        if proc.poll()is None:
            proc.terminate();proc.wait(timeout=10)
    assert proc.returncode==0
def at_height(node,height,limit):
    proc,port,log=node
    until=time.monotonic()+limit
    while time.monotonic()<until and proc.poll()is None:
        try:
            chain=rpc(port,'getblockchaininfo')
            if chain['blocks']>=height:return chain['blocks']
        except (OSError,ValueError,http.client.HTTPException):pass
        time.sleep(.08)
    with log.open('rb')as stream:
        stream.seek(max(0,log.stat().st_size-1800));tail=stream.read(1800).decode(errors='replace')
    raise RuntimeError(f'Expected {height}, exit {proc.poll()}; scoped Core tail '+tail)
marker=b'Completing interrupted PGN chainstate validation through full replay of retained blocks'
def marker_seen(log):
    assert log.stat().st_size<12*1024*1024
    with log.open('rb')as f:
        return any(marker in line for line in f)
wallet='QA_interrupted_replay'
address=None
partial=[]
for name in ('legacy-interrupted','first-replay-interrupted'):
    node=start(name)
    try:
        at_height(node,200,130)
        if address is None:
            assert rpc(node[1],'createwallet',[wallet,False,False,'',False,True],20)['name']==wallet
            address=rpc(node[1],'getnewaddress',['Retained QA address','bech32'])
        height=at_height(node,1160,100)
        assert 1160<=height<2400, height
        partial.append(height)
    finally:stop(node)
    assert marker_seen(node[2])==(name=='first-replay-interrupted')
node=start('finish')
try:
    h=at_height(node,5681,240)
    assert rpc(node[1],'getblockhash',[5681])==fixture['canonical_tip']
    assert rpc(node[1],'getblockhash',[0])==fixture['genesis']
    if wallet not in rpc(node[1],'listwallets'):rpc(node[1],'loadwallet',[wallet],25)
    assert rpc(node[1],'getaddressinfo',[address])['ismine'] is True
finally:stop(node)
assert marker_seen(node[2])
cold=start('cold')
try:
    assert at_height(cold,5681,120)==h
    assert rpc(cold[1],'getblockhash',[5681])==fixture['canonical_tip']
finally:stop(cold)
assert not marker_seen(cold[2])
receipt=dict(PASS=True,partial_heights=partial,height=h,wallet_unchanged=True,retained_tip_exact=True,
             automatic_native_replay=True,cold_restart_no_repeat=True,
             external_peers=False,disabled_validation=False)
p.receipt.write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
print(json.dumps(receipt))
