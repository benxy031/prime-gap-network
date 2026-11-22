"""Crash, concurrency and parent-conflict tests for ready audit publication."""
from pathlib import Path
from types import SimpleNamespace
import copy, json, sys, tempfile, threading, unittest
from unittest.mock import patch
from horizon_audit_batch import apply_group,reserve_assignments,commit_ready,save,LEGACY_AUDIT_USAGE,retain_assignments
from horizon_coordination import native_header_hash
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
        if method=='getpgnreports':return [dict(ticket='d'*64,report_root='e'*64)]
        if method=='getpgnauditstate':return [self.state(params[2])]
        if method=='getpgnworkview':
            request=params[3]
            return dict(schema='PGN_WORK_VIEW_V1',kind='audits',request_id=request,
                        state=dict(bestblockhash=self.tip,genesis='7'*64),
                        reports=[dict(request_id=request,ticket='d'*64,report_root='e'*64)],
                        audits=[dict(request_id=request,**self.state(request))])
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
    def test_watchonly_signer_cannot_reenter_legacy_assignment_recovery(self):
        from horizon_network_audits import NetworkAudits
        root=self.f.root/'watchonly-caller';root.mkdir();recovered=[];self.f.watchonly=True
        def recover(folder):recovered.append(folder);return None,True
        worker=SimpleNamespace(root=root,rpc=self.f,network_lock=self.f.lock,args=SimpleNamespace(),
            source=SimpleNamespace(),audit_prefix_supported=True,recover_assigned_audit=recover)
        publisher=NetworkAudits(worker)
        publisher.apply=lambda *args:dict(available=False,unavailable_reason='no_eligible_independent_work',actor='0014'+'1'*40)
        self.assertEqual(publisher.process_one(),'none')
        self.assertEqual(recovered,[])
        self.assertEqual(publisher.last_unavailable['unavailable_reason'],'signing_key_unavailable')

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

