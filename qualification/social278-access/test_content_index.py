from pathlib import Path
import hashlib,json,tempfile,threading,unittest,uuid,http.client,urllib.parse
from authority import Store,Verifier,Denied,DOMAIN,canonical
from access_backend import Backend
from access_http import AccessServer
from content_index import ContentIndex,projection
from community_keys import CommunityKeys

class ContentTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)/'community.sqlite3';self.genesis='7'*64;self.moderator='0014'+'1'*40;self.owner='0014'+'2'*40;self.other='0014'+'3'*40;self.account=lambda s:hashlib.sha256(bytes.fromhex(s)).hexdigest();self.mods=[self.account(self.moderator)];self.target='a'*64
  self.store=Store(self.path,self.genesis,self.mods,create=True,account_registry=self.account)
  self.event=dict(genesis=self.genesis,kind='chat',event_id=self.target,channel='global',sender_script=self.owner,created_ms=1)
  self.calls=[]
  def rpc(method,args):
   self.calls.append((method,args));self.assertEqual(method,'getsocialevents');return dict(genesis=self.genesis,generation='test-generation',records=[dict(canonical_message='PGN/SOCIAL/EVENT/1\n'+canonical(self.event))],next_cursor=1,more=False)
  self.index=ContentIndex(self.path,self.genesis,rpc);self.index.tick();self.store.content_lookup=self.index.lookup
  self.verifier=Verifier(self.genesis,lambda actor,address,message,signature:signature=='fixture-proof',require_client=True);self.backend=Backend(self.store,self.verifier)
 def tearDown(self):self.store.close();self.temp.cleanup()
 def command(self,actor,action,args,client=None):
  body=dict(v=1,genesis=self.genesis,community=self.store.community,actor=actor,client=client or actor,address='fixture-address',request_id=str(uuid.uuid4()),created_ms=self.store.clock(),revision=self.store.revision,action=action,args=args)
  return DOMAIN+canonical(body)
 def test_index_is_incremental_bounded_and_ignores_another_network(self):
  self.index.tick();self.assertEqual(self.calls[-1][1],['test-generation',1,50]);self.assertEqual(self.index.lookup(self.target)['owner'],self.owner)
  self.event=dict(self.event,event_id='b'*64,genesis='8'*64);self.index.tick();self.assertIsNone(self.index.lookup('b'*64))
 def test_unknown_or_forged_client_cannot_remove_someone_elses_message(self):
  self.backend.handle(self.command(self.moderator,'pin_content',dict(target=self.target,channel='global')),'fixture-proof')
  state=projection(self.path,self.genesis,self.store.community,dict(community=self.store.community,channel='global',targets=self.target));self.assertEqual(state['pinned'],[self.target])
  with self.assertRaises(Denied):self.backend.handle(self.command(self.other,'remove_content',dict(target=self.target,channel='global'),client=self.owner),'fixture-proof')
  self.backend.handle(self.command(self.owner,'remove_content',dict(target=self.target,channel='global')),'fixture-proof')
  state=projection(self.path,self.genesis,self.store.community,dict(community=self.store.community,channel='global',targets=self.target));self.assertEqual(state['removed'],[self.target]);self.assertEqual(state['pinned'],[])
 def test_focused_availability_uses_verified_content_and_current_roles(self):
  def read(actor):return self.backend.handle(self.command(actor,'read_access',dict(channel='global',member='',role='',targets=[self.target,'b'*64])),'fixture-proof')
  moderator=read(self.moderator);guest=read(self.other)
  self.assertTrue(next(r for r in moderator['availability']if r['action']=='pin_content')['allowed']);self.assertFalse(next(r for r in guest['availability']if r['action']=='pin_content')['allowed'])
  self.assertFalse(any(r['target']=='b'*64 for r in moderator['availability']))
 def test_projection_http_is_public_bounded_conditional_and_has_no_core_calls(self):
  server=AccessServer(('127.0.0.1',0),lambda:None,projection=lambda query:projection(self.path,self.genesis,self.store.community,query));worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start();before=len(self.calls)
  try:
   path='/v1/projection?'+urllib.parse.urlencode(dict(community=self.store.community,channel='global',targets=self.target))
   def get(headers={}):
    c=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=3);c.request('GET',path,headers=headers);r=c.getresponse();raw=r.read();status,tag=r.status,r.getheader('ETag');c.close();return status,tag,raw
   status,tag,raw=get();self.assertEqual(status,200);self.assertTrue(tag);self.assertEqual(json.loads(raw)['value']['targets'],[self.target]);self.assertEqual(get({'If-None-Match':tag})[0],304);self.assertEqual(len(self.calls),before)
  finally:server.shutdown();server.server_close();worker.join(timeout=2)
 def test_community_key_authority_does_not_query_a_loading_mining_index(self):
  calls=[]
  def rpc(method,args):
   calls.append(method)
   if method=='validateaddress':return dict(isvalid=True,scriptPubKey=self.moderator)
   if method=='verifymessage':return True
   raise AssertionError('Community authorization must not depend on the mining index')
  keys=CommunityKeys(rpc,self.genesis)
  with self.assertRaises(Denied):keys.resolve(self.moderator)
  self.assertTrue(keys.verify_and_bind(self.moderator,'fixture-address','scoped-message','fixture-proof'));self.assertEqual(keys.resolve(self.moderator),self.account(self.moderator));self.assertEqual(calls,['validateaddress','verifymessage'])
if __name__=='__main__':unittest.main()
