"""Private authorization/durability tests. Fake proofs here are not crypto QA."""
from pathlib import Path
import hashlib,json,threading,unittest,uuid
from authority import *
GENESIS='7'*64;WIZZ='0014'+'1'*40;ROBERT='0014'+'2'*40;USER='0014'+'3'*40;NOW=1794840000000
def signature(actor,message):return hashlib.sha256((actor+message).encode()).hexdigest()
class AccessTests(unittest.TestCase):
 def setUp(self):
  root=Path(__file__).resolve().parent/'test-runs';root.mkdir(exist_ok=True);self.path=root/(uuid.uuid4().hex+'.sqlite3')
  self.lookup={ 'a'*64:dict(genesis=GENESIS,channel='global',owner=USER)}
  self.store=Store(self.path,GENESIS,[WIZZ,ROBERT],create=True,clock=lambda:NOW,content_lookup=self.lookup.get)
  self.verifier=Verifier(GENESIS,lambda s,a,m,p:a=='address-'+s and p==signature(s,m),clock=lambda:NOW)
 def tearDown(self):self.store.close()
 def command(self,action,args,actor=WIZZ,revision=None,request_id=None,created=NOW):
  b=dict(v=1,genesis=GENESIS,community=self.store.community,request_id=request_id or str(uuid.uuid4()),actor=actor,address='address-'+actor,created_ms=created,revision=self.store.revision if revision is None else revision,action=action,args=args)
  message=DOMAIN+canonical(b);return self.verifier.verify(message,signature(actor,message))
 def role(self,**changes):return dict(role='helper',name='Helper',position=10,permissions=['manage_roles'],**changes)
 def test_role_lifecycle_and_persistence(self):
  self.store.apply(self.command('create_role',self.role()))
  self.store.apply(self.command('edit_role',dict(role='helper',name='Support',position=20,permissions=['react'])))
  self.store.apply(self.command('move_role',dict(role='helper',position=30)))
  self.store.apply(self.command('assign_role',dict(target=USER,role='helper')))
  self.assertIn('helper',[r['id']for r in self.store.member(USER)[0]])
  self.store.close();self.store=Store(self.path,GENESIS,[WIZZ,ROBERT],clock=lambda:NOW)
  self.assertEqual(self.store.role('helper')['position'],30)
  self.store.apply(self.command('delete_role',dict(role='helper')))
  self.assertEqual([r['id']for r in self.store.member(USER)[0]],['everyone'])
 def test_no_privilege_from_caller_or_display_name(self):
  before=self.store.revision
  with self.assertRaises(Denied):self.store.apply(self.command('create_role',self.role(),actor=USER))
  for role in ['everyone','moderator']:
   with self.assertRaises(Denied):self.store.apply(self.command('delete_role',dict(role=role)))
  with self.assertRaises(Denied):self.store.apply(self.command('ban_member',dict(target=ROBERT)))
  self.assertEqual(self.store.revision,before)
 def test_helper_cannot_escalate_or_edit_itself(self):
  self.store.apply(self.command('create_role',self.role()))
  self.store.apply(self.command('assign_role',dict(target=USER,role='helper')))
  with self.assertRaises(Denied):self.store.apply(self.command('edit_role',dict(role='helper',name='Top',position=998,permissions=list(PERMISSIONS)),actor=USER))
  with self.assertRaises(Denied):self.store.apply(self.command('create_role',dict(role='attack',name='Attack',position=5,permissions=['manage_members']),actor=USER))
 def test_exact_retry_and_conflict(self):
  command=self.command('create_role',self.role());first=self.store.apply(command);revision=self.store.revision
  self.assertEqual(self.store.apply(command),first);self.assertEqual(self.store.revision,revision)
  with self.assertRaises(Conflict):self.store.apply(self.command('create_role',dict(role='another',name='Another',position=20,permissions=[]),request_id=command.body['request_id']))
  with self.assertRaises(Conflict):self.store.apply(self.command('move_role',dict(role='helper',position=20),revision=0))
 def test_signed_body_cannot_change_after_verification(self):
  command=self.command('create_role',self.role(),actor=USER);command.body['actor']=WIZZ
  with self.assertRaises(Denied):self.store.apply(command)
 def test_reaction_retry_and_removed_target(self):
  command=self.command('react_content',dict(target='a'*64,channel='global',reaction='like',selected=True),actor=USER)
  self.store.apply(command);self.store.apply(command)
  self.assertEqual(self.store.db.execute('SELECT count(*)FROM reactions').fetchone()[0],1)
  self.store.apply(self.command('remove_content',dict(target='a'*64,channel='global'),actor=USER))
  self.assertEqual(self.store.db.execute('SELECT count(*)FROM reactions').fetchone()[0],0)
  with self.assertRaises(Denied):self.store.apply(self.command('pin_content',dict(target='a'*64,channel='global')))
 def test_storage_failure_does_not_publish_or_partial_grant(self):
  self.store.before_commit=lambda:(_ for _ in()).throw(Unavailable('injected disk full'))
  with self.assertRaises(Unavailable):self.store.apply(self.command('create_role',self.role()))
  self.assertEqual(self.store.revision,0);self.assertEqual(self.store.db.execute('SELECT count(*)FROM roles').fetchone()[0],2)
  self.assertEqual(self.store.db.execute('SELECT count(*)FROM receipts').fetchone()[0],0)
 def test_channel_deny_and_scoped_content_actions(self):
  self.store.apply(self.command('set_overwrite',dict(channel='global',target='everyone',member=False,allow=[],deny=['react'])))
  self.assertNotIn('react',self.store.permissions(USER,'global')[0]);self.assertIn('react',self.store.permissions(USER,'other')[0])
  with self.assertRaises(Denied):self.store.apply(self.command('pin_content',dict(target='a'*64,channel='global'),actor=USER))
  with self.assertRaises(Denied):self.store.apply(self.command('remove_content',dict(target='a'*64,channel='other'),actor=USER))
  self.store.apply(self.command('remove_content',dict(target='a'*64,channel='global'),actor=USER))
  self.assertEqual(self.store.db.execute('SELECT removed FROM content').fetchone()[0],1)
 def test_concurrent_same_revision_has_one_winner(self):
  barrier=threading.Barrier(2);results=[];commands=[self.command('create_role',dict(role='a',name='A',position=10,permissions=[])),self.command('create_role',dict(role='b',name='B',position=20,permissions=[]),actor=ROBERT)]
  def run(command):
   store=Store(self.path,GENESIS,[WIZZ,ROBERT],clock=lambda:NOW)
   try:
    barrier.wait();store.apply(command);results.append('accepted')
   except Conflict:results.append('conflict')
   finally:store.close()
  threads=[threading.Thread(target=run,args=(c,))for c in commands]
  for t in threads:t.start()
  for t in threads:t.join(timeout=8);self.assertFalse(t.is_alive())
  self.assertEqual(sorted(results),['accepted','conflict']);self.assertEqual(self.store.revision,1)
 def test_no_automatic_authority_reset(self):
  with self.assertRaises(Unavailable):Store(self.path,GENESIS,[USER],clock=lambda:NOW)
 def test_malformed_and_cross_context_proof_rejected(self):
  valid=self.command('create_role',self.role());body=dict(valid.body)
  for changed in [dict(body,actor='Wizz'),dict(body,v=True),dict(body,genesis='8'*64),dict(body,request_id='bad')]:
   message=DOMAIN+canonical(changed)
   with self.assertRaises(Denied):self.verifier.verify(message,signature(changed['actor'],message))
if __name__=='__main__':unittest.main(verbosity=2)
