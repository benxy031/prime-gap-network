"""Two actual SocialView / Native peers. No manual certificate transfer to reader."""
from pathlib import Path
import argparse,ast,types,json,hashlib,time,secrets,base64,os,queue,threading,subprocess
p=argparse.ArgumentParser();p.add_argument('--out',required=True);args=p.parse_args()
root=Path(args.out).resolve();assert Path('C:/PGN/tests/social223').resolve()in root.parents;root.mkdir(exist_ok=False)
fixture=Path('C:/PGN/tests/social223/r246-functional/public-consent-ready-r1');seed=json.loads((fixture/'RESULT.json').read_text())
core=Path('C:/PGN/tests/social223/r246-functional/offchain-relay-build/Core/pgn-core.exe')
client_exe=Path('C:/PGN/tests/social223/r246-functional/relay-qt-build/pgn-social-relay-display-witness.exe')
initial_client_sha=hashlib.sha256(client_exe.read_bytes()).hexdigest()
assert hashlib.sha256(core.read_bytes()).hexdigest()=='a013b52b88cb2a42739c42224718296c0fd631e9800f036000c6d1d746073f10'
helper=Path('C:/PGN/tests/social223/functional-witness/social_moderation_native.py');tree=ast.parse(helper.read_text(encoding='utf8'));defs=ast.Module(body=[x for x in tree.body if isinstance(x,(ast.Import,ast.ImportFrom,ast.FunctionDef,ast.ClassDef))],type_ignores=[])
n=types.ModuleType('closed_qt_native_display');n.__dict__.update(core=core,root=root,nodes=[],checks=[],ports=set(),g='');exec(compile(defs,str(helper),'exec'),n.__dict__)
clients=[];tree=ast.parse(Path('C:/PGN/tests/social223/r246-functional/compat/run_compat.py').read_text(encoding='utf8'));exec(compile(ast.Module(body=[x for x in tree.body if isinstance(x,ast.ClassDef)and x.name=='Client'],type_ignores=[]),'Client','exec'),globals())
source=Path('C:/PGN/tests/social223/r246-functional/native_display_relay_test.py');tree=ast.parse(source.read_text(encoding='utf8'))
exec(compile(ast.Module(body=[x for x in tree.body if isinstance(x,ast.FunctionDef)and x.name in['existing','records','state','submit','certificate']],type_ignores=[]),str(source),'exec'),globals())
def named(c,script):return any(e['author_id']==script and'Social QA operator A'in e['author']for e in c.command('rendered')['messages'])
def native_wire(node,event):
 generation='';cursor=0
 for _ in range(128):
  page=node.rpc('getsocialevents',[generation,cursor,50])
  if generation and page['generation']!=generation:raise RuntimeError('Native source generation changed while finding signed event')
  generation=page['generation']
  for row in page['records']:
   raw=row['canonical_message']
   if raw.startswith('PGN/SOCIAL/EVENT/1\n')and json.loads(raw.split('\n',1)[1])['event_id']==event:return row
  if not page['more']:return None
  following=page['next_cursor'];assert following>cursor;cursor=following
 raise RuntimeError('Bounded Native cursor search exhausted')
def phase(c,value):return c.command('name_state').get('phase')==value
result=dict(scope='Actual current SocialView menu -> two explicit synthetic wallet consents -> Native typed P2P -> two named views; no block inclusion, roles or Online claims',checks=n.checks,public_network=False,manual_reader_certificate_exchange=False,client_sha256_before_start=initial_client_sha)
try:
 A,B=existing('A'),existing('B');n.g=A.rpc('getblockhash',[0]);before=[x.rpc('getbestblockhash')for x in[A,B]];n.connect(A,B)
 a=A.identity();A.address=a['address'];A.script=a['script'];a['node']=A
 B.address=seed['social_address'];B.script=seed['social_script'];ca,cb=Client(A,''),Client(B,'')
 for c in[ca,cb]:c.command('draft',text='Synthetic private relay readiness draft')
 n.wait(lambda:all(c.command('snapshot').get('can_send')for c in[ca,cb]),'Actual Qt composers ready with authenticated native wallets')
 ca.command('leaderboard');n.wait(lambda:ca.command('name_from_leaderboard',operator_id=seed['native_operator']['operator_id']).get('clicked'),'Real Native leaderboard context action opens consent dialog')
 n.check(ca.command('name_click',action='prepare')['clicked'],'User prepares the actual same-wallet consent dialog')
 n.wait(lambda:phase(ca,2),'Native consent required')
 n.check('Social QA operator A'in ca.command('name_state')['summary'],'Dialog name comes from authenticated Native data')
 ca.command('name_agree',agreed=True);n.check(ca.command('name_click',action='sign-native')['clicked'],'User explicitly consents with Native K')
 n.wait(lambda:phase(ca,3),'Native consent verified')
 n.check(ca.command('name_click',action='sign-social')['clicked'],'User explicitly consents with Social S in same wallet')
 n.wait(lambda:phase(ca,4),'Both separate signature domains verified')
 raw=ca.command('name_state')['packet'];cid=n.sha('PGN/SOCIAL/IDENTITY-LINK-ID/2\n'+n.canonical(json.loads(raw)['statement']))
 n.check(ca.command('name_click',action='submit')['clicked'],'Actual dialog submits typed Native display link')
 n.wait(lambda:phase(ca,11),'Native admission plus current-display verification')
 n.wait(lambda:state(B,A.script).get('state')=='linked','Reader Native receives proof automatically over PGN')
 ca.command('name_close');ca.command('chat')
 text='Synthetic closed-network profile relay '+root.name;ca.command('send',text=text,format='b')
 message=n.wait(lambda:next((e for e in cb.messages()if e['body']=='[b]'+text+'[/b]'),None),'Actual formatted Chat send/read after public consent')
 for c in[ca,cb]:
  n.wait(lambda:named(c,A.script),'Actual SocialView author automatically named from Native relay')
  n.check(any(m['id']==A.script and'Social QA operator A'in m['name']for m in c.command('rendered')['members']),'Rendered member list uses the same signed S/K display relation')
  n.check(not any('online'in m['presence'].lower()for m in c.command('snapshot')['participants']),'No Online inferred from profile or peer activity')
 n.check(ca.command('name_state')['signatures']==2,'Accessible S and K need two explicit wallet actions, no import/export')
 cb.command('name_open',native_script=seed['operator_script']);n.check(cb.command('name_state')['signatures']==0,'Reader never signs or imports another identity certificate');cb.command('name_close')
 for c,label in[(ca,'A'),(cb,'B')]:
  for w,h in[(1366,990),(1920,1080)]:
   n.check(c.command('capture',width=w,height=h,path=str(root/('native-named-'+label+'-'+str(w)+'.png')))['saved'],'Real named window capture '+label+' '+str(w))
 # Populate ordinary signed messages only when necessary for a real history anchor.
 for i in range(12):
  line='Synthetic history anchor line '+str(i)+' '+root.name+'\nClosed-network reading position check.\nThird literal line for a measurable viewport.'
  ca.command('draft',text=line)
  n.wait(lambda:ca.command('snapshot').get('can_send'),'Composer ready for next closed review line')
  previous_ids={entry['id']for entry in ca.messages()}
  ca.command('send',text=line)
  sent=n.wait(lambda:next((e for e in ca.messages()if e['id']not in previous_ids and e['author_id']==A.script),None),'Native sender event ID allocated')
  received=n.wait(lambda:cb.message(sent['id']),'Same signed event ID reaches the reader')
  n.check(received['body']==sent['body'],'Exact wire body matches for native event '+sent['id'][:12])
 source_a=native_wire(A,sent['id']);source_b=native_wire(B,sent['id'])
 n.check(source_a['canonical_message']==source_b['canonical_message']and source_a['signature']==source_b['signature'],'Underlying Native signed wire is byte-identical across peers')
 cb.command('scroll_older');anchor=n.wait(lambda:(lambda s:s if s['maximum']>0 and s['scroll']<s['maximum'] and s['anchor'] else None)(cb.command('snapshot')),'Reader leaves latest after actual layout')
 withdrawn,wid=certificate(a,cid,'withdraw');n.check(submit(A,withdrawn)['state']=='accepted_local','Native K alone consents to withdrawal')
 n.wait(lambda:all(state(x,A.script).get('state')=='withdrawn'for x in[A,B]),'Withdrawal reaches both actual peers')
 n.wait(lambda:all(not named(c,A.script)for c in[ca,cb]),'Withdrawn proof removes the displayed name in both real Qt views')
 after=cb.command('snapshot');n.check(after['anchor']==anchor['anchor']and abs(after['anchor_top']-anchor['anchor_top'])<=2,'Non-signing reader keeps message anchor and pixel offset during withdrawal')
 n.check(submit(A,raw)['state']=='accepted_local','Native exact certificate replay remains idempotent')
 n.wait(lambda:all(not named(c,A.script)for c in[ca,cb]),'Old binding replay cannot restore a withdrawn name')
 for x in[A,B]:x.stop();x.start(True)
 n.connect(A,B)
 for c in[ca,cb]:c.command('reopen')
 n.wait(lambda:all(state(x,A.script).get('state')=='withdrawn'for x in[A,B]),'Persisted Native withdrawal reloads')
 n.wait(lambda:all(c.messages()and not named(c,A.script)for c in[ca,cb]),'Two restarted actual Qt readers use persisted Native state, no local name import')
 n.check(all(x.rpc('getblockhash',[seed['height']])==seed['anchor']for x in[A,B]),'Non-signing reader retains correct canonical Native anchor after restart')
 # A new consenting descendant may rebind; a fork must remove that name again.
 rebound,rid=certificate(a,wid);n.check(submit(A,rebound)['state']=='accepted_local','Explicit new double consent rebinds after withdrawal')
 n.wait(lambda:all(named(c,A.script)for c in[ca,cb]),'Current descendant becomes visible in both views')
 one,_=certificate(a,rid);two,_=certificate(a,rid);submit(A,one);submit(B,two)
 n.wait(lambda:all(state(x,A.script).get('state')=='conflict'for x in[A,B]),'Different received children converge to scoped conflict')
 n.wait(lambda:all(not named(c,A.script)for c in[ca,cb]),'Conflict removes name instead of leaving stale confirmed display')
 n.check(cb.command('capture',width=1366,height=990,path=str(root/'native-conflict-reader-1366.png'))['saved'],'Actual conflict fallback capture')
 n.check([x.rpc('getbestblockhash')for x in[A,B]]==before,'Complete consent, Chat, withdrawal, restart and conflict manufacture ZERO blocks')
 result.update(status='PASS',certificate_id=cid,certificate_bytes=len(raw.encode()),withdrawal_id=wid,core_sha256=hashlib.sha256(core.read_bytes()).hexdigest(),client_sha256=hashlib.sha256(client_exe.read_bytes()).hexdigest(),anchor=before[0],reader_scroll_before={k:anchor[k]for k in['anchor','anchor_top','scroll','maximum']},reader_scroll_after={k:after[k]for k in['anchor','anchor_top','scroll','maximum']},limits=['Private per-S16 retention; no50k/Sybil proof','Current-display only, not Native historic ownership or moderator authority','Restart opens default current chat position; in-session withdrawal preserves the exact historical reading anchor','Harness polls visible view500ms; production lifecycle/capability bridge needs Root review'])
except Exception as error:
 import traceback;result.update(status='FAIL',error_type=type(error).__name__,error=str(error)[:900],traceback=traceback.format_exc()[-4500:]);raise
finally:
 for c in reversed(clients):
  try:c.stop()
  except Exception as error:result.setdefault('stop_errors',[]).append(str(error))
 for x in reversed(n.nodes):
  try:x.stop();x.log.close()
  except Exception as error:result.setdefault('stop_errors',[]).append(str(error))
 result['owned_processes_stopped']=all(x.process and x.process.poll()is not None for x in clients+n.nodes)
 (root/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf8');print(json.dumps(dict(status=result.get('status'),checks=len(n.checks),owned_processes_stopped=result['owned_processes_stopped'])),flush=True)
