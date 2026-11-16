"""Closed two-process Native display relay, real SIMPLE signatures, no blocks."""
from pathlib import Path
import argparse, ast, types, json, hashlib, time, secrets, base64, os, subprocess
p=argparse.ArgumentParser();p.add_argument('--out',required=True);args=p.parse_args()
root=Path(args.out).resolve();assert Path('C:/PGN/tests/social223').resolve() in root.parents;root.mkdir(exist_ok=False)
fixture=Path('C:/PGN/tests/social223/r246-functional/public-consent-ready-r1')
seed=json.loads((fixture/'RESULT.json').read_text())
core=Path('C:/PGN/tests/social223/r246-functional/offchain-relay-build/Core/pgn-core.exe')
assert hashlib.sha256(core.read_bytes()).hexdigest()=='a013b52b88cb2a42739c42224718296c0fd631e9800f036000c6d1d746073f10'
helper=Path('C:/PGN/tests/social223/functional-witness/social_moderation_native.py')
tree=ast.parse(helper.read_text(encoding='utf8'));defs=ast.Module(body=[x for x in tree.body if isinstance(x,(ast.Import,ast.ImportFrom,ast.FunctionDef,ast.ClassDef))],type_ignores=[])
n=types.ModuleType('closed_native_display');n.__dict__.update(core=core,root=root,nodes=[],checks=[],ports=set(),g='');exec(compile(defs,str(helper),'exec'),n.__dict__)
def existing(name):
 node=n.Node.__new__(n.Node);node.name=name;node.path=fixture/('named-'+name);node.rpcport=n.port();node.peerport=n.port();node.process=None;node.cookie=None;node.wallet='private-named-'+name;node.log=(root/(name+'-process.log')).open('ab');n.nodes.append(node);node.start(True);return node
def records(node,scripts):
 value=node.rpc('getsocialdisplaylinks',[scripts])
 return value.get('results',[]) if value.get('schema')=='PGN_SOCIAL_CURRENT_DISPLAY_1' else []
def state(node,script):
 rows=records(node,[script]);return rows[0] if rows else {}
def submit(node,raw):
 return n.wait(lambda:(lambda r:r if r.get('state')!='queued' else None)(node.rpc('submitsocialdisplaylink',[raw])),'Native async display admission')
def certificate(s,previous,action='bind',key=None):
 key=key or dict(script=seed['operator_script'],address=seed['operator_address'],node=A)
 st=dict(v=2,genesis=n.g,origin='pgn-social://'+n.g,scope='display_link_for_script_history',action=action,social_script=s['script'],operator_script=key['script'],operator_id=seed['native_operator']['operator_id'],anchor_hash=seed['anchor'],anchor_height=seed['height'],previous_link=previous,nonce=secrets.token_hex(32))
 eid=n.sha('PGN/SOCIAL/IDENTITY-LINK-ID/2\n'+n.canonical(st));payload=n.canonical(dict(event_id=eid,statement=st));c=dict(statement=st,social_proof='',operator_proof='',authorizer_script='',authorization_proof='')
 if action=='bind':
  c['social_proof']=s['node'].rpc('signmessage',[s['address'],'PGN/SOCIAL/IDENTITY-LINK/SOCIAL-CONSENT/2\n'+payload],True)
  c['operator_proof']=key['node'].rpc('signmessage',[key['address'],'PGN/SOCIAL/IDENTITY-LINK/OPERATOR-CONSENT/2\n'+payload],True)
 else:
  c['authorizer_script']=key['script'];c['authorization_proof']=key['node'].rpc('signmessage',[key['address'],'PGN/SOCIAL/IDENTITY-LINK/STATE-TRANSITION/2\n'+payload],True)
 return n.canonical(c),eid
result=dict(scope='PRIVATE two Native peers; actual typed P2P bind/withdraw persistence. No production activation or roles.',checks=n.checks,public_network=False)
try:
 A,B=existing('A'),existing('B');n.g=A.rpc('getblockhash',[0]);before=[node.rpc('getbestblockhash')for node in[A,B]];n.connect(A,B)
 n.wait(lambda:all(any(p.get('display_links_v2')for p in node.rpc('getsocialpeers').get('relay_peers',[]))for node in[A,B]),'Both actual PGN peers negotiate display capability2')
 social=A.identity();social['node']=A
 raw,cid=certificate(social,'0'*64)
 n.check(submit(A,raw)['state']=='accepted_local','A admits real double-domain SIMPLE proof')
 row=n.wait(lambda:(lambda r:r if r.get('state')=='linked' else None)(state(B,social['script'])),'B automatically receives binding over PGN P2P')
 n.check(row['certificate_id']==cid and row['operator_id']==seed['native_operator']['operator_id'] and row['operator_script']==seed['operator_script'],'P2P proof preserves exact S/K/O without display-name matching')
 n.check(row['display_name']=='Social QA operator A','Name originates in authenticated Native owner report')
 forged_key=B.identity();forged_key['node']=B;forged,_=certificate(social,'0'*64,key=forged_key)
 n.check(submit(A,forged)['state']=='rejected','Fully signed unknown K cannot borrow an existing O')
 n.check(state(B,social['script'])['certificate_id']==cid,'Other-peer verified name unchanged by refused claimant')
 tampered=json.loads(raw);signature=tampered['operator_proof'];tampered['operator_proof']=signature[:20]+('A' if signature[20]!='A' else 'B')+signature[21:]
 assert tampered['operator_proof']!=signature
 n.check(submit(A,n.canonical(tampered))['state']=='rejected','Altered BIP322 proof rejected by Native')
 withdrawn,wid=certificate(social,cid,'withdraw')
 n.check(submit(B,withdrawn)['state']=='accepted_local','B admits K-only withdrawal, no old S consent required')
 n.wait(lambda:all(state(node,social['script']).get('state')=='withdrawn'for node in[A,B]),'Withdrawal automatically propagates in reverse direction')
 n.check(submit(A,raw)['state']=='accepted_local' and state(A,social['script'])['state']=='withdrawn','Exact binding retry is idempotent and does not resurrect withdrawn name')
 for node in[A,B]:node.stop();node.start(True)
 n.connect(A,B)
 n.wait(lambda:all(state(node,social['script']).get('state')=='withdrawn'for node in[A,B]),'Both Native processes retain K withdrawal after restart')
 other=A.identity();other['node']=A;base,bid=certificate(other,'0'*64);one,_=certificate(other,bid);two,_=certificate(other,bid)
 n.check(submit(A,base)['state']=='accepted_local','Independent S proof admitted')
 n.wait(lambda:state(B,other['script']).get('state')=='linked','Independent parent delivered before concurrent children')
 n.check(submit(A,one)['state']=='accepted_local' and submit(B,two)['state']=='accepted_local','Different peers admit two consenting children')
 n.wait(lambda:all(state(node,other['script']).get('state')=='conflict'for node in[A,B]),'Fork converges to explicit conflict, not arbitrary hash/time winner')
 for node in[A,B]:
  rows=n.wait(lambda:records(node,[social['script'],'invalid-script',other['script']]),'Bounded mixed-validity batch')
  n.check([r['state']for r in rows]==['withdrawn','unavailable','conflict'],'Invalid entry neither authorizes nor suppresses other identities')
 n.check([node.rpc('getbestblockhash')for node in[A,B]]==before,'Binding, withdrawal, replay and restart create ZERO Native blocks')
 result.update(status='PASS',core_sha256=hashlib.sha256(core.read_bytes()).hexdigest(),certificate_id=cid,certificate_bytes=len(raw),withdrawal_id=wid,anchor=before[0],limits=['Private per-S16 retained proof bound; no50k/Sybil qualification','Partitions cannot guarantee instant withdrawal visibility','Current display relation only; no Moderator/Native historical ownership or human identity claim'])
except Exception as error:
 import traceback;result.update(status='FAIL',error_type=type(error).__name__,error=str(error)[:800],traceback=traceback.format_exc()[-4500:]);raise
finally:
 for node in reversed(n.nodes):
  try:node.stop();node.log.close()
  except Exception as error:result.setdefault('stop_errors',[]).append(str(error))
 result['owned_processes_stopped']=all(x.process and x.process.poll()is not None for x in n.nodes)
 (root/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf8');print(json.dumps(dict(status=result.get('status'),checks=len(n.checks),owned_processes_stopped=result['owned_processes_stopped'])),flush=True)
