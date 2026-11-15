"""Owned synthetic name fixture using the existing public Native audit generator.
Counts are controlled protocol inputs, NOT scientific computation/Gold/payments.
No production wallet, key, RPC, peer, chain or authority is an input.
"""
from pathlib import Path
import argparse,ast,types,json,hashlib,threading,sys
p=argparse.ArgumentParser();p.add_argument('--core',required=True);p.add_argument('--protocol',required=True);p.add_argument('--out',required=True);args=p.parse_args()
root=Path(args.out).resolve();root.mkdir(parents=True,exist_ok=False)
protocol=Path(args.protocol).resolve();sys.path.insert(0,str(protocol/'tests'));import native_audit_batch_rpc as batch
helper=Path('tests/social_moderation_native.py');tree=ast.parse(helper.read_text(encoding='utf8'));defs=ast.Module(body=[x for x in tree.body if isinstance(x,(ast.Import,ast.ImportFrom,ast.FunctionDef,ast.ClassDef))],type_ignores=[])
n=types.ModuleType('owned_native_names');n.__dict__.update(core=Path(args.core).resolve(),root=root,nodes=[],checks=[],ports=set(),g='');exec(compile(defs,str(helper),'exec'),n.__dict__)
result=dict(scope='Existing Native protocol fixture only, synthetic named report; no scientific/count/gap/reward claim',core_sha256=hashlib.sha256(n.core.read_bytes()).hexdigest(),checks=n.checks,public_network=False)
class Rpc:
 def __init__(self,node):self.node=node
 def call(self,method,params=None,wallet=False):
  try:return self.node.rpc(method,params,wallet)
  except RuntimeError as error:
   prefix=method+': '
   if str(error).startswith(prefix):
    value=ast.literal_eval(str(error)[len(prefix):])
    if isinstance(value,dict)and type(value.get('code'))is int and isinstance(value.get('message'),str):raise batch.CoreRpcError(value['code'],value['message'])from error
   raise
try:
 A,B=n.Node('named-A'),n.Node('named-B');n.g=A.rpc('getblockhash',[0]);assert n.g==batch.GENESIS==B.rpc('getblockhash',[0]);n.connect(A,B)
 rpc=Rpc(A);roles=[A.rpc('getnewaddress',['synthetic-name-protocol','bech32'],True)for _ in range(3)];lock=threading.RLock()
 owner=batch.NetworkAudits(batch.worker(rpc,root/'protocol-owner',roles[0],lock))
 def probe():
  try:
   response=A.rpc('getsocialassociations',[[A.script]])
   assert response['results'][0].get('state')!='linked','No unconsented name'
  except RuntimeError as error:
   result.setdefault('typed_context_errors',[]).append(str(error)[:400])
  __import__('time').sleep(.15)
  assert A.process.poll()is None and B.process.poll()is None,'Context failure stopped native process'
  A.rpc('getblockcount')
 def publish(envelopes):
  block=owner.worker.ledger(envelopes)['block_hash'];probe();return block
 probe()
 u=A.rpc('getpgnticketrequest',[roles[0]]);signature=A.rpc('signmessage',[roles[0],u['challenge']],True);request=A.rpc('getpgnticketrequest',[roles[0],None,signature,u['anchor']]);publish([request['envelope']])
 envelopes=[]
 for address in roles[1:]:
  u=A.rpc('getpgnticketrequest',[address]);signature=A.rpc('signmessage',[address,u['challenge']],True);envelopes.append(A.rpc('getpgnticketrequest',[address,None,signature,u['anchor']])['envelope'])
 publish(envelopes);publish([]);publish([])
 work=A.rpc('getpgnscientificwork',[request['request_id'],'0'*64]);start=int(work['start']);end=start+int(work['length'])
 body=dict(schema='PGN_HORIZON_COMPUTED_REPORT_V1',ticket_id=work['ticket'],start=work['start'],end=str(end),positions=work['length'],policy_sha256=batch.fields(start,end)['threshold_policy']['sha256'],counts=['0']*100,best=[])
 sealed=batch.seal_report(dict(body=body,sha256=batch.digest(body),network_accepted=False),work,'f'*64,discoverer='Social QA operator A')
 u=A.rpc('getpgnreportcommit',[work['request_id'],sealed['report_root']]);signature=A.rpc('signmessage',[roles[0],u['challenge']],True);publish([A.rpc('getpgnreportcommit',[work['request_id'],sealed['report_root'],signature])['envelope']]);publish([])
 peers=[batch.NetworkAudits(batch.worker(rpc,root/('protocol-auditor-'+str(i)),address,lock))for i,address in enumerate(roles[1:])]
 for peer in peers:peer.worker.audit_publishers=tuple(peers);peer.worker.audit_group_journal=root/'synthetic-assignment-group.json'
 batch.reserve_assignments(peers[0]);batch.reserve_assignments(peers[1]);audits=[json.loads((peer.folder/'current.json').read_bytes())for peer in peers]
 n.check([a['slot']for a in audits]==[0,1],'Existing Native generator assigns two real synthetic audit actors')
 for i,(peer,audit) in enumerate(zip(peers,audits)):
  audit['answer']=batch.audit_commitment(audit['ticket'],audit['attempt'],[r['index']for r in audit['ranges']],['0']*len(audit['ranges']),str(i+1)*64);audit['commit_ready']=True;batch.save(peer.folder/'current.json',audit)
 batch.commit_ready(peers[0],audits[0],lambda intent:None);batch.commit_ready(peers[1],audits[1],lambda intent:None)
 report_path=owner.worker.root/'tickets'/work['ticket']/'network-report.json';report_path.parent.mkdir(parents=True,exist_ok=True);batch.save(report_path,sealed)
 errors=owner.apply_reveal_group([(peer,str(i),dict(audit,_ready_owner_disclosure=True))for i,(peer,audit)in enumerate(zip(peers,audits))]);assert errors==[],errors
 probe();state=peers[0].state(work['request_id']);n.check([j['state']for j in state['jobs']]==['passed','passed'],'Owner disclosure and both audit reveals accepted by actual Native ledger')
 k=A.rpc('getaddressinfo',[roles[0]],True)['scriptPubKey'];anchor=A.rpc('getbestblockhash');n.wait(lambda:B.rpc('getbestblockhash')==anchor,'Named Native history reaches independent peer')
 rows=[]
 for node in [A,B]:
  data=node.rpc('getpgnleaderboard',[0,100]);row=next(r for r in data['rows'] if k in r['signing_scripts'])
  n.check(row['display_name']=='Social QA operator A','Actual Native public label resolves from authenticated report on '+node.name)
  n.check(node.rpc('verifychain',[4,0])is True,'Actual Native chain verification on '+node.name);rows.append(row)
 n.check(all(node.process.poll()is None for node in[A,B]),'Native context errors never terminate either native process');result.update(status='PASS',genesis=n.g,anchor=anchor,height=A.rpc('getblockcount'),native_operator=rows[0],operator_address=roles[0],operator_script=k,social_script=B.script,social_address=B.address,actors_distinct=k!=B.script,protocol_fixture_only=True,controlled_counts=True,scientific_computation_claimed=False)
except Exception as error:
 import traceback;result.update(status='FAIL',error_type=type(error).__name__,error=str(error)[:700],traceback=traceback.format_exc()[-5000:]);raise
finally:
 for node in reversed(n.nodes):
  try:node.stop();node.log.close()
  except Exception as error:result.setdefault('stop_errors',[]).append(str(error))
 result['owned_processes_stopped']=all(node.process.poll()is not None for node in n.nodes);(root/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf8');print(json.dumps(dict(status=result.get('status'),checks=len(n.checks),owned_processes_stopped=result['owned_processes_stopped'])),flush=True)
