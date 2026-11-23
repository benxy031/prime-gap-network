"""Crash, concurrency and parent-conflict tests for ready audit publication."""
from pathlib import Path
from types import SimpleNamespace
import copy, json, sys, tempfile, threading, unittest
from unittest.mock import patch
from horizon_audit_batch import apply_group,reserve_assignments,commit_ready,save,LEGACY_AUDIT_USAGE,retain_assignments
from horizon_coordination import PublicationPending, native_header_hash
from horizon_core_rpc import CoreRpcError
from horizon_network_audits import NetworkAudits

PARENT='a'*64;REQUEST='c'*64

def commitment_script(event):
    actor=bytes.fromhex('0014'+str(event['index']+1)*40)
    message=(b'\x01'+bytes.fromhex(event['request'])[::-1]+bytes.fromhex(event['parent'])[::-1]
             +bytes([1,event['slot']])+(0).to_bytes(4,'little')+bytes([len(actor)])+actor
             +b'\x20'+bytes.fromhex(event['body'])+b'\x01\x01')
    return (b'\x6a\x10PGNAUDIT-EVENT-1\x4c'+bytes([len(message)])+message).hex()

class Fixture:
    def __init__(self, root):
        self.root=root;self.tip=PARENT;self.lock=threading.RLock();self.jobs={};self.sent=[];self.intents={};self.calls=[];self.crash=False;self.conflict=False;self.more_work=False;self.additional_jobs={};self.watchonly=False
        self.omit_audit_projection=False
        self.report_root='e'*64
        self.block_outputs={}
        self.peers=[]
        for index in range(2):
            folder=root/str(index)/'audits';folder.mkdir(parents=True)
            worker=SimpleNamespace(rpc=self,network_lock=self.lock,quitting=threading.Event(),ledger=self.ledger,address=lambda i=index:str(i))
            peer=SimpleNamespace(folder=folder,worker=worker)
            peer.event=lambda action,request=None,slot=None,attempt=None,body=None,preceding_events=None,i=index:self.event(i,action,request,slot,attempt,body,preceding_events)
            peer.state=lambda request:self.state(request)
            self.peers.append(peer)
        for peer in self.peers:
            peer.worker.audit_publishers=tuple(self.peers);peer.worker.audit_group_journal=root/'assign-group.json'
    def call(self,method,params=None,**kwargs):
        if method=='getbestblockhash':return self.tip
        if method=='getpgnauditevent':return dict(proposal_context_version=1)
        if method=='decodescript':return dict(address='owned-'+params[0])
        if method=='getaddressinfo':return dict(ismine=True,iswatchonly=self.watchonly,scriptPubKey=params[0][6:])
        if method=='getwalletinfo':return dict(private_keys_enabled=True)
        if method=='getpgnreports':return [dict(ticket='d'*64,report_root=self.report_root)]
        if method=='getpgnscientificwork':return dict(target_numerator='27',target_denominator='1')
        if method=='getpgnauditstate':return [self.state(params[2])]
        if method=='getpgnauditactivity':raise CoreRpcError(-32601,'Method not found')
        if method=='getpgnworkview':
            request=params[3]
            return dict(schema='PGN_WORK_VIEW_V1',kind='audits',request_id=request,
                        state=dict(bestblockhash=self.tip,genesis='7'*64),
                        reports=[dict(request_id=request,ticket='d'*64,report_root=self.report_root)],
                        audits=[] if self.omit_audit_projection else [dict(request_id=request,**self.state(request))])
        if method=='getblockheader':
            if params[0] not in self.intents:raise CoreRpcError(-5,'unknown block')
            return dict(hash=params[0],height=1,confirmations=1 if self.tip==params[0] else -1)
        if method=='getblockhash':return self.tip
        if method=='getblock':
            return dict(hash=params[0],previousblockhash=self.intents[params[0]]['parent'],
                        tx=[dict(vin=[dict(coinbase='')],vout=self.block_outputs[params[0]])])
        raise AssertionError(method)
    def state(self,request):
        jobs=self.additional_jobs if request=='f'*64 else self.jobs
        return dict(jobs=[dict(dict(slot=s,attempt=0),**row)for s,row in sorted(jobs.items())])
    def event(self,index,action,request,slot,attempt,body,prefix):
        prefix=prefix or [];self.calls.append((index,action,self.tip,tuple(prefix)))
        request=('f'*64 if self.more_work else REQUEST) if action=='assign' else request
        proposal=dict(available=True,proposal_context_version=1,parent=self.tip,actor='0014'+str(index+1)*40,request_id=request,slot=len(prefix) if action=='assign' else slot,attempt=0,ranges=[dict(index=1,start='10',end='20')])
        proposal['envelope']=json.dumps(dict(index=index,action=action,parent=self.tip,request=request,slot=proposal['slot'],body=body),sort_keys=True).encode().hex()
        return proposal
    def ledger(self,envelopes,*,on_intent=None):
        if self.conflict:
            self.conflict=False;self.tip='b'*64
            raise CoreRpcError(-25,'parent changed before template')
        raw=bytearray(113);raw[4:36]=bytes.fromhex(self.tip)[::-1];raw[80]=2
        raw[-1]=len(self.sent)
        intent=dict(parent=self.tip,block=raw.hex(),hash=native_header_hash(raw))
        if on_intent:on_intent(intent)
        outputs=[]
        for envelope in envelopes:
            event=json.loads(bytes.fromhex(envelope));assert event['parent']==self.tip
            jobs=self.additional_jobs if event['request']=='f'*64 else self.jobs
            if event['action']=='assign':
                assert event['slot'] not in jobs
                jobs[event['slot']]=dict(state='assigned',actor='0014'+str(event['index']+1)*40,expired=False,actor_policy_accepted=True,ranges=[dict(index=1,start='10',end='20')])
            elif event['action']=='commit':
                jobs[event['slot']].update(state='committed',commitment=event['body'])
                outputs.append(dict(value=0,scriptPubKey=dict(hex=commitment_script(event))))
        self.block_outputs[intent['hash']]=outputs
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
        self.assertEqual([call[2]for call in self.f.calls],[PARENT,PARENT,'b'*64,'b'*64])
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
    def test_ready_process_resume_keeps_the_other_lanes_block_receipt(self):
        self.ready()
        first,second=self.f.peers
        first.worker.root=first.folder.parent
        completed=[]
        publisher=NetworkAudits(first.worker,on_committed=lambda audit,block:completed.append((audit,block)))
        first.worker.audit_publishers=second.worker.audit_publishers=(publisher,second)
        publisher.event=first.event
        invoke=publisher.resume_grouped_commit
        def after_old_cursor_read(journal,observed):
            self.assertNotIn("publication_intent",observed)
            commit_ready(second,json.loads((second.folder/'current.json').read_bytes()),lambda _:None)
            self.assertIn("publication_intent",json.loads(journal.read_bytes()))
            return invoke(journal,observed)
        with patch.object(publisher,'resume_grouped_commit',side_effect=after_old_cursor_read):
            self.assertEqual(publisher.process_one(),'committed')
        self.assertEqual(len(self.f.sent),2)
        self.assertEqual(len(self.f.sent[-1]),2)
        self.assertEqual(len(completed),1)
        self.assertEqual(completed[0][1],self.f.tip)
        self.assertFalse((first.folder/'current.json').exists())
    def test_matching_chain_job_cannot_inherit_the_other_lanes_block(self):
        audits=self.ready()
        first,second=self.f.peers
        second.event=lambda *args,**kwargs:dict(available=False)
        commit_ready(first,audits[0],lambda _:None)
        path=second.folder/'current.json'
        forged=dict(audits[1],publication_intent=self.f.intents[self.f.tip])
        save(path,forged)
        self.f.jobs[1].update(state='committed',commitment=audits[1]['answer']['commitment'])
        with self.assertRaisesRegex(ValueError,'Active block lacks'):
            commit_ready(second,forged,lambda _:None)
        self.assertEqual(json.loads(path.read_bytes()),forged)
    def test_replaced_ready_cursor_with_the_same_answer_is_rejected(self):
        audits=self.ready()
        first=self.f.peers[0]
        first.worker.root=first.folder.parent
        publisher=NetworkAudits(first.worker,on_committed=lambda *_:self.fail('Wrong cursor credited'))
        altered=dict(audits[0],attempt=audits[0]['attempt']+1)
        path=first.folder/'current.json'
        save(path,altered)
        with self.assertRaisesRegex(RuntimeError,'changed during reconciliation'):
            publisher.resume_grouped_commit(path,audits[0])
        self.assertEqual(json.loads(path.read_bytes()),altered)
    def test_restart_after_callback_reconciles_an_orphaned_commit_before_resending(self):
        audits=self.ready()
        first,second=self.f.peers
        old=commit_ready(first,audits[0],lambda _:None)['confirmed_block']
        self.f.tip='b'*64
        self.f.intents[self.f.tip]={}
        self.f.jobs[0]['state']='assigned'
        del self.f.jobs[0]['commitment']
        first.worker.root=first.folder.parent
        calls=[]
        def reopened(audit,block):
            self.assertEqual(block,old)
            calls.append('reopened')
            audit['replay_previous_block']=block
        def finished(audit,block):
            self.assertEqual(audit['replay_previous_block'],old)
            self.assertNotEqual(block,old)
            calls.append('confirmed')
        publisher=NetworkAudits(first.worker,on_committed=finished,on_reassigned=reopened,
                                reassignment_needed=lambda audit:True)
        first.worker.audit_publishers=second.worker.audit_publishers=(publisher,second)
        publisher.event=first.event
        self.assertEqual(publisher.process_one(),'committed')
        self.assertEqual(calls,['reopened','confirmed'])
        self.assertEqual(len(self.f.sent[-1]),1)
    def test_partition_change_excludes_the_old_ready_peer(self):
        audits=self.ready()
        self.f.jobs[1]['ranges']=[dict(index=1,start='20',end='30')]
        commit_ready(self.f.peers[0],audits[0],lambda _:None)
        self.assertEqual(len(self.f.sent[-1]),1)
        later=json.loads((self.f.peers[1].folder/'current.json').read_bytes())
        self.assertEqual(later,audits[1])
        self.assertEqual(self.f.jobs[1]['state'],'assigned')
    def test_mixed_branch_ranges_never_reach_signing_after_tip_returns(self):
        audits=self.ready()
        active=self.f.tip
        original=self.f.call
        def changed_view(method,params=None,**kwargs):
            value=original(method,params,**kwargs)
            if method=='getpgnworkview':
                value['state']['bestblockhash']='b'*64 if active!='b'*64 else '7'*64
            return value
        with patch.object(self.f,'call',side_effect=changed_view):
            with self.assertRaisesRegex(RuntimeError,'Audit work view changed'):
                commit_ready(self.f.peers[0],audits[0],lambda _:None)
        self.assertEqual(self.f.tip,active)
        self.assertEqual(len(self.f.sent),1)
        self.assertEqual(json.loads((self.f.peers[0].folder/'current.json').read_bytes()),audits[0])
    def test_tip_advance_during_grouped_commit_rechecks_and_resigns_only_the_caller(self):
        first, second = self.f.peers
        audits = self.ready()
        self.f.conflict = True
        before_tip = self.f.tip
        result = commit_ready(first, audits[0], lambda _:None)
        self.assertEqual(result['confirmed_block'], self.f.tip)
        self.assertEqual(len(self.f.sent), 2)
        self.assertEqual(len(self.f.sent[-1]), 1)
        sent = json.loads(bytes.fromhex(self.f.sent[-1][0]))
        self.assertEqual((sent['slot'], sent['parent'], sent['body']), (0, 'b'*64, audits[0]['answer']['commitment']))
        self.assertNotEqual(sent['parent'], before_tip)
        self.assertEqual(json.loads((second.folder/'current.json').read_bytes()), audits[1])
        self.assertEqual(self.f.jobs[1]['state'], 'assigned')

    def test_missing_canonical_attempt_is_not_a_parent_retry(self):
        audits = self.ready()
        first = self.f.peers[0]
        before = (first.folder / 'current.json').read_bytes()
        self.f.jobs.pop(0)
        with self.assertRaisesRegex(RuntimeError, 'report identity or exact attempt unavailable'):
            commit_ready(first, audits[0], lambda _:None)
        self.assertEqual((first.folder / 'current.json').read_bytes(), before)
        self.assertEqual(len(self.f.sent), 1)

    def test_unavailable_peer_keeps_its_seal_without_an_unrelated_receipt(self):
        audits=self.ready()
        other=self.f.peers[1]
        other.event=lambda *args,**kwargs:dict(available=False)
        commit_ready(self.f.peers[0],audits[0],lambda _:None)
        self.assertEqual(len(self.f.sent[-1]),1)
        self.assertEqual(json.loads((other.folder/'current.json').read_bytes()),audits[1])
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
        peer=self.f.peers[0];original=self.f.call
        def old(method,params=None,**kwargs):
            if method=='getpgnauditevent':raise CoreRpcError(-1,LEGACY_AUDIT_USAGE)
            return original(method,params,**kwargs)
        self.f.call=old
        self.assertIs(reserve_assignments(peer),NotImplemented)
        self.assertEqual(self.f.sent,[])
    def test_signature_parent_quota_or_bad_metadata_are_never_missing_capability(self):
        peer=self.f.peers[0]
        for code,text in ((-1,'Signature invalid'),(-8,'Audit quota exceeded'),(-25,'Parent mismatch')):
            def failing(*args,code=code,text=text,**kwargs):raise CoreRpcError(code,text)
            with patch.object(self.f,'call',side_effect=failing):
                with self.assertRaises(CoreRpcError):reserve_assignments(peer)
            self.assertFalse(hasattr(peer.worker,'audit_prefix_supported'))
        with patch.object(self.f,'call',return_value=dict(available=True)):
            with self.assertRaises(RuntimeError):reserve_assignments(peer)
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
    def test_nonretained_terminal_expired_reassigned_groups_resume_useful_work(self):
        for status in ('passed','failed','committed','expired','reassigned','actor_changed'):
            with self.subTest(status=status):
                root=self.f.root/status;root.mkdir();f=Fixture(root);f.crash=True
                with self.assertRaises(OSError):reserve_assignments(f.peers[0])
                journal=root/'assign-group.json';raw=journal.read_bytes();digest=__import__('hashlib').sha256(raw).hexdigest()
                proof=root/'original-seal.json';proof.write_bytes(b'original private seal must remain exact\n')
                for job in f.jobs.values():
                    if status in ('passed','failed','committed'):job['state']=status
                    elif status=='expired':job['expired']=True
                    elif status=='reassigned':job['attempt']=1
                    else:job['actor']='0014'+'9'*40
                f.more_work=True
                adopted=reserve_assignments(f.peers[0])
                self.assertEqual(adopted['request_id'],'f'*64)
                self.assertEqual(len(f.sent),2)
                self.assertEqual(proof.read_bytes(),b'original private seal must remain exact\n')
                history=root/'audit-assignment-history'
                self.assertEqual((history/(digest+'.json')).read_bytes(),raw)
                resolutions=[json.loads(p.read_bytes())for p in history.glob(digest+'-*.resolution.json')]
                self.assertEqual(len(resolutions),1)
                self.assertEqual([r['status']for r in resolutions[0]['rows']],[status,status])
                self.assertEqual(resolutions[0]['anchor'],list(f.intents)[0])
    def test_watchonly_key_and_wrong_canonical_actor_never_adopt(self):
        self.f.crash=True
        with self.assertRaises(OSError):reserve_assignments(self.f.peers[0])
        group=json.loads((self.f.root/'assign-group.json').read_bytes())
        self.f.watchonly=True
        observed=retain_assignments(self.f.peers,group)
        self.assertEqual([r['status']for r in observed['rows']],['key_unavailable']*2)
        self.assertTrue(all(not(p.folder/'current.json').exists()for p in self.f.peers))
        self.f.watchonly=False;self.f.jobs[0]['actor']='0014'+'9'*40
        observed=retain_assignments(self.f.peers,group)
        self.assertEqual(observed['rows'][0]['status'],'actor_changed')
        self.assertFalse((self.f.peers[0].folder/'current.json').exists())
        self.assertTrue((self.f.peers[1].folder/'current.json').exists())
    def test_own_expired_cursor_is_archived_before_any_computation(self):
        from horizon_network_audits import NetworkAudits
        root=self.f.root/'own-cursor';root.mkdir();retired=[]
        worker=SimpleNamespace(root=root,rpc=self.f,network_lock=self.f.lock,args=SimpleNamespace(),
            source=SimpleNamespace(run_ticket=lambda *args: self.fail('Expired work must not compute')))
        publisher=NetworkAudits(worker,on_retired=lambda value,anchor:retired.append((value,anchor)))
        publisher.state=lambda request:dict(jobs=[dict(slot=0,attempt=0,state='assigned',expired=True,actor='0014'+'1'*40)])
        audit=dict(request_id=REQUEST,slot=0,attempt=0,actor='0014'+'1'*40,ticket='d'*64,report_root='e'*64,
                   counts=[],ranges=[dict(index=i,start=str(i*10),end=str(i*10+10))for i in range(5)])
        path=publisher.folder/'current.json';save(path,audit);raw=path.read_bytes()
        self.assertEqual(publisher.process_one(),'retired');self.assertFalse(path.exists())
        kept=list((publisher.folder/'retained-assignments').glob('*.json'))
        self.assertEqual(len(kept),1);self.assertEqual(kept[0].read_bytes(),raw)
        self.assertEqual(retired,[(audit,PARENT)])

    def test_unsealed_five_counts_resume_after_cursor_archival_on_another_lane(self):
        root = self.f.root / 'recovered-progress'
        first = root / 'first'
        second = root / 'second'
        first.mkdir(parents=True)
        second.mkdir()
        retired = []
        source = SimpleNamespace(run_ticket=lambda *args: self.fail('Exact completed range must not recompute'))
        common = dict(rpc=self.f, network_lock=self.f.lock, args=SimpleNamespace(genesis='7'*64),
                      source=source, retained_audit_progress=root / 'shared', running=threading.Event())
        common['running'].set()
        old = NetworkAudits(SimpleNamespace(root=first, **common),
                            on_retired=lambda audit, anchor: retired.append((audit, anchor)))
        old.state = lambda request: {}
        audit = dict(request_id=REQUEST, ticket='d'*64, report_root='e'*64, slot=0, attempt=0,
                     actor='0014'+'1'*40, counts=[4, 9, 1, 7, 2], target_merit_q16=27*65536,
                     ranges=[dict(index=i, start=str(i*10+10), end=str(i*10+20)) for i in range(5)])
        path = old.folder / 'current.json'
        save(path, audit)
        original = path.read_bytes()
        self.f.omit_audit_projection = True
        with self.assertRaisesRegex(PublicationPending, 'view incomplete'):
            old.process_one()
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(retired, [])
        self.f.omit_audit_projection = False
        self.assertEqual(old.process_one(), 'retired')
        self.assertFalse(path.exists())
        self.assertEqual(next((old.folder/'retained-assignments').glob('*.json')).read_bytes(), original)
        self.assertEqual(retired, [(audit, PARENT)])
        fresh = {key:value for key,value in audit.items() if key != 'target_merit_q16'}
        fresh['counts'] = []
        resumed = NetworkAudits(SimpleNamespace(root=second, **common),
                                before_commit=lambda current: False)
        resumed.apply = lambda action: dict(available=False, unavailable_reason='no_eligible_independent_work')
        resumed.worker.recover_assigned_audit = lambda folder: (fresh, True)
        self.assertEqual(resumed.process_one(), 'paused')
        again = json.loads((resumed.folder/'current.json').read_bytes())
        self.assertEqual(again, audit)
        self.assertEqual(self.f.sent, [])
        self.assertEqual(resumed.restore_retired_progress(dict(fresh, attempt=1))['counts'], [])
        self.assertEqual(resumed.restore_retired_progress(dict(fresh, actor='0014'+'2'*40))['counts'], [])
        self.assertEqual(resumed.restore_retired_progress(dict(fresh, ranges=audit['ranges'][::-1]))['counts'], [])
        with self.assertRaisesRegex(ValueError, 'differ from the active prefix'):
            resumed.restore_retired_progress(dict(fresh, counts=[11]))
        with self.assertRaisesRegex(ValueError, 'differs from the active target'):
            resumed.restore_retired_progress(dict(fresh, counts=[4], target_merit_q16=26*65536))

    def test_real_claim_recovers_the_original_seal_from_an_inactive_lane(self):
        from horizon_assigned_audit_recovery import AssignedAuditRecovery
        from report_commitment import audit_commitment

        root = self.f.root / 'role-change'
        former_role = root / 'audit-lanes' / '0'
        current_role = root / 'service-audit-lanes' / '0'
        former_role.mkdir(parents=True)
        current_role.mkdir(parents=True)
        args = SimpleNamespace(genesis='7'*64)
        actor = '0014'+'1'*40
        ranges = [dict(index=i, start=str(i*10+10), end=str(i*10+20)) for i in range(5)]
        answer = audit_commitment('d'*64, 0, list(range(5)), ['4', '9', '1', '7', '2'], '8'*64)
        audit = dict(request_id=REQUEST, ticket='d'*64, report_root='e'*64, slot=0, attempt=0,
                     actor=actor, counts=[4, 9, 1, 7, 2], target_merit_q16=27*65536,
                     ranges=ranges, answer=answer, commit_ready=True)
        common = dict(args=args, rpc=self.f, network_lock=self.f.lock, address=lambda:'owner')
        progress = root / 'work-cycles' / 'retained-audit-progress'
        old = NetworkAudits(SimpleNamespace(root=former_role, retained_audit_progress=progress, **common),
                            on_retired=lambda *_:None)
        journal = old.folder / 'current.json'
        save(journal, audit)
        original = journal.read_bytes()
        self.f.jobs = {}
        self.f.omit_audit_projection = True
        with self.assertRaisesRegex(PublicationPending, 'view incomplete'):
            old.process_one()
        self.assertEqual(journal.read_bytes(), original)
        self.f.omit_audit_projection = False
        self.f.jobs = {0:dict(attempt=1, state='assigned')}
        self.assertEqual(old.process_one(), 'retired')
        name = f'{REQUEST}-0-0.json'
        sealed = old.folder / 'sealed-answers' / name
        sealed_bytes = sealed.read_bytes()
        self.assertFalse(journal.exists())
        (old.folder / name).unlink()

        self.f.jobs = {0:dict(state='assigned', actor=actor, expired=False,
                              actor_policy_accepted=True, ranges=ranges)}
        call = self.f.call
        def recovery_rpc(method, params=None, **kwargs):
            if method == 'validateaddress':
                return dict(isvalid=True, scriptPubKey=actor)
            if method == 'getpgnworkview' and len(params) == 3:
                return dict(schema='PGN_WORK_VIEW_V1', kind='audits', offset=0, limit=100,
                    state=dict(bestblockhash=PARENT, height=5, genesis=args.genesis),
                    reports=[dict(request_id=REQUEST, ticket='d'*64, report_root='e'*64)],
                    audits=[dict(request_id=REQUEST, **self.f.state(REQUEST))])
            return call(method, params, **kwargs)
        self.f.call = recovery_rpc
        recovery = AssignedAuditRecovery(SimpleNamespace(root=root, **common))
        source = SimpleNamespace(run_ticket=lambda *args: self.fail('An original seal must not recompute'))
        new = NetworkAudits(SimpleNamespace(root=current_role, retained_audit_progress=progress, source=source, **common),
                            before_commit=lambda current:False)
        recovered, complete = recovery.claim(new.folder, verify_signer=True)
        self.assertTrue(complete)
        self.assertEqual(recovered['answer'], answer)
        changed = dict(recovered, ranges=[dict(part, start=str(int(part['start'])+1)) for part in ranges], counts=[])
        changed.pop('answer')
        self.assertNotIn('answer', new.restore_retired_progress(changed))
        self.assertEqual(new.process_one(), 'paused')
        self.assertEqual(json.loads((new.folder/'current.json').read_bytes())['answer'], answer)
        self.assertEqual(sealed.read_bytes(), sealed_bytes)
        self.assertEqual(self.f.sent, [])

    def test_stale_cursor_with_distinct_nonce_releases_the_slot_and_keeps_both_copies(self):
        from horizon_assigned_audit_recovery import AssignedAuditRecovery
        from horizon_audit_recovery import RetiredAuditSealConflict, progress_path, retain_progress
        from report_commitment import audit_commitment

        root = self.f.root / 'retirement-conflict'
        genesis = '7' * 64
        progress = root / 'work-cycles' / 'retained-audit-progress'
        retired = []
        source = SimpleNamespace(run_ticket=lambda *args: self.fail('A stale job must not recompute'))
        worker = SimpleNamespace(root=root / 'audit-lanes' / '1', args=SimpleNamespace(genesis=genesis),
                                 rpc=self.f, network_lock=self.f.lock, retained_audit_progress=progress,
                                 source=source, running=threading.Event())
        worker.root.mkdir(parents=True)
        worker.running.set()
        lane = NetworkAudits(worker, on_retired=lambda current, anchor: retired.append((current, anchor)))
        ranges = [dict(index=i, start=str(i * 10 + 10), end=str(i * 10 + 20)) for i in range(5)]
        counts = [4, 9, 1, 7, 2]
        job = dict(request_id=REQUEST, ticket='d' * 64, report_root='e' * 64, slot=0, attempt=0,
                   actor='0014' + '1' * 40, counts=counts, target_merit_q16=27 * 65536, ranges=ranges)

        def seal(nonce):
            return dict(job, answer=audit_commitment(job['ticket'], job['attempt'], list(range(5)),
                                                    list(map(str, counts)), nonce * 64))

        older, current = seal('8'), seal('9')
        retain_progress(progress, genesis, older)
        checkpoint = progress_path(progress, genesis, older)[0]
        before = checkpoint.read_bytes()
        path = lane.folder / 'current.json'
        save(path, current)
        active = path.read_bytes()
        self.f.jobs = {0: dict(state='assigned', attempt=1)}
        self.f.omit_audit_projection = True
        with self.assertRaises(PublicationPending):
            lane.process_one()
        self.assertEqual(path.read_bytes(), active)
        self.assertEqual(retired, [])
        self.f.omit_audit_projection = False
        self.assertEqual(lane.process_one(), 'retired')
        self.assertEqual(retired, [(current, PARENT)])
        self.assertFalse(path.exists())
        self.assertEqual(checkpoint.read_bytes(), before)
        self.assertEqual(next((lane.folder / 'retained-assignments').glob('*.json')).read_bytes(), active)
        with self.assertRaises(RetiredAuditSealConflict):
            lane.restore_retired_progress(current)

        following = 'f' * 64
        assigned = dict(attempt=0, state='assigned', actor=job['actor'], expired=False,
                        actor_policy_accepted=True, ranges=ranges)
        self.f.jobs = {0: assigned}
        self.f.additional_jobs = {0: assigned}
        fallback = self.f.call
        def recovery_rpc(method, params=None, **kwargs):
            if method == 'validateaddress':
                return dict(isvalid=True, scriptPubKey=job['actor'])
            if method == 'getpgnworkview' and len(params) == 3:
                return dict(schema='PGN_WORK_VIEW_V1', kind='audits', offset=0, limit=100,
                            state=dict(bestblockhash=PARENT, height=5, genesis=genesis),
                            reports=[dict(request_id=request, ticket=job['ticket'], report_root=job['report_root'])
                                     for request in (REQUEST, following)],
                            audits=[dict(request_id=request, **self.f.state(request))
                                    for request in (REQUEST, following)])
            return fallback(method, params, **kwargs)

        self.f.call = recovery_rpc
        recovery = AssignedAuditRecovery(SimpleNamespace(root=root, args=worker.args, rpc=self.f,
                                                          network_lock=self.f.lock, address=lambda: 'owner'))
        adopted, complete = recovery.claim(lane.folder)
        self.assertTrue(complete)
        self.assertEqual(adopted['request_id'], following)
        self.assertEqual(adopted['counts'], [])
        self.assertEqual(self.f.sent, [])

    def test_complete_absence_at_a_later_tip_can_release_but_repeated_same_tip_cannot(self):
        from horizon_audit_recovery import assignment_absence_confirmed
        prior, current = '3'*64, '4'*64
        audit = dict(assigned_audit_anchor=prior)
        calls = []
        def get_header(method, params):
            calls.append((method,params))
            return dict(hash=prior,confirmations=-1)
        rpc = SimpleNamespace(call=get_header)
        self.assertFalse(assignment_absence_confirmed(audit, dict(_report_binding_observed=True), prior, rpc))
        self.assertFalse(assignment_absence_confirmed(audit, {}, current, rpc))
        self.assertTrue(assignment_absence_confirmed(audit, dict(_report_binding_observed=True), current, rpc))
        self.assertTrue(assignment_absence_confirmed(audit, dict(_report_absent=True), current, rpc))
        self.assertEqual(calls,[('getblockheader',[prior,True])])

    def test_legacy_absence_needs_an_exact_report_confirmation(self):
        from horizon_audit_recovery import assignment_absence_confirmed
        legacy = dict(request_id=REQUEST)
        for report, tip, expected in (([], PARENT, True), ([dict(request_id=REQUEST)], PARENT, False),
                                      ([], 'b'*64, False)):
            with self.subTest(report=report, tip=tip):
                def respond(method, params=None):
                    if method == 'getpgnreports':
                        self.assertEqual(params, [0, 1, REQUEST])
                        return report
                    self.assertEqual(method, 'getbestblockhash')
                    return tip
                self.assertEqual(assignment_absence_confirmed(
                    legacy, dict(_report_absent=True), PARENT, SimpleNamespace(call=respond)), expected)

    def test_complete_report_replacement_releases_only_the_obsolete_cursor(self):
        from report_commitment import audit_commitment
        for sealed in (False, True):
            with self.subTest(sealed=sealed):
                root = self.f.root / f'replacement-{sealed}'
                root.mkdir()
                self.f.tip = PARENT
                self.f.more_work = False
                self.f.sent.clear()
                self.f.additional_jobs.clear()
                self.f.report_root = 'a'*64
                ranges = [dict(index=i, start=str(i*10+10), end=str(i*10+20)) for i in range(5)]
                actor = '0014'+'1'*40
                self.f.jobs[0] = dict(state='assigned', actor=actor, expired=False, ranges=ranges,
                                      actor_policy_accepted=True)
                retired=[]
                source=SimpleNamespace(run_ticket=lambda *args:self.fail('Replaced work must not compute'))
                worker=SimpleNamespace(root=root,rpc=self.f,args=SimpleNamespace(genesis='7'*64),
                    network_lock=self.f.lock,source=source,running=threading.Event(),
                    audit_group_journal=root/'group.json',quitting=threading.Event(),
                    ledger=self.f.ledger,address=lambda:'owner')
                publisher=NetworkAudits(worker,on_retired=lambda item,block:retired.append((item,block)))
                audit=dict(request_id=REQUEST,ticket='d'*64,report_root='e'*64,slot=0,attempt=0,
                    actor=actor,counts=[3]*5 if sealed else [],target_merit_q16=27*65536,ranges=ranges)
                if sealed:
                    audit.update(commit_ready=True,
                        answer=audit_commitment(audit['ticket'],0,list(range(5)),['3']*5,'8'*64))
                journal=publisher.folder/'current.json'
                save(journal,audit)
                original=journal.read_bytes()
                self.assertEqual(publisher.process_one(),'retired')
                self.assertEqual(retired,[(audit,PARENT)])
                self.assertFalse(journal.exists())
                self.assertEqual(next((publisher.folder/'retained-assignments').glob('*.json')).read_bytes(),original)
                if sealed:
                    self.assertEqual(publisher.answers.recover_seal(f'{REQUEST}-0-0',audit)['answer'],audit['answer'])
                self.assertEqual(self.f.sent,[])
                self.f.more_work=True
                worker.audit_publishers=(publisher,)
                publisher.event=lambda action,request,slot,attempt,body,preceding_events=None: self.f.event(
                    0,action,request,slot,attempt,body,preceding_events)
                self.assertEqual(publisher.process_one(),'paused')
                self.assertEqual(json.loads(journal.read_bytes())['request_id'],'f'*64)
                self.assertEqual(len(self.f.sent),1)
                self.assertEqual([json.loads(bytes.fromhex(envelope))['action']
                                  for envelope in self.f.sent[0]],['assign'])

    def test_already_committed_audit_retires_during_the_same_handoff(self):
        from horizon_mining_flow import MiningFlow
        from horizon_ticket_prefetch import TicketPrefetch
        from report_commitment import audit_commitment

        actor = '0014' + '1' * 40
        ranges = [dict(index=i, start=str(i*10+10), end=str(i*10+20)) for i in range(5)]
        local = audit_commitment('d'*64, 0, list(range(5)), ['7']*5, '3'*64)
        other = audit_commitment('d'*64, 0, list(range(5)), ['7']*5, '5'*64)
        self.f.jobs[0] = dict(state='assigned', actor=actor, expired=False,
                              ranges=ranges, actor_policy_accepted=True)
        worker = self.f.peers[0].worker
        worker.root = self.f.peers[0].folder.parent
        worker.args = SimpleNamespace(genesis='7'*64)
        worker.audit_prefix_supported = True
        worker.source = SimpleNamespace(run_ticket=lambda *args: self.fail('A sealed audit must not recalculate'))
        worker.running = threading.Event()
        worker.running.set()
        retired, acknowledged = [], []
        released, wakeups = [], []
        flow = object.__new__(MiningFlow)
        flow.worker = worker
        flow.gpu = SimpleNamespace(retire_audit=lambda *binding: released.append(binding),
                                   wake=lambda: wakeups.append(True))
        flow.cycles = SimpleNamespace(bound_audit=lambda *binding: None)
        flow.unbound_service_lock = threading.RLock()
        flow.unbound_service_audits = {(REQUEST, 0, 0)}
        flow.unbound_service_completed = set()
        flow.prefetch = TicketPrefetch(None, worker.running, worker.quitting, worker.network_lock)
        flow.prefetch.publish_service_observation(0, dict(enforced=True, may_request=False))

        def interleaved_commit(_):
            self.f.jobs[0].update(state='committed', commitment=other['commitment'])
            self.f.tip = 'b'*64
            return True

        def retire(current, tip):
            flow.retire_audit_cycle(0, current, tip)
            retired.append((current['answer'], tip))

        publisher = NetworkAudits(
            worker, before_commit=interleaved_commit, on_retired=retire,
            on_committed=lambda audit, tip: acknowledged.append(tip),
        )
        worker.audit_publishers = (publisher,)
        audit = dict(request_id=REQUEST, ticket='d'*64, report_root='e'*64, slot=0,
                     attempt=0, actor=actor, ranges=ranges, counts=[7]*5,
                     target_merit_q16=27*65536, answer=local)
        cursor = publisher.folder / 'current.json'
        save(cursor, audit)
        self.assertEqual(publisher.process_one(), 'retired')
        self.assertFalse(cursor.exists())
        self.assertEqual(retired, [(local, 'b'*64)])
        self.assertEqual(released, [(0, REQUEST, 0, 0)])
        self.assertEqual(wakeups, [True])
        self.assertIsNone(flow.prefetch.service_observation())
        self.assertEqual(flow.prefetch.native_service_generation, 1)
        self.assertEqual(acknowledged, [])
        self.assertEqual(self.f.sent, [])
        original = next((publisher.folder/'retained-assignments').glob('*.json'))
        self.assertEqual(json.loads(original.read_bytes())['answer'], local)
        self.assertEqual(self.f.jobs[0]['commitment'], other['commitment'])

    def test_legacy_seal_with_unrecorded_actor_cannot_enter_the_active_cursor(self):
        from horizon_assigned_audit_recovery import AssignedAuditRecovery
        from horizon_audit_answers import AuditSealBindingMismatch
        from report_commitment import audit_commitment
        root = self.f.root / 'legacy-actor'
        path = root / 'audit-lanes' / '0' / 'audits' / 'current.json'
        path.parent.mkdir(parents=True)
        ranges = [dict(index=i,start=str(i*10+10),end=str(i*10+20)) for i in range(5)]
        old = dict(request_id=REQUEST,ticket='d'*64,report_root='e'*64,slot=0,attempt=0,
                   ranges=ranges,counts=[4]*5,target_merit_q16=27*65536,
                   answer=audit_commitment('d'*64,0,list(range(5)),['4']*5,'8'*64))
        save(path, old)
        raw = path.read_bytes()
        genesis = '7'*64
        actor = '0014'+'1'*40
        following = 'f'*64
        job = dict(slot=0,attempt=0,state='assigned',actor=actor,expired=False,actor_policy_accepted=True,ranges=ranges)
        def rpc(method, params=None):
            if method == 'validateaddress':
                return dict(isvalid=True,scriptPubKey=actor)
            if method == 'getbestblockhash':
                return PARENT
            if method == 'getpgnauditactivity':
                raise CoreRpcError(-32601,'Method not found')
            self.assertEqual(method,'getpgnworkview')
            return dict(schema='PGN_WORK_VIEW_V1',kind='audits',offset=0,limit=100,
                state=dict(bestblockhash=PARENT,height=5,genesis=genesis),
                reports=[dict(request_id=value,ticket='d'*64,report_root='e'*64)
                         for value in (REQUEST,following)],
                audits=[dict(request_id=value,jobs=[job]) for value in (REQUEST,following)])
        worker = SimpleNamespace(root=root,args=SimpleNamespace(genesis=genesis),
            address=lambda:'owner',rpc=SimpleNamespace(call=rpc),network_lock=self.f.lock)
        recovery = AssignedAuditRecovery(worker)
        candidate = dict(old,actor=actor,counts=[])
        candidate.pop('answer')
        with self.assertRaisesRegex(AuditSealBindingMismatch,'original signer'):
            recovery.retain_legacy_answer(candidate)
        active = root / 'service-audit-lanes' / '0' / 'audits'
        active.mkdir(parents=True)
        recovered,complete = recovery.claim(active)
        self.assertTrue(complete)
        self.assertEqual(recovered['request_id'],following)
        self.assertEqual(recovered['counts'],[])
        self.assertNotIn('answer',recovered)
        self.assertEqual(path.read_bytes(),raw)
        self.assertNotIn('answer',candidate)

    def test_stale_owner_disclosure_row_cannot_preempt_the_current_generation(self):
        from hashlib import sha256
        from horizon_audit_batch import disclose_local_ready
        from report_commitment import audit_commitment, canonical_bytes
        ticket = 'd'*64
        owner_body = {'report':'saved owner report'}
        report_root = sha256(canonical_bytes(owner_body)).hexdigest()
        report = self.f.root / 'tickets' / ticket / 'network-report.json'
        report.parent.mkdir(parents=True)
        save(report,dict(report_root=report_root,body=owner_body))
        actor_a,actor_b = '0014'+'1'*40,'0014'+'2'*40
        common = dict(request_id=REQUEST,ticket=ticket,report_root=report_root,slot=0,attempt=0)
        a = dict(common,actor=actor_a,answer=audit_commitment(ticket,0,list(range(5)),['4']*5,'6'*64))
        b = dict(common,actor=actor_b,answer=audit_commitment(ticket,0,list(range(5)),['4']*5,'8'*64))
        state = dict(jobs=[
            dict(slot=0,attempt=0,state='committed',actor=actor_b,commitment=b['answer']['commitment']),
            dict(slot=1,attempt=0,state='committed'),
        ])
        worker=SimpleNamespace(root=self.f.root,rpc=SimpleNamespace(call=lambda method,params:[dict(ticket=ticket,report_root=report_root)]))
        owner=SimpleNamespace(worker=worker,state=lambda request:state)
        lane=SimpleNamespace(state=lambda request:state)
        with patch('horizon_audit_batch.apply_group') as submit:
            self.assertEqual(disclose_local_ready(owner,[(lane,'old',a),(lane,'current',b)]),([],[]))
            operations=submit.call_args.args[0]
            self.assertEqual([operation[1] for operation in operations],['reveal_report','reveal'])
            self.assertEqual(operations[-1][-1],canonical_bytes(b['answer']['body']).hex())
            submit.reset_mock()
            self.assertEqual(disclose_local_ready(owner,[(lane,'old',a)]),([],[]))
            submit.assert_not_called()

def run():
    import io, socket, subprocess
    attempted = dict(network=0, children=0)
    def no_network(*args, **kwargs):
        attempted['network'] += 1
        raise RuntimeError('Network forbidden during audit grouping self-test')
    def no_child(*args, **kwargs):
        attempted['children'] += 1
        raise RuntimeError('Child process forbidden during audit grouping self-test')
    output = io.StringIO()
    with patch.object(socket, 'socket', side_effect=no_network), patch.object(subprocess, 'Popen', side_effect=no_child):
        result = unittest.TextTestRunner(stream=output, verbosity=0).run(unittest.defaultTestLoader.loadTestsFromTestCase(BatchTests))
    passed = result.wasSuccessful() and not any(attempted.values())
    return dict(schema='PGN_HORIZON_AUDIT_BATCH_SELFTEST_V1', passed=passed, checks=result.testsRun,
        no_network=attempted['network']==0, no_child_processes=attempted['children']==0,
        cpu_activated=False, scientific_qualification=False,
        details=output.getvalue()[-2000:] if not passed else 'Audit grouping, recovery and rejection checks passed')

