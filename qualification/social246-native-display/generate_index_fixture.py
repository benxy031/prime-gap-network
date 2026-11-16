"""Generate PUBLIC signature fixtures with one existing SYNTHETIC wallet.
No private key export, association admission, peer connection or new block.
"""
from pathlib import Path
import ast,types,json,hashlib,secrets,argparse
parser=argparse.ArgumentParser();parser.add_argument('--count',type=int,choices=[32,1000],default=1000);parser.add_argument('--out',required=True);args=parser.parse_args()
root=Path(args.out).resolve();assert Path('C:/PGN/tests/social223/r246-functional').resolve()in root.parents;root.mkdir(exist_ok=False)
fixture=Path('C:/PGN/tests/social223/r246-functional/public-consent-ready-r1');seed=json.loads((fixture/'RESULT.json').read_text())
core=Path('C:/PGN/tests/social223/r246-functional/public21-code/pgn-core.exe')
helper=Path('C:/PGN/tests/social223/functional-witness/social_moderation_native.py');tree=ast.parse(helper.read_text(encoding='utf8'));defs=ast.Module(body=[x for x in tree.body if isinstance(x,(ast.Import,ast.ImportFrom,ast.FunctionDef,ast.ClassDef))],type_ignores=[])
n=types.ModuleType('closed_index_signer');n.__dict__.update(core=core,root=root,nodes=[],checks=[],ports=set(),g='');exec(compile(defs,str(helper),'exec'),n.__dict__)
tree=ast.parse(Path('C:/PGN/tests/social223/r246-functional/native_display_relay_test.py').read_text(encoding='utf8'));exec(compile(ast.Module(body=[x for x in tree.body if isinstance(x,ast.FunctionDef)and x.name in['existing','certificate']],type_ignores=[]),'known_fixture','exec'),globals())
result=dict(synthetic_only=True,private_keys_exported=False,new_blocks=0,signing_core_sha256=hashlib.sha256(core.read_bytes()).hexdigest())
try:
 A=existing('A');n.g=A.rpc('getblockhash',[0]);tip=A.rpc('getbestblockhash');social=A.identity();social['node']=A
 events=[];previous='0'*64
 for i in range(args.count):
  raw,previous=certificate(social,previous,'bind'if i%2==0 else'withdraw');events.append(raw)
  if i%200==199:print('Public real-SIMPLE signature fixture '+str(i+1),flush=True)
 rebind,head=certificate(social,previous);fork_a,_=certificate(social,head);fork_b,_=certificate(social,head)
 migration=A.identity();migration['node']=A;parent,pid=certificate(migration,'0'*64);child,child_id=certificate(migration,pid);other,_=certificate(migration,pid);grandchild,_=certificate(migration,child_id);missing,_=certificate(migration,'f'*64)
 result.update(genesis=n.g,anchor=seed['anchor'],height=seed['height'],operator_script=seed['operator_script'],operator_id=seed['native_operator']['operator_id'],social_script=social['script'],events=events,fork=[rebind,fork_a,fork_b],migration_script=migration['script'],migration=[parent,child,other,grandchild],missing_parent=missing)
 assert A.rpc('getbestblockhash')==tip and not A.rpc('getpeerinfo')
 (root/'FIXTURE.json').write_text(json.dumps(result,separators=(',',':'))+'\n',encoding='utf8')
 print('Fixture contains real PUBLIC21 signatures; no admission/block/network',flush=True)
finally:
 for node in n.nodes:node.stop();node.log.close()
