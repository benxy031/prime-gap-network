"""Actual default-OFF binary: private file cannot bootstrap any moderator."""
from pathlib import Path
import argparse,ast,hashlib,json,types
p=argparse.ArgumentParser();p.add_argument('--core',required=True);p.add_argument('--out',required=True);a=p.parse_args()
root=Path(a.out).resolve();root.mkdir(parents=True,exist_ok=False);tree=ast.parse((Path(__file__).parent/'social_moderation_native.py').read_text(encoding='utf-8'));defs=ast.Module(body=[x for x in tree.body if isinstance(x,(ast.Import,ast.ImportFrom,ast.FunctionDef,ast.ClassDef))],type_ignores=[])
n=types.ModuleType('public_absence_helpers');n.__dict__.update(core=Path(a.core).resolve(),root=root,nodes=[],checks=[],ports=set(),g='');exec(compile(defs,'social_moderation_native.py','exec'),n.__dict__)
result=dict(scope='Actual production-default OFF capability absence; synthetic isolated testnet only',checks=n.checks,public_network=False)
try:
 n.check(b'PGN_PRIVATE_TESTNET_MODERATION_1'not in n.core.read_bytes(),'Synthetic bootstrap marker absent from actual default-OFF binary')
 A=n.Node('public-A');n.g=A.rpc('getblockhash',[0]);B=A.identity();C=A.identity();D=A.identity();doc=n.policy_doc(0,'0'*64,[B['script'],C['script'],D['script']],[])
 A.stop();folder=A.path/'testnet/social';folder.mkdir(exist_ok=True);(folder/'private-moderation-bootstrap.json').write_text(json.dumps(dict(synthetic_only='PGN_PRIVATE_TESTNET_MODERATION_1',policy=doc)),encoding='utf-8');A.start(True)
 state=n.wait(lambda:n.state(A),'Default-OFF authority API ready')
 n.check(state.get('private_qualification')is False and state.get('available')is False and state.get('root_id')=='','Actual default-OFF executable refuses synthetic bootstrap even when file exists')
 n.check(not A.rpc('getpeerinfo'),'Public-default capability check has zero network peers')
 proof=n.envelope(B,'MODERATION',dict(channel='global',policy_id=n.sha('PGN/SOCIAL/POLICY-ID/1\n'+n.canonical(doc)),target_event='e'*64,action='hide'))
 n.check(n.submit(A,proof)['state']=='rejected','An owned valid signer cannot activate moderator permission in default-OFF binary')
 result['status']='PASS'
except Exception as e:result['status']='FAIL';result['error']=str(e);raise
finally:
 for node in reversed(n.nodes):
  try:node.stop();node.log.close()
  except Exception as e:result.setdefault('stop_errors',[]).append(str(e))
 result['owned_processes_stopped']=all(x.process.poll()is not None for x in n.nodes);(root/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status=result.get('status'),checks=len(n.checks),owned_processes_stopped=result['owned_processes_stopped'])),flush=True)
