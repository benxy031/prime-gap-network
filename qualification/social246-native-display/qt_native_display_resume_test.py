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
 checkpoint=Path('C:/PGN/tests/social223/r246-functional/qt-native-display-relay-test-r5')
 cfg=json.loads((checkpoint/'qt-A/config.json').read_text(encoding='utf8'));A.address=cfg['address'];A.script=cfg['script'];a=dict(node=A,address=A.address,script=A.script)
 cfg=json.loads((checkpoint/'qt-B/config.json').read_text(encoding='utf8'));B.address=cfg['address'];B.script=cfg['script']
 packet=json.loads((checkpoint/'qt-A/public-consent.json').read_text(encoding='utf8'));raw=packet['certificate'];cid=n.sha('PGN/SOCIAL/IDENTITY-LINK-ID/2\n'+n.canonical(json.loads(raw)['statement']))
 operations=json.loads((checkpoint/'qt-A/outbox.json').read_text(encoding='utf8'))['operations'];events=[json.loads(op['message'].split('\n',1)[1])for op in operations.values()];last=max(events,key=lambda x:x['created_ms'])
 source_a=native_wire(A,last['event_id']);source_b=native_wire(B,last['event_id'])
 n.check(source_a is not None and source_b is not None and source_a['canonical_message']==source_b['canonical_message']and source_a['signature']==source_b['signature'],'Paginated Native source proves identical actual signed wire after earlier page50')
 current=n.wait(lambda:(lambda row:row if row.get('state')in['withdrawn','linked']else None)(state(A,A.script)),'Native current display projection completes before continuation')
 cid=current['certificate_id'];result['withdraw_parent_id']=cid
 if current.get('state')=='withdrawn':
  raw,cid=certificate(a,current['certificate_id']);n.check(submit(A,raw)['state']=='accepted_local','Explicit synthetic rebind prepares the isolated reader diagnostic')
 ca,cb=Client(A,''),Client(B,'')
 n.wait(lambda:all(named(c,A.script)for c in[ca,cb]),'Two fresh Qt processes restore the Native display link without local packet import')
 result['inherited_prefix']=dict(receipt=str(checkpoint/'RESULT.json'),passed_assertions=27,first_invalid_boundary='Test requested Native page100; actual contract max50; binaries unchanged')
 for c,label in[(ca,'A'),(cb,'B')]:
  for w,h in[(1366,990),(1920,1080)]:n.check(c.command('capture',width=w,height=h,path=str(root/('native-named-'+label+'-'+str(w)+'.png')))['saved'],'Resumed actual named window '+label+' '+str(w))
 cb.command('scroll_trace_start');cb.command('scroll_older');immediate=cb.command('snapshot');fence=cb.command('scroll_fence');anchor=fence['scroll_state']
 result['reader_immediate']={k:immediate[k]for k in['anchor','anchor_top','scroll','maximum']};result['reader_scroll_before']=anchor
 n.check(fence['stable']and anchor['maximum']>0 and anchor['scroll']<anchor['maximum'],'Reader history anchor observed after stable GUI idle turns')
 withdrawn,wid=certificate(a,cid,'withdraw');n.check(submit(A,withdrawn)['state']=='accepted_local','Native K alone consents to withdrawal')
 n.wait(lambda:all(state(x,A.script).get('state')=='withdrawn'for x in[A,B]),'Withdrawal reaches both actual peers')
 n.wait(lambda:all(not named(c,A.script)for c in[ca,cb]),'Withdrawn proof removes the displayed name in both real Qt views')
 fence=cb.command('scroll_fence');after=fence['scroll_state'];result['reader_scroll_after']=after;trace=cb.command('scroll_trace')['events'];(root/'SCROLL-TRACE.json').write_text(json.dumps(trace,indent=2)+'\n',encoding='utf8');n.check(fence['stable']and after['anchor']==anchor['anchor']and abs(after['anchor_top']-anchor['anchor_top'])<=2,'Non-signing reader keeps message anchor and pixel offset during withdrawal')
 n.check(submit(A,raw)['state']=='accepted_local','Native exact certificate replay remains idempotent')
 n.wait(lambda:all(not named(c,A.script)for c in[ca,cb]),'Old binding replay cannot restore a withdrawn name')
 for c in[ca,cb]:c.stop()
 for x in[A,B]:x.stop();x.start(True)
 n.connect(A,B)
 ca,cb=Client(A,''),Client(B,'')
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
 result['qt_exit_codes']=[c.process.poll()for c in clients];result['native_exit_codes']=[x.process.poll()for x in n.nodes]
 if result.get('status')=='PASS'and any(code!=0 for code in result['qt_exit_codes']+result['native_exit_codes']):result.update(status='FAIL',error='Owned process exit code was not zero')
 (root/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf8');print(json.dumps(dict(status=result.get('status'),checks=len(n.checks),owned_processes_stopped=result['owned_processes_stopped'])),flush=True)

if result.get('status')!='PASS':raise SystemExit(1)
