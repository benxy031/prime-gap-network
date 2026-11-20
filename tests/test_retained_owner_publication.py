"""A damaged legacy receipt cannot prevent unrelated signed results publishing."""
from pathlib import Path
from types import SimpleNamespace
import copy, hashlib, json, sys, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'src/engine/runtime_host'),str(ROOT/'src/protocol'),str(ROOT/'tests')]
from horizon_network_audits import NetworkAudits, save
from report_commitment import audit_commitment
from audit_publication_chain_fixture import ChainRPC

class PublicationIsolationTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
  self.worker=SimpleNamespace(root=Path(self.temp.name),rpc=ChainRPC())
  self.audit=NetworkAudits(self.worker);self.names=[];self.states={};self.sent=[]
  for n in (1,2):
   request=f'{n:064x}';ticket=f'{n+10:064x}';name=request+'-0-0'
   ranges=[dict(index=i,start=str(100+20*i),end=str(120+20*i))for i in range(5)]
   answer=audit_commitment(ticket,0,list(range(5)),['7']*5,'b'*64)
   value=dict(request_id=request,ticket=ticket,report_root='d'*64,slot=0,attempt=0,ranges=ranges,counts=[7]*5,answer=answer)
   save(self.audit.folder/(name+'.json'),value);self.names.append(name)
   self.states[request]=dict(details_root='c'*64,jobs=[dict(slot=0,attempt=0,state='committed',commitment=answer['commitment'])])
  self.audit.answers.register(self.names)
  with self.audit.answers.database()as db:db.execute("INSERT INTO meta VALUES('migration_done','true')")
  self.bind(self.audit)
 def bind(self,audit):
  audit.states=lambda requests:{r:copy.deepcopy(self.states[r])for r in requests}
  audit.state=lambda request:copy.deepcopy(self.states[request])
  def apply(action,request,*args):
   self.sent.append(request)
   job=self.states[request]['jobs'][0]
   job['state']='passed'
   self.worker.rpc.confirm_audit(request,job['commitment'],slot=job['slot'],attempt=job['attempt'])
   return dict(available=True)
  audit.apply=apply
 def waiting(self):
  with self.audit.answers.database()as db:return dict(db.execute('SELECT name,waiting FROM answers'))
 def test_missing_original_seal_does_not_starve_another_valid_result(self):
  broken=self.names[0]
  (self.audit.folder/(broken+'.json')).unlink()
  (self.audit.folder/'sealed-answers'/(broken+'.json')).unlink()
  self.audit.reveal_committed()
  self.assertEqual(self.sent,[self.names[1][:64]])
  self.assertEqual(self.waiting(),{self.names[0]:1,self.names[1]:0})
  self.assertEqual(len(self.audit.answers.publication_issues),1)
  self.audit=NetworkAudits(self.worker);self.bind(self.audit);self.audit.reveal_committed()
  self.assertEqual(self.sent,[self.names[1][:64]],'Restart must not resubmit an acknowledged result')
  self.assertEqual(self.waiting()[broken],1,'Missing data must never receive a verdict or ACK')
 def test_corrupt_original_and_recovery_copy_do_not_starve_a_valid_result(self):
  broken=self.names[0];file=self.audit.folder/(broken+'.json');value=json.loads(file.read_bytes())
  value['answer']['body']['nonce']='e'*64;save(file,value)
  save(self.audit.folder/'sealed-answers'/(broken+'.json'),value)
  self.audit.reveal_committed()
  self.assertEqual(self.sent,[self.names[1][:64]])
  self.assertEqual(self.waiting()[broken],1)
  self.assertEqual(json.loads(file.read_bytes()),value,'Invalid data must be preserved for diagnosis')
 def test_transport_failure_is_not_acknowledged_or_hidden(self):
  def unavailable(*args):raise ConnectionError('RPC unavailable')
  self.audit.apply=unavailable
  with self.assertRaises(ConnectionError):self.audit.reveal_committed()
  self.assertEqual(list(self.waiting().values()),[1,1])
 def test_foreign_legacy_signer_does_not_pin_a_wallet_owned_result(self):
  original=self.worker.rpc.call;script='0014'+'ab'*20
  self.worker.address=lambda:'current-wallet-address'
  self.states[self.names[0][:64]]['jobs'][0]['actor']=script
  def call(method,args=None,**kwargs):
   if method=='decodescript':return dict(address='foreign-original-signer')
   if method=='getaddressinfo':return dict(ismine=False,scriptPubKey=script)
   return original(method,args,**kwargs)
  self.worker.rpc.call=call;publish=self.audit.apply
  def apply(action,request,*args):
   if request==self.names[0][:64]:return self.audit.event(action,request,*args)
   return publish(action,request,*args)
  self.audit.apply=apply;self.audit.reveal_committed()
  self.assertEqual(self.sent,[self.names[1][:64]])
  self.assertEqual(self.waiting(),{self.names[0]:1,self.names[1]:0})
  self.assertEqual(len(self.audit.answers.publication_issues),1)

class OwnerSignerTests(unittest.TestCase):
 def fixture(self,*,owned=True,body_changed=False):
  body=b'{"owner":"retained nonce and exact counts"}';script='0014'+'ab'*20;calls=[]
  def call(method,args,**kwargs):
   calls.append((method,args,kwargs))
   if method=='getpgnreports':return [dict(request_id='1'*64,report_root=hashlib.sha256(body).hexdigest(),miner_script=script)]
   if method=='decodescript':return dict(address='original-owner')
   if method=='getaddressinfo':return dict(ismine=owned,scriptPubKey=script)
   if method=='getpgnauditevent':return dict(available=True,challenge='original report challenge',envelope='00')
   if method=='signmessage':return 'signature'
   raise AssertionError(method)
  audit=object.__new__(NetworkAudits);audit.worker=SimpleNamespace(address=lambda:'current-address',rpc=SimpleNamespace(call=call))
  return audit,body if not body_changed else b'changed nonce',calls
 def test_owner_report_rotation_uses_original_wallet_owned_key(self):
  audit,body,calls=self.fixture();audit.event('reveal_report','1'*64,0,0,body.hex())
  self.assertEqual([a[1]for m,a,_ in calls if m=='getpgnauditevent'],['original-owner']*2)
  self.assertEqual([a[0]for m,a,_ in calls if m=='signmessage'],['original-owner'])
 def test_foreign_owner_key_cannot_be_substituted(self):
  audit,body,calls=self.fixture(owned=False)
  with self.assertRaisesRegex(RuntimeError,'key is unavailable'):audit.event('reveal_report','1'*64,0,0,body.hex())
  self.assertFalse(any(m in ('signmessage','getpgnauditevent')for m,_,_ in calls))
 def test_owner_body_must_match_original_commitment_before_signing(self):
  audit,body,calls=self.fixture(body_changed=True)
  with self.assertRaisesRegex(RuntimeError,'canonical commitment'):audit.event('reveal_report','1'*64,0,0,body.hex())
  self.assertFalse(any(m in ('signmessage','getpgnauditevent','getaddressinfo')for m,_,_ in calls))

if __name__=='__main__':unittest.main()
