"""Real disconnected epoch3 bootstrap, first ticket, peer sync and cold recovery.

No scientific result is fabricated and no audit is marked Passed by this test.
All wallets and nodes are new private fixtures under an explicit scratch root.
"""
from pathlib import Path
from types import SimpleNamespace
import argparse,hashlib,json,os,socket,subprocess,sys,tempfile,threading,time,traceback
a=argparse.ArgumentParser();a.add_argument('--core',required=True);a.add_argument('--source',required=True);a.add_argument('--workspace',required=True);a.add_argument('--genesis',required=True);args=a.parse_args()
exe=Path(args.core).resolve(strict=True);source=Path(args.source).resolve(strict=True);workspace=Path(args.workspace).resolve();workspace.mkdir(parents=True,exist_ok=True)
root=Path(tempfile.mkdtemp(prefix='epoch3-native-',dir=workspace));(root/'.owned-test').write_text(args.genesis)
sys.path[:0]=[str(source/'src/engine/runtime_host'),str(source/'src/protocol')]
from horizon_core_rpc import CoreRpc
from horizon_network_worker import NetworkWorker
from horizon_mining_flow import MiningFlow
def port():
 with socket.socket()as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
class Node:
 def __init__(self,name,*,peer=None,reopen=None,wallet=None,wallets=False):
  self.root=reopen or root/name;self.root.mkdir(exist_ok=bool(reopen));self.data=self.root/'data';self.data.mkdir(exist_ok=bool(reopen));self.wallet=wallet
  self.rpcport=port();self.peerport=port();self.log=(self.root/'native.log').open('ab');self.process=None;self.rpc=None
  command=[str(exe),'-testnet=1','-server=1','-datadir='+str(self.data),'-listen=1','-bind=127.0.0.1','-port='+str(self.peerport),'-rpcbind=127.0.0.1','-rpcallowip=127.0.0.1','-rpcport='+str(self.rpcport),'-connect='+('127.0.0.1:'+str(peer)if peer else'0'),'-dnsseed=0','-fixedseeds=0','-discover=0','-listenonion=0','-natpmp=0','-par=1','-dbcache=32','-keypool=2']
  if wallet:command.append('-wallet='+wallet)
  elif not wallets:command.append('-disablewallet=1')
  flags=(subprocess.CREATE_NO_WINDOW|subprocess.BELOW_NORMAL_PRIORITY_CLASS)if os.name=='nt'else 0
  self.process=subprocess.Popen(command,cwd=exe.parent,stdin=subprocess.DEVNULL,stdout=self.log,stderr=subprocess.STDOUT,creationflags=flags)
  self.rpc=CoreRpc(self.rpcport,self.data/'testnet/.cookie',args.genesis,wallet)
  deadline=time.monotonic()+45
  try:
   while True:
    if self.process.poll()is not None:
     self.log.flush()
     with (self.root/'native.log').open('rb')as evidence:
      evidence.seek(max(0,os.fstat(evidence.fileno()).st_size-2500));tail=evidence.read().decode(errors='replace')
     raise RuntimeError('Owned private native node exited: '+tail[-2200:])
    try:self.rpc.verify();break
    except (OSError,RuntimeError):
     if time.monotonic()>=deadline:raise
     time.sleep(.1)
  except BaseException:
   try:self.close()
   except BaseException:pass
   raise
 def close(self):
  if self.process and self.process.poll()is None:
   try:self.rpc.call('stop');self.process.wait(timeout=20)
   except BaseException:self.process.kill();self.process.wait(timeout=5)
  if self.rpc:self.rpc.cancel_all()
  if self.process and self.process.returncode!=0:raise RuntimeError('Private Core did not close normally')
  self.log.close()
nodes=[];checks=[];failure=None;started=time.monotonic();out={}
try:
 # Create the initial native wallet through Core; no key/database transplant.
 first=Node('seed',wallets=True);nodes.append(first)
 assert first.rpc.call('getblockhash',[0])==args.genesis
 assert first.rpc.call('getblockcount')==0
 first.rpc.call('createwallet',['epoch3-qa',False,False,'',False,True,False])
 first.rpc.cancel_all();first.rpc=CoreRpc(first.rpcport,first.data/'testnet/.cookie',args.genesis,'epoch3-qa')
 address=first.rpc.call('getnewaddress',['Horizon miner','bech32'],wallet=True)
 worker=object.__new__(NetworkWorker);worker.root=root/'worker';worker.root.mkdir();worker.rpc=first.rpc;worker.sequence=0;worker.network_lock=threading.RLock();worker.state_lock=threading.RLock();worker.audits=None
 worker.args=SimpleNamespace(pilot_coordination=True,genesis=args.genesis,wallet='epoch3-qa',miner=address,reward=address)
 # Cross the real service activation so the first-ticket allowance, rather
 # than a preactivation bypass, is exercised on the new empty network.
 for _ in range(64):worker.ledger([])
 assert first.rpc.call('getblockcount')==64
 checks.append('fresh-native-service-activation-with-zero-old-audits')
 work=worker.assign();assert work['genesis']==args.genesis and int(work['length'])==200000000000000
 assert int(work['target_numerator'])*2==55*int(work['target_denominator'])
 job=MiningFlow.job(object.__new__(MiningFlow),work);assert job['target_merit_q16']==1802240
 metrics=first.rpc.call('getpgnnetworkmetrics',[2,3600,600]);assert sum(metrics['audit_queue'].get(k,0)for k in('awaiting','awaiting_seed','assigned','committed'))==0
 checks.append('first-real-signed-ticket-at-exact-27-point-5-after-service-activation')
 second=Node('peer',peer=first.peerport);nodes.append(second);tip=first.rpc.call('getbestblockhash');deadline=time.monotonic()+30
 while second.rpc.call('getbestblockhash')!=tip:
  if time.monotonic()>=deadline:raise RuntimeError('Fresh native peer did not converge')
  time.sleep(.1)
 assert second.rpc.call('verifychain',[4,0])is True
 checks.append('second-native-peer-synchronizes-and-validates-same-new-epoch')
 second.close();nodes.remove(second);oldroot=first.root;first.close();nodes.remove(first)
 first=Node('seed-cold',reopen=oldroot,wallet='epoch3-qa');nodes.append(first);worker.rpc=first.rpc
 assert first.rpc.call('getbestblockhash')==tip and first.rpc.call('getaddressinfo',[address],wallet=True)['ismine']
 restored=worker.assign(recover_only=True);assert restored['request_id']==work['request_id'] and restored['target_numerator']==work['target_numerator']and restored['target_denominator']==work['target_denominator']
 checks.append('cold-native-wallet-and-exact-ticket-recovery-without-reissue')
 out.update(genesis=args.genesis,height=first.rpc.call('getblockcount'),tip=tip,request_id=work['request_id'],target_numerator=work['target_numerator'],target_denominator=work['target_denominator'])
except BaseException as error:
 failure=type(error).__name__+': '+str(error)[:1000]
 (root/'failure-trace.txt').write_text(traceback.format_exc(limit=8))
finally:
 for node in reversed(nodes):
  try:node.close()
  except BaseException as error:failure=failure or type(error).__name__+': '+str(error)[:300]
 out.update(PASS=failure is None,error=failure,checks=checks,seconds=time.monotonic()-started,root=str(root),core_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),synthetic_scientific_reports=0,audits_falsely_marked_passed=0,scope='Actual fresh native bootstrap, service gate, signed ticket, peer sync and cold recovery. No GPU/scientific acceptance or capacity claim.',live_changed=False)
 (workspace/'FRESH-NATIVE-RESULT.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))
raise SystemExit(0 if out['PASS']else 1)
