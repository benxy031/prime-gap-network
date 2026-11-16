import hashlib,unittest
from native_registry import NativeRegistry
from authority import Denied,Unavailable
class RegistryTests(unittest.TestCase):
 def setUp(self):
  self.genesis='7'*64;self.script='0014'+'1'*40;self.op='a'*64;self.address='qa-public-address';self.row=dict(signing_script=self.script,operator_id=self.op);self.root=dict(genesis=self.genesis,height=123,bestblockhash='b'*64,participants=[self.row]);self.proof=True;self.canonical=True
  self.registry=NativeRegistry(self.rpc,self.genesis)
 def rpc(self,method,args):
  if method=='validateaddress':return dict(isvalid=True,scriptPubKey=self.script)
  if method=='verifymessage':return self.proof
  if method=='getpgnparticipants':return self.root
  if method=='getblockhash':return'b'*64 if self.canonical else'c'*64
  raise AssertionError('Unexpected RPC')
 def bind(self):self.assertTrue(self.registry.verify_and_bind(self.script,self.address,'fixture challenge','fixture signature'))
 def test_no_account_resolution_before_key_proof(self):
  with self.assertRaises(Denied):self.registry.resolve(self.script)
  self.proof=False;self.assertFalse(self.registry.verify_and_bind(self.script,self.address,'x','x'))
  with self.assertRaises(Denied):self.registry.resolve(self.script)
 def test_exact_admitted_alias_and_native_fallback(self):
  self.bind();self.assertEqual(self.registry.resolve(self.script),self.op)
  self.root['participants']=[];self.assertEqual(self.registry.resolve(self.script),hashlib.sha256(bytes.fromhex(self.script)).hexdigest())
 def test_wrong_genesis_reorg_and_mismatched_alias_are_not_grants(self):
  self.bind();self.root['genesis']='8'*64
  with self.assertRaises(Unavailable):self.registry.resolve(self.script)
  self.root['genesis']=self.genesis;self.canonical=False
  with self.assertRaises(Unavailable):self.registry.resolve(self.script)
  self.canonical=True;self.row['signing_script']='0014'+'2'*40
  with self.assertRaises(Unavailable):self.registry.resolve(self.script)
 def test_another_address_cannot_bind_a_claimed_script(self):
  self.assertFalse(self.registry.verify_and_bind('0014'+'2'*40,self.address,'x','x'))

if __name__=='__main__':unittest.main()
