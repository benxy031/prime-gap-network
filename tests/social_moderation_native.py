"""Private 2-peer Native admission/ACL witness; synthetic REGTEST wallets only.
No UI or public governance claim. No user identity/credential is an input.
"""
from pathlib import Path
import argparse,base64,hashlib,json,secrets,socket,subprocess,time,urllib.request,urllib.error
p=argparse.ArgumentParser();p.add_argument('--core',required=True);p.add_argument('--out',required=True);args=p.parse_args()
core=Path(args.core).resolve();root=Path(args.out).resolve();root.mkdir(parents=True,exist_ok=False)
nodes=[];checks=[];ports=set();g='';base_time=int(time.time()*1000)
def check(value,label,**facts):
    if not value:raise AssertionError(label)
    checks.append(dict(name=label,status='PASS',**facts));print('PASS '+label,flush=True)
def wait(fn,label,seconds=35):
    deadline=time.monotonic()+seconds;last=None
    while time.monotonic()<deadline:
        last=fn()
        if last:return last
        time.sleep(.1)
    raise TimeoutError(label+': '+str(last))
def port():
    for _ in range(100):
        with socket.socket() as s:s.bind(('127.0.0.1',0));n=s.getsockname()[1]
        if n not in ports and not 48950<=n<=48959:ports.add(n);return n
    raise RuntimeError('Disposable port allocation failed')
def canonical(v):return json.dumps(v,ensure_ascii=True,separators=(',',':'),sort_keys=True)
def sha(s):return hashlib.sha256(s.encode()).hexdigest()
class Node:
    def __init__(self,name):
        self.name=name;self.path=root/name;self.path.mkdir();self.rpcport=port();self.peerport=port();self.process=None;self.cookie=None;self.wallet='private-'+name;self.log=(self.path/'process.log').open('ab');nodes.append(self);self.start();self.rpc('createwallet',[self.wallet]);self.address=self.rpc('getnewaddress',['','bech32'],True);self.script=self.rpc('getaddressinfo',[self.address],True)['scriptPubKey']
    def start(self,reload=False):
        cmd=[str(core),'-regtest','-datadir='+str(self.path),'-server=1','-listen=1','-bind=127.0.0.1','-port='+str(self.peerport),'-connect=0','-dnsseed=0','-fixedseeds=0','-discover=0','-listenonion=0','-rpcbind=127.0.0.1','-rpcallowip=127.0.0.1','-rpcport='+str(self.rpcport),'-par=1','-dbcache=32','-maxconnections=8','-keypool=2','-printtoconsole=0']
        if reload:cmd+=['-wallet='+self.wallet]
        self.process=subprocess.Popen(cmd,cwd=self.path,stdout=self.log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        def ready():
            if self.process.poll() is not None:
                self.log.flush();tail=(self.path/'process.log').read_bytes()[-3000:].decode('utf-8','replace');raise RuntimeError('Owned private Core exited '+self.name+' code='+str(self.process.returncode)+'; startup_tail='+tail)
            files=list(self.path.glob('*/.cookie'))
            if files:
                self.cookie=files[0]
                try:return self.rpc('getblockchaininfo')
                except (OSError,RuntimeError,ValueError):pass
            return False
        wait(ready,'Private Core startup '+self.name)
    def rpc(self,method,params=None,wallet=False):
        token=base64.b64encode(self.cookie.read_bytes().strip()).decode()
        req=urllib.request.Request('http://127.0.0.1:'+str(self.rpcport)+('/wallet/'+self.wallet if wallet else ''),json.dumps(dict(jsonrpc='2.0',id='private-social',method=method,params=params or [])).encode(),{'Authorization':'Basic '+token,'Content-Type':'application/json'})
        try:
            with urllib.request.urlopen(req,timeout=5) as response:value=json.load(response)
        except urllib.error.HTTPError as error:value=json.load(error)
        if value.get('error'):raise RuntimeError(method+': '+str(value['error']))
        return value['result']
    def stop(self):
        if self.process and self.process.poll() is None:
            try:self.rpc('stop')
            except (OSError,RuntimeError):pass
            try:self.process.wait(timeout=25)
            except subprocess.TimeoutExpired:self.process.terminate();self.process.wait(timeout=10)
    def identity(self):
        address=self.rpc('getnewaddress',['','bech32'],True);return dict(node=self,address=address,script=self.rpc('getaddressinfo',[address],True)['scriptPubKey'])
def envelope(identity,domain,payload):
    v=dict(v=1,genesis=g,origin='pgn-social://'+g,sender_script=identity['script'],signing_address=identity['address'],device='d'*32,created_ms=int(time.time()*1000),nonce=secrets.token_hex(32));v.update(payload)
    if domain!='RECEIPT':v['event_id']=sha('PGN/SOCIAL/'+{'EVENT':'EVENT-ID','MODERATION':'MODERATION-ID','POLICY-ATTEST':'POLICY-ATTEST-ID'}[domain]+'/1\n'+canonical(v))
    message='PGN/SOCIAL/'+domain+'/1\n'+canonical(v)
    signature=identity['node'].rpc('signmessage',[identity['address'],message],True)
    return dict(canonical_message=message,signature=signature,event_id=v['event_id'])
def submit(node,proof):
    def terminal():
        status=node.rpc('submitsocialevent',[proof['canonical_message'],proof['signature']]);return status if status['state'] in ['accepted_local','rejected','uncertain'] else False
    return wait(terminal,'Native admission')
def state(n):return n.rpc('getsocialaccess',[0,1])
def events(n):return n.rpc('getsocialevents',['',0,50])['records']
def visible(n,event):
    return next((r for r in events(n) if json.loads(r['canonical_message'].split('\n',1)[1])['event_id']==event),None)
def connect(a,b):
    a.rpc('addnode',['127.0.0.1:'+str(b.peerport),'add']);wait(lambda:a.rpc('getsocialpeers').get('relay_peers'),'PGN relay negotiation')
def policy_doc(epoch,parent,mods,masks):return dict(v=1,genesis=g,origin='pgn-social://'+g,scope='social',epoch=epoch,previous_policy=parent,moderators=sorted(mods),threshold=2,ratified_masks=sorted(masks,key=lambda x:x['action_id']))
result={'scope':'Private Core20 Social overlay, two actual native peers, synthetic REGTEST only','core_sha256':hashlib.sha256(core.read_bytes()).hexdigest(),'checks':checks,'public_network':False,'qt_witness':False,'production_authority':False}
try:
    a,b=Node('A'),Node('B');g=a.rpc('getblockhash',[0]);check(g==b.rpc('getblockhash',[0]),'Exact isolated regtest genesis')
    A=dict(node=a,address=a.address,script=a.script);B=dict(node=b,address=b.address,script=b.script);C=b.identity();D=b.identity()
    root_doc=policy_doc(0,'0'*64,[B['script'],C['script'],D['script']],[]);root_id=sha('PGN/SOCIAL/POLICY-ID/1\n'+canonical(root_doc))
    for n in nodes:
        n.stop();directory=n.path/'regtest/social';directory.mkdir(exist_ok=True);(directory/'private-moderation-bootstrap.json').write_text(json.dumps(dict(synthetic_only='PGN_PRIVATE_REGTEST_MODERATION_1',policy=root_doc)),encoding='utf-8');n.start(True)
    wait(lambda:state(a).get('available') and state(b).get('available'),'Explicit synthetic trust anchors loaded')
    check(state(a)['root_id']==root_id==state(b)['root_id'],'Same explicit private authority anchor; no name binding')
    connect(a,b);check(all(p['addr'].startswith('127.0.0.1:') for n in nodes for p in n.rpc('getpeerinfo')),'All actual network peers are localhost')
    def chat(text):
        proof=envelope(A,'EVENT',dict(channel='global',kind='chat',parents=[],bbcode_b64=base64.b64encode(text.encode()).decode()));check(submit(a,proof)['state']=='accepted_local','Native signed chat admitted');wait(lambda:visible(b,proof['event_id']),'Actual P2P message received');return proof
    first=chat('[b]Synthetic moderation target[/b]');late=chat('Synthetic unratified target')
    forged=envelope(A,'MODERATION',dict(channel='global',policy_id=root_id,target_event=first['event_id'],action='hide'))
    check(submit(a,forged)['state']=='rejected','Authenticated nonmoderator cannot hide')
    grant=envelope(A,'MODERATION',dict(channel='global',policy_id=root_id,target_event=first['event_id'],action='grant'))
    check(submit(a,grant)['state']=='rejected','Nonmoderator cannot grant itself a role')
    homonym=envelope(A,'MODERATION',dict(channel='global',policy_id=root_id,target_event=first['event_id'],action='hide',displayName='B'))
    check(submit(a,homonym)['state']=='rejected','Homonymous display name cannot obtain authority')
    hidden=envelope(B,'MODERATION',dict(channel='global',policy_id=root_id,target_event=first['event_id'],action='hide'))
    check(submit(b,hidden)['state']=='accepted_local','Owned moderator B hide accepted by actual native ACL')
    wait(lambda:visible(a,first['event_id']).get('moderation_hidden'),'Signed hide propagated over PGN connection')
    check(visible(b,first['event_id'])['moderation_hidden'],'Both actual peers apply current signed hide')
    unratified=envelope(B,'MODERATION',dict(channel='global',policy_id=root_id,target_event=late['event_id'],action='hide'))
    check(submit(b,unratified)['state']=='accepted_local','Second authorized unratified hide accepted')
    wait(lambda:visible(a,late['event_id']).get('moderation_hidden'),'Second hide propagated')
    before=state(b)['next_cursor'];check(submit(b,hidden)['state']=='accepted_local' and state(b)['next_cursor']==before,'Identical current action retry does not append')
    masks=[dict(action_id=hidden['event_id'],channel='global',target_event=first['event_id'])]
    successor=policy_doc(1,root_id,[C['script'],D['script']],masks);next_id=sha('PGN/SOCIAL/POLICY-ID/1\n'+canonical(successor))
    vc=envelope(C,'POLICY-ATTEST',dict(policy=successor));vd=envelope(D,'POLICY-ATTEST',dict(policy=successor))
    check(submit(b,vc)['state']=='accepted_local' and state(b)['policy_id']==root_id,'One private fixture vote cannot revoke moderator')
    check(submit(b,vd)['state']=='accepted_local','Second distinct signed fixture vote durably admitted')
    wait(lambda:state(a).get('policy_id')==next_id,'Signed authority transition propagated')
    check(visible(a,first['event_id'])['moderation_hidden'] and visible(b,first['event_id'])['moderation_hidden'],'Ratified historical hide remains effective after revocation')
    check(not visible(a,late['event_id'])['moderation_hidden'] and not visible(b,late['event_id'])['moderation_hidden'],'Old unratified hide superseded regardless declared timestamp')
    check(submit(a,unratified)['state']=='rejected','Revoked cached old action retry cannot bypass current ACL')
    revoked=envelope(B,'MODERATION',dict(channel='global',policy_id=next_id,target_event=late['event_id'],action='hide'))
    check(submit(b,revoked)['state']=='rejected','Revoked signer denied at current epoch')
    # Actual CDB close/load: neither local confirmed bool nor display name is read.
    for n in nodes:n.stop();n.start(True)
    wait(lambda:state(a).get('policy_id')==next_id and state(b).get('policy_id')==next_id,'Signed certificates reconstructed from native CDB')
    check(visible(a,first['event_id'])['moderation_hidden'] and not visible(b,late['event_id'])['moderation_hidden'],'Restart preserves ratified state and revocation')
    check(submit(a,revoked)['state']=='rejected','Revocation still enforced after actual restart')
    connect(a,b);after=chat('Chat continues after role revocation and restart');check(not visible(b,after['event_id'])['moderation_hidden'],'Ordinary chat remains usable independently of privileges')
    result['status']='PASS';result['limits']=['Private synthetic single authority lineage; 2/3 is not public governance/BFT ordering','Certified fork negative witness belongs to component test; no public fork-resolution rule','Hide applies in conforming clients and retains original proof; hostile/legacy copies remain','No Qt context-menu claim in this native witness','Bounded private journal4MiB/4096; public signed compaction/recovery not activated']
except Exception as error:
    result['status']='FAIL';result['error']=str(error);raise
finally:
    for n in reversed(nodes):
        try:n.stop();n.log.close()
        except Exception as error:result.setdefault('stop_errors',[]).append(str(error))
    result['owned_processes_stopped']=all(n.process.poll() is not None for n in nodes)
    (root/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(status=result.get('status'),checks=len(checks),owned_processes_stopped=result['owned_processes_stopped'])),flush=True)
