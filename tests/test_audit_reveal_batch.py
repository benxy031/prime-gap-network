"""Durable publication/ACK tests with controlled competing-chain faults."""
from pathlib import Path
from types import SimpleNamespace
import copy,json,sys,tempfile,threading,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'src/protocol'),str(ROOT/'src/engine/runtime_host'),str(ROOT/'tests')]
from horizon_network_audits import NetworkAudits,RetainedAuditPublicationError,save
from horizon_coordination import CoordinationParentChanged
from audit_publication_chain_fixture import ChainRPC
from report_commitment import digest

class RevealBatch(unittest.TestCase):
    def setUp(self):
        t=tempfile.TemporaryDirectory();self.addCleanup(t.cleanup)
        self.rpc=ChainRPC();self.blocks=[];self.sent=[];self.fail=None;self.bad=set();self.status='committed'
        self.worker=SimpleNamespace(root=Path(t.name),rpc=self.rpc,network_lock=threading.RLock(),batch_audit_reveals=True,quitting=threading.Event(),ledger=self.ledger)
        self.a=NetworkAudits(self.worker);self.ready=[];self.original={}
        for i in range(1,5):
            request=f'{i:064x}';name=request+'-0-0'
            body=dict(counts=['7'],nonce=f'{i+1:064x}')
            commitment=digest(body)
            audit=dict(request_id=request,slot=0,attempt=0,answer=dict(body=body,commitment=commitment))
            self.rpc.audit_jobs[request]=dict(slot=0,attempt=0,state='passed',commitment=commitment)
            save(self.a.folder/(name+'.json'),audit);self.ready.append((name,audit));self.original[name]=(self.a.folder/(name+'.json')).read_bytes()
        self.a.answers.register([n for n,_ in self.ready])
        with self.a.answers.database()as db:db.execute("INSERT OR REPLACE INTO meta VALUES('migration_done','true')")
        self.a.states=lambda requests:{r:dict(details_root='c'*64,jobs=[dict(slot=0,attempt=0,state=self.status)])for r in requests}
        self.a.state=lambda request:dict(jobs=[dict(slot=0,attempt=0,state='passed')])
        self.a.event=self.event
    def event(self,action,request,slot,attempt,body):
        self.sent.append((action,request,body))
        if request in self.bad:raise RetainedAuditPublicationError('Retained signer unavailable')
        return dict(available=True,envelope=bytes.fromhex(request).hex())
    def ledger(self,envelopes):
        self.blocks.append(list(envelopes))
        if self.fail:raise self.fail
        return dict(block_hash='d'*64)
    def waiting(self):
        with self.a.answers.database()as db:return dict(db.execute('SELECT name,waiting FROM answers'))
    def test_four_answers_one_block_and_durable_ack(self):
        self.a.reveal_committed()
        self.assertEqual(len(self.blocks),1);self.assertEqual(len(self.blocks[0]),4)
        self.assertTrue(all(v==0 for v in self.waiting().values()))
        for name,_ in self.ready:self.assertEqual((self.a.folder/(name+'.json')).read_bytes(),self.original[name])
    def test_acceptance_not_observed_on_branch_retains_every_answer(self):
        self.rpc.view_tip='e'*64
        self.a.reveal_committed()
        self.assertEqual(len(self.blocks),1)
        self.assertTrue(all(v==1 for v in self.waiting().values()))
        self.status='passed';self.rpc.view_tip='1'*64
        NetworkAudits(self.worker) # Real catalogue reopening must preserve pending identities.
        self.a.reveal_committed()
        self.assertEqual(len(self.blocks),1);self.assertTrue(all(v==0 for v in self.waiting().values()))
    def test_unavailable_key_does_not_starve_other_answers(self):
        self.bad.add(self.ready[0][1]['request_id']);self.a.reveal_committed()
        self.assertEqual(len(self.blocks),1);self.assertEqual(len(self.blocks[0]),3)
        self.assertEqual(self.waiting()[self.ready[0][0]],1)
        self.assertEqual(sum(self.waiting().values()),1);self.assertEqual(len(self.a.answers.publication_issues),1)
    def test_ambiguous_send_never_acknowledges(self):
        self.fail=TimeoutError('Send outcome unknown')
        with self.assertRaises(TimeoutError):self.a.reveal_committed()
        self.assertTrue(all(v==1 for v in self.waiting().values()))
        self.fail=None;self.status='passed';self.a.reveal_committed()
        self.assertEqual(len(self.blocks),1);self.assertTrue(all(v==0 for v in self.waiting().values()))
    def test_parent_change_rebuilds_signatures_before_retry(self):
        original=self.worker.ledger
        def changed(envelopes):
            if not self.blocks:self.blocks.append([]);raise CoordinationParentChanged('Parent changed')
            return original(envelopes)
        self.worker.ledger=changed;self.a.reveal_committed()
        self.assertEqual(len(self.blocks),2);self.assertEqual(len(self.sent),8)
        self.assertTrue(all(v==0 for v in self.waiting().values()))
    def test_stop_does_not_send_or_mutate_answers(self):
        self.worker.quitting.set();self.a.reveal_committed()
        self.assertEqual(self.blocks,[]);self.assertTrue(all(v==1 for v in self.waiting().values()))
    def test_oversized_page_refused_before_sending(self):
        with self.assertRaises(ValueError):self.a.apply_reveals(self.ready*17)
        self.assertEqual(self.blocks,[])
    def test_repeated_identity_refused_before_sending(self):
        with self.assertRaises(ValueError):self.a.apply_reveals(self.ready[:1]*2)
        self.assertEqual(self.blocks,[])
    def test_missing_confirmation_keeps_all_answers(self):
        self.worker.ledger=lambda env:None
        with self.assertRaises(RuntimeError):self.a.reveal_committed()
        self.assertTrue(all(v==1 for v in self.waiting().values()))

if __name__=='__main__':unittest.main()
