"""Exact decimal proof targets survive resident search and audit preparation."""
from pathlib import Path
from types import SimpleNamespace
import copy,json,sys,tempfile,threading,unittest
P=Path(__file__).resolve().parents[1];sys.path[:0]=[str(P/'src/engine/runtime_host'),str(P/'src/protocol')]
from pgn_native_contract import encode_target_q16
from horizon_mining_flow import MiningFlow
from horizon_network_audits import NetworkAudits,save

class HourlyRuntimeTargetTests(unittest.TestCase):
 def test_every_decimal_target_keeps_exact_fraction_and_native_header_identifier(self):
  for tenth in range(275,641):
   value=dict(target_numerator=str(tenth),target_denominator='10');original=copy.deepcopy(value)
   encoded=encode_target_q16(value)
   self.assertGreaterEqual(encoded*10,tenth*65536)
   self.assertLess((encoded-1)*10,tenth*65536)
   self.assertEqual(value,original,'Encoding must not rebind the signed proof fraction')
 def test_invalid_or_unbounded_fractions_fail_before_computation(self):
  for numerator,denominator in [('0','1'),('1','0'),('65','1'),(True,'1'),('01','1'),('1','-1'),('9'*65,'1')]:
   with self.subTest(n=numerator,d=denominator),self.assertRaises(ValueError):encode_target_q16(dict(target_numerator=numerator,target_denominator=denominator))
 def test_resident_search_accepts_27_point_6_without_rewriting_work(self):
  flow=object.__new__(MiningFlow)
  work=dict(ticket='a'*64,start='150000000000000000000',length='200000000000000',target_numerator='138',target_denominator='5')
  before=copy.deepcopy(work);job=flow.job(work)
  self.assertEqual(job['target_merit_q16'],1808794);self.assertEqual(work,before)
 def test_audit_completes_all_five_partitions_at_nonbinary_decimal_target(self):
  with tempfile.TemporaryDirectory()as temp:
   root=Path(temp);running=threading.Event();running.set();computed=[]
   def rpc(method,args):
    self.assertEqual(method,'getpgnscientificwork');return dict(target_numerator='138',target_denominator='5')
   def execute(job,limit):
    computed.append(job);return dict(state='COMPLETE',body=dict(counts=['1']))
   worker=SimpleNamespace(root=root,running=running,quitting=threading.Event(),network_lock=threading.RLock(),args=SimpleNamespace(piece_limit=1),rpc=SimpleNamespace(call=rpc),source=SimpleNamespace(run_ticket=execute))
   audit=NetworkAudits(worker);request='b'*64
   ranges=[dict(index=i,start=str(150000000000000000000+i*2000000000000),end=str(150000000000000000000+(i+1)*2000000000000))for i in range(5)]
   value=dict(request_id=request,ticket='c'*64,report_root='d'*64,slot=0,attempt=0,ranges=ranges,counts=[])
   save(audit.folder/'current.json',value);audit.state=lambda _:dict(jobs=[dict(slot=0,attempt=0,state='assigned')])
   sent=[];audit.apply=lambda *args,**kwargs:sent.append(args)or dict(available=True)
   self.assertEqual(audit.process_one(),'committed');self.assertEqual(len(computed),5)
   self.assertEqual({v['target_merit_q16']for v in computed},{1808794})
   sealed=json.loads((audit.folder/(request+'-0-0.json')).read_bytes())
   self.assertEqual(sealed['answer']['body']['counts'],['1']*5);self.assertEqual(sent[0][0],'commit')
   self.assertFalse((audit.folder/'current.json').exists())

if __name__=='__main__':unittest.main()
