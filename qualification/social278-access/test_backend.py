from pathlib import Path
import json,time,unittest,uuid
from authority import Store,Verifier,Denied,Conflict,DOMAIN,canonical,digest,PERMISSIONS
from access_backend import Backend

class BackendTests(unittest.TestCase):
 def setUp(self):
  self.genesis='7'*64;self.keys=['0014'+c*40 for c in '1234'];self.accounts=[c*64 for c in 'abc'];self.registry=dict(zip(self.keys[:3],self.accounts));self.registry[self.keys[3]]=self.accounts[0]
  root=Path(__file__).resolve().parent/'test-runs';root.mkdir(exist_ok=True);self.path=root/('accounts-'+uuid.uuid4().hex+'.sqlite3')
  self.store=Store(self.path,self.genesis,self.accounts[:2],create=True,account_registry=self.registry.get);self.verifier=Verifier(self.genesis,lambda *args:True);self.backend=Backend(self.store,self.verifier)
 def tearDown(self):self.store.close()
 def request(self,key,action,args,revision=None):
  return DOMAIN+canonical(dict(v=1,genesis=self.genesis,community=self.store.community,request_id=str(uuid.uuid4()),actor=key,address='fixture-proof-only',created_ms=int(time.time()*1000),revision=self.store.revision if revision is None else revision,action=action,args=args))
 def read(self,key,member='',role=''):return self.backend.handle(self.request(key,'read_access',dict(channel='global',member=member,role=role)),'fixture-proof')
 def apply(self,key,action,args):return self.backend.handle(self.request(key,action,args),'fixture-proof')
 def test_account_alias_rotation_keeps_role_and_community(self):
  self.apply(self.keys[0],'create_role',dict(role='helper',name='Helper',position=10,permissions=['react']))
  oldcommunity=self.store.community;self.registry.pop(self.keys[0]);self.apply(self.keys[3],'edit_role',dict(role='helper',name='Support',position=11,permissions=['react']))
  with self.assertRaises(Denied):self.read(self.keys[0])
  self.assertEqual(self.store.community,oldcommunity);self.assertEqual(self.store.permissions(self.accounts[0])[1],999)
  self.store.close();self.store=Store(self.path,self.genesis,self.accounts[:2],account_registry=self.registry.get);self.assertEqual(self.store.community,oldcommunity)
 def test_guest_native_proof_does_not_gain_moderator_from_label(self):
  self.backend.labels[self.accounts[2]]='Wizz';s=self.read(self.keys[2]);self.assertEqual(s['identity'],self.keys[2]);self.assertNotIn('manage_roles',s['grantable']);self.assertFalse(next(a for a in s['availability']if a['action']=='create_role')['allowed'])
  with self.assertRaises(Denied):self.apply(self.keys[2],'create_role',dict(role='admin',name='Admin',position=1,permissions=['manage_roles']))
 def test_focused_availability_stays_bounded_at_full_ui_capacity(self):
  for i in range(98):self.store.db.execute('INSERT INTO roles VALUES(?,?,?,?,0)',('r'+str(i),'Role '+str(i),i+1,'["react"]'))
  for i in range(198):self.store.db.execute('INSERT INTO members(id,roles)VALUES(?,?)',(format(i,'064x'),'[]'))
  s=self.read(self.keys[0],self.accounts[2]);self.assertEqual(len(s['roles']),100);self.assertEqual(len(s['members']),200);self.assertLessEqual(len(s['availability']),1000)
  self.assertTrue(all(not a['target']or a['target']==self.accounts[2]for a in s['availability']if a['action']=='assign_role'))
 def test_snapshot_tracks_current_grant_and_denies_stale_mutation(self):
  self.apply(self.keys[0],'create_role',dict(role='helper',name='Helper',position=10,permissions=['react']))
  old=self.request(self.keys[1],'delete_role',dict(role='helper'));before=self.read(self.keys[1]);self.apply(self.keys[0],'assign_role',dict(target=self.accounts[2],role='helper'))
  with self.assertRaises(Conflict):self.backend.handle(old,'fixture-proof')
  after=self.read(self.keys[1]);self.assertNotEqual(before['revision'],after['revision']);self.assertIn('helper',next(m for m in after['members']if m['id']==self.accounts[2])['roles'])
 def test_expired_cross_context_and_malformed_read_fail_closed(self):
  message=self.request(self.keys[0],'read_access',dict(channel='global',member='',role=''));b=json.loads(message[len(DOMAIN):]);b['created_ms']-=180000
  with self.assertRaises(Denied):self.backend.handle(DOMAIN+canonical(b),'fixture-proof')
  with self.assertRaises(Denied):self.read(self.keys[0],'not-an-account')
  with self.assertRaises(Denied):self.backend.handle(message.replace(self.genesis,'8'*64),'fixture-proof')
 def test_canonical_membership_targets_are_not_key_aliases(self):
  self.apply(self.keys[0],'create_role',dict(role='helper',name='Helper',position=10,permissions=['react']))
  with self.assertRaises(Denied):self.apply(self.keys[0],'assign_role',dict(target=self.keys[2],role='helper'))
  self.apply(self.keys[0],'assign_role',dict(target=self.accounts[2],role='helper'));self.assertIn('helper',[r['id']for r in self.store.member(self.accounts[2])[0]])

if __name__=='__main__':unittest.main()
