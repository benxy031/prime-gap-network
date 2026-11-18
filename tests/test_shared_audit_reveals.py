from pathlib import Path
from types import SimpleNamespace
import json,sys,tempfile,threading,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'src/protocol'),str(ROOT/'src/engine/runtime_host'),str(ROOT/'tests')]
from horizon_network_audits import NetworkAudits,RetainedAuditPublicationError,save
from horizon_report_poll import ReportPoller
from audit_publication_chain_fixture import ChainRPC
from report_commitment import digest

class SharedAuditReveals(unittest.TestCase):
    def setUp(self):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup);self.root=Path(tmp.name)
        self.rpc=ChainRPC();self.lock=threading.RLock();self.stopping=threading.Event()
        self.blocks=[];self.fail=False;self.stages={};self.names=[];self.now=0
        def ledger(envelopes):
            self.blocks.append(list(envelopes))
            if self.fail:raise TimeoutError('Ambiguous outcome')
            return dict(block_hash='d'*64)
        self.worker=SimpleNamespace(root=self.root,rpc=self.rpc,network_lock=self.lock,quitting=self.stopping,ledger=ledger,batch_audit_reveals=True)
        self.owner=NetworkAudits(self.worker);self.lanes=[]
        for i in range(2):
            # Match production: lane wrappers have no capability flag.
            worker=SimpleNamespace(root=self.root/str(i),rpc=self.rpc,network_lock=self.lock,quitting=self.stopping,ledger=ledger)
            worker.root.mkdir();lane=NetworkAudits(worker);self.lanes.append(lane)
            request=f'{i+1:064x}';name=request+'-0-0';self.names.append(name);self.stages[request]='committed'
            body=dict(counts=['7'],nonce=f'{i+1:064x}')
            commitment=digest(body)
            audit=dict(request_id=request,slot=0,attempt=0,answer=dict(body=body,commitment=commitment))
            self.rpc.audit_jobs[request]=dict(slot=0,attempt=0,state='passed',commitment=commitment)
            save(lane.folder/(name+'.json'),audit);lane.answers.register([name])
            with lane.answers.database()as db:db.execute("INSERT OR REPLACE INTO meta VALUES('migration_done','true')")
            lane.states=lambda requests:{r:dict(details_root='c'*64,jobs=[dict(slot=0,attempt=0,state=self.stages[r])])for r in requests}
            lane.event=lambda action,request,slot,attempt,body:dict(available=True,envelope=request)
        self.poll=ReportPoller(self.owner,self.lanes,clock=lambda:self.now)
    def waiting(self,lane):
        with lane.answers.database()as db:return db.execute('SELECT sum(waiting) FROM answers').fetchone()[0]
    def test_two_actual_lane_catalogues_use_one_block_then_canonical_ack(self):
        self.assertIsNone(self.poll.poll_audits())
        self.assertEqual(len(self.blocks),1);self.assertEqual(len(self.blocks[0]),2)
        self.assertEqual([self.waiting(a)for a in self.lanes],[1,1])
        for r in self.stages:self.stages[r]='passed'
        self.now=6;self.assertIsNone(self.poll.poll_audits())
        self.assertEqual(len(self.blocks),1);self.assertEqual([self.waiting(a)for a in self.lanes],[0,0])
        self.assertTrue(all((a.folder/(name+'.json')).is_file()for a,name in zip(self.lanes,self.names)))
    def test_ambiguous_shared_send_retains_both_and_reconciles_without_resend(self):
        self.fail=True;self.assertIn('Ambiguous',self.poll.poll_audits())
        self.assertEqual([self.waiting(a)for a in self.lanes],[1,1])
        for r in self.stages:self.stages[r]='passed'
        self.now=6;self.fail=False;self.assertIsNone(self.poll.poll_audits())
        self.assertEqual(len(self.blocks),1);self.assertEqual([self.waiting(a)for a in self.lanes],[0,0])
    def test_foreign_authority_rejected_before_submission(self):
        a=self.lanes[0];a.worker.rpc=ChainRPC()
        value=json.loads((a.folder/(self.names[0]+'.json')).read_bytes())
        with self.assertRaisesRegex(ValueError,'crosses authority'):self.owner.apply_reveal_group([(a,self.names[0],value)])
        self.assertEqual(self.blocks,[])
    def test_bad_original_signer_isolated_from_other_lane(self):
        def bad(*args):raise RetainedAuditPublicationError('Original key unavailable')
        self.lanes[0].event=bad;self.assertIn('Original key',self.poll.poll_audits())
        self.assertEqual(len(self.blocks),1);self.assertEqual(len(self.blocks[0]),1)
        self.assertEqual([self.waiting(a)for a in self.lanes],[1,1])

if __name__=='__main__':unittest.main()
