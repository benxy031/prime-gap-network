"""Crash, concurrency and parent-conflict tests for ready audit publication."""
from pathlib import Path
from types import SimpleNamespace
import copy, json, sys, tempfile, threading, unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'src/engine/runtime_host'),str(ROOT/'src/protocol')]
from horizon_audit_batch import apply_group,reserve_assignments,commit_ready,save
from horizon_coordination import native_header_hash
from horizon_core_rpc import CoreRpcError

PARENT='a'*64;REQUEST='c'*64

class Fixture:
    def __init__(self, root):
        self.root=root;self.tip=PARENT;self.lock=threading.RLock();self.jobs={};self.sent=[];self.intents={};self.calls=[];self.crash=False;self.conflict=False
        self.peers=[]
        for index in range(2):
            folder=root/str(index)/'audits';folder.mkdir(parents=True)
            worker=SimpleNamespace(rpc=self,network_lock=self.lock,quitting=threading.Event(),ledger=self.ledger)
            peer=SimpleNamespace(folder=folder,worker=worker)
            peer.event=lambda action,request=None,slot=None,attempt=None,body=None,preceding_events=None,i=index:self.event(i,action,request,slot,attempt,body,preceding_events)
            peer.state=lambda request:self.state(request)
            self.peers.append(peer)
        for peer in self.peers:
            peer.worker.audit_publishers=tuple(self.peers);peer.worker.audit_group_journal=root/'assign-group.json'
    def call(self,method,params=None,**kwargs):
        if method=='getbestblockhash':return self.tip
        if method=='getpgnreports':return [dict(ticket='d'*64,report_root='e'*64)]
        if method=='getblockheader':
            if params[0] not in self.intents:raise CoreRpcError(-5,'unknown block')
            return dict(hash=params[0],height=1,confirmations=1 if self.tip==params[0] else -1)
        if method=='getblockhash':return self.tip
        raise AssertionError(method)
    def state(self,request):return dict(jobs=[dict(slot=s,attempt=0,**row)for s,row in sorted(self.jobs.items())])
    def event(self,index,action,request,slot,attempt,body,prefix):
        prefix=prefix or [];self.calls.append((index,action,self.tip,tuple(prefix)))
        proposal=dict(available=True,proposal_context_version=1,parent=self.tip,request_id=REQUEST,slot=len(prefix) if action=='assign' else slot,attempt=0,ranges=[dict(index=1,start='10',end='20')])
        proposal['envelope']=json.dumps(dict(index=index,action=action,parent=self.tip,slot=proposal['slot'],body=body),sort_keys=True).encode().hex()
        return proposal
    def ledger(self,envelopes,*,on_intent=None):
        if self.conflict:
            self.conflict=False;self.tip='b'*64
            raise CoreRpcError(-25,'parent changed before template')
        raw=bytearray(113);raw[4:36]=bytes.fromhex(self.tip)[::-1];raw[80]=2
        raw[-1]=len(self.sent)
        intent=dict(parent=self.tip,block=raw.hex(),hash=native_header_hash(raw))
        if on_intent:on_intent(intent)
        for envelope in envelopes:
            event=json.loads(bytes.fromhex(envelope));assert event['parent']==self.tip
            if event['action']=='assign':
                assert event['slot'] not in self.jobs
                self.jobs[event['slot']]=dict(state='assigned',ranges=[dict(index=1,start='10',end='20')])
            elif event['action']=='commit':self.jobs[event['slot']].update(state='committed',commitment=event['body'])
        self.tip=intent['hash'];self.intents[self.tip]=intent;self.sent.append(envelopes)
        if self.crash:self.crash=False;raise OSError('simulated crash after Core acceptance')
        return dict(block_hash=self.tip)

class BatchTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.f=Fixture(Path(self.temp.name))
    def test_two_concurrent_reservations_produce_one_block_and_two_journals(self):
        gate=threading.Barrier(2);errors=[]
        def run(peer):
            try:gate.wait();reserve_assignments(peer)
            except BaseException as error:errors.append(error)
        threads=[threading.Thread(target=run,args=(p,))for p in self.f.peers]
        for thread in threads:thread.start()
        for thread in threads:thread.join(3);self.assertFalse(thread.is_alive())
        self.assertEqual(errors,[]);self.assertEqual(len(self.f.sent),1)
        self.assertEqual([json.loads((p.folder/'current.json').read_bytes())['slot']for p in self.f.peers],[0,1])
    def test_crash_after_acceptance_recovers_both_before_new_assignment(self):
        self.f.crash=True
        with self.assertRaises(OSError):reserve_assignments(self.f.peers[0])
        self.assertFalse((self.f.peers[0].folder/'current.json').exists())
        recovered=reserve_assignments(self.f.peers[1]);self.assertEqual(recovered['slot'],1)
        self.assertEqual(len(self.f.sent),1)
        self.assertTrue((self.f.peers[0].folder/'current.json').exists())
    def test_parent_conflict_resigns_whole_prefix_without_partial_send(self):
        self.f.conflict=True;reserve_assignments(self.f.peers[0]);self.assertEqual(len(self.f.sent),1)
        self.assertEqual([call[2]for call in self.f.calls],[PARENT,PARENT,PARENT,'b'*64,'b'*64])
        self.assertEqual(json.loads(bytes.fromhex(self.f.calls[-1][3][0]))['parent'],'b'*64)
    def ready(self):
        reserve_assignments(self.f.peers[0]);audits=[]
        for index,peer in enumerate(self.f.peers):
            audit=json.loads((peer.folder/'current.json').read_bytes());audit.update(commit_ready=True,answer=dict(commitment=str(index+1)*64))
            save(peer.folder/'current.json',audit);audits.append(audit)
        return audits
    def test_two_commits_and_replay_have_one_confirmation(self):
        audits=self.ready();a=commit_ready(self.f.peers[0],audits[0],lambda _:None);b=commit_ready(self.f.peers[1],audits[1],lambda _:None)
        self.assertEqual(a['confirmed_block'],b['confirmed_block']);self.assertEqual(len(self.f.sent),2);self.assertEqual(len(self.f.sent[-1]),2)
    def test_two_concurrent_commit_producers_share_one_confirmation(self):
        audits=self.ready();gate=threading.Barrier(2);results=[];errors=[]
        def run(index):
            try:gate.wait();results.append(commit_ready(self.f.peers[index],audits[index],lambda _:None))
            except BaseException as error:errors.append(error)
        threads=[threading.Thread(target=run,args=(i,))for i in range(2)]
        for thread in threads:thread.start()
        for thread in threads:thread.join(3);self.assertFalse(thread.is_alive())
        self.assertEqual(errors,[]);self.assertEqual(len(self.f.sent),2)
        self.assertEqual(len({result['confirmed_block']for result in results}),1)
    def test_slow_peer_does_not_delay_ready_commit(self):
        audits=self.ready();delayed=copy.deepcopy(audits[1]);delayed.pop('commit_ready');save(self.f.peers[1].folder/'current.json',delayed)
        commit_ready(self.f.peers[0],audits[0],lambda _:None)
        self.assertEqual(len(self.f.sent[-1]),1);self.assertEqual(self.f.jobs[1]['state'],'assigned')
    def test_crash_after_commit_send_recovers_original_intent(self):
        audits=self.ready();self.f.crash=True
        with self.assertRaises(OSError):commit_ready(self.f.peers[0],audits[0],lambda _:None)
        receipt=commit_ready(self.f.peers[1],audits[1],lambda _:None)
        self.assertEqual(receipt['confirmed_block'],self.f.tip);self.assertEqual(len(self.f.sent),2)
    def test_conflicting_retained_lane_is_never_overwritten(self):
        self.f.crash=True
        with self.assertRaises(OSError):reserve_assignments(self.f.peers[0])
        foreign=dict(request_id='f'*64,slot=0,attempt=0);save(self.f.peers[0].folder/'current.json',foreign)
        with self.assertRaisesRegex(ValueError,'conflicts'):reserve_assignments(self.f.peers[1])
        self.assertEqual(json.loads((self.f.peers[0].folder/'current.json').read_bytes()),foreign)
    def test_changed_recovery_ranges_have_no_partial_lane_writes(self):
        self.f.crash=True
        with self.assertRaises(OSError):reserve_assignments(self.f.peers[0])
        path=self.f.root/'assign-group.json';group=json.loads(path.read_bytes())
        group['rows'][1]['audit']['ranges'][0]['end']='21';save(path,group)
        with self.assertRaisesRegex(ValueError,'ranges'):reserve_assignments(self.f.peers[1])
        self.assertTrue(all(not(p.folder/'current.json').exists()for p in self.f.peers))
        self.assertEqual(len(self.f.sent),1)
    def test_older_core_preserves_ordinary_single_publication_path(self):
        peer=self.f.peers[0];original=peer.event
        def old(*args,**kwargs):
            proposal=original(*args,**kwargs);proposal.pop('proposal_context_version');return proposal
        peer.event=old
        self.assertIs(reserve_assignments(peer),NotImplemented)
        self.assertEqual(self.f.sent,[])
    def test_real_transport_retains_group_before_shared_journal_crash(self):
        from horizon_network_worker import NetworkWorker
        raw=bytearray(113);raw[4:36]=bytes.fromhex(PARENT)[::-1];raw[80]=2
        rpc=SimpleNamespace(call=lambda method,params:dict(block=raw.hex(),parent=PARENT,numeric_target='f'*64))
        worker=object.__new__(NetworkWorker);worker.root=self.f.root;worker.rpc=rpc;worker.args=SimpleNamespace(pilot_coordination=True)
        retained=[]
        def fail_shared(path,value):
            self.assertEqual(len(retained),1)
            self.assertEqual(retained[0],value)
            raise OSError('power failure before shared journal write')
        with patch('horizon_network_worker.save',side_effect=fail_shared):
            with self.assertRaises(OSError):worker.ledger(['00'],on_intent=lambda intent:retained.append(dict(intent)))
        self.assertEqual(len(retained),1)
        self.assertFalse((self.f.root/'coordination-intent.json').exists())
    def test_cross_stage_or_authority_has_no_send(self):
        p,q=self.f.peers
        with self.assertRaises(ValueError):apply_group([(p,'assign',None,None,None,None),(q,'commit',REQUEST,0,0,'f'*64)])
        foreign=SimpleNamespace(worker=SimpleNamespace(rpc=object(),network_lock=self.f.lock))
        with self.assertRaises(ValueError):apply_group([(p,'assign',None,None,None,None),(foreign,'assign',None,None,None,None)])
        self.assertEqual(self.f.sent,[])

if __name__=='__main__':unittest.main()
