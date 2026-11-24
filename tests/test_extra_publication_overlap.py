"""Bounded real-control-path witness; fake Core/engine, no native/GPU safety claim."""
from pathlib import Path
from types import SimpleNamespace
from concurrent.futures import CancelledError
import copy, hashlib, json, os, sys, tempfile, threading, time, unittest
from unittest.mock import patch
HERE=Path(__file__).resolve().parent
DEFAULT_BASE=HERE.parent if(HERE.parent/'src').is_dir()else HERE.parent/'gpu112-dejan-private'
BASE=Path(os.environ.get('PGN_OVERLAP_SOURCE',str(DEFAULT_BASE)))
BASELINE_FLOW=os.environ.get('PGN_OVERLAP_BASELINE_FLOW')
sys.dont_write_bytecode=True
sys.path[:0]=[str(HERE),str(BASE/'src/engine/runtime_host'),str(BASE/'src/protocol'),str(BASE/'src/engine/cpu_auditor/shared_service'),str(BASE/'tests')]
from horizon_mining_flow import MiningFlow,save
from horizon_gpu_queue import ResidentGpuQueue
from horizon_network_audits import NetworkAudits
from horizon_cycle_execution import CycleExecution
from horizon_work_cycle import WorkCycleJournal
from horizon_ticket_prefetch import TicketPrefetch
from horizon_report_outbox import ReportOutbox
from horizon_preseal_queue import PresealQueue
from horizon_audit_batch_selftest import Fixture,REQUEST
from horizon_native_work_duty import context
from horizon_service_presentation import audit_view_ref
from horizon_cpu_cycle import MixedNormalSource,cycle_request
from report_commitment import canonical_bytes,audit_commitment
from pgn_native_contract import fields
from horizon_core_rpc import CoreRpcError
from horizon_network_worker import NetworkWorker
from horizon_extra_publication_overlap import candidate,verify_start,finish,ticket_status,_extra_gate,_repair
import test_ticket_archive as archive_fixture_module
import horizon_ticket_archive as archive


def wait_for(check,timeout=4):
    stop=time.monotonic()+timeout
    while time.monotonic()<stop:
        if check():return
        time.sleep(.01)
    raise AssertionError('Bounded condition did not arrive')

class Harness:
    def __init__(self,test,root=None,chain=None,archive_fixture=None):
        self.test=test;self.tmp=tempfile.TemporaryDirectory()if root is None else None
        if self.tmp:test.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)if root is None else root
        self.a=archive_fixture or archive_fixture_module.TicketArchiveTests('test_compact_and_exact_result')
        if archive_fixture is None:self.a.setUp();test.addCleanup(self.a.tearDown)
        if root is None:self.root=Path(self.a.tmp.name)
        self.root.mkdir(parents=True,exist_ok=True)
        self.w=dict(genesis='7'*64,request_id='9'*64,ticket=self.a.folder.name,ticket_number='1',
            start=self.a.cfg['start'],length=str(int(self.a.cfg['end'])-int(self.a.cfg['start'])),
            target_numerator='32',target_denominator='1',miner_script='0014'+'1'*40,reward_script='0014'+'1'*40,parent='a'*64,segments=100,work_id='3'*64)
        self.receipt=dict(schema='PGN_NATIVE_ASSIGNED_AUDIT_SERVICE_V1',enforced=True,request_id=self.w['request_id'],issuance_block='e'*64,issuance_height=16,
            credits=[dict(request_id=f'{100+i:064x}',slot=i,attempt=0,commitment=f'{200+i:064x}')for i in range(2)])
        self.w['native_audit_service']=dict(self.receipt,view_parent='a'*64)
        self.chain=chain or Fixture(self.root/'chain');self.calls=[];self.expired=False;self.absent=False;self.changed=False;self.uncertain=False
        self.cpu_calls=[];self.compute_calls=0;self.started=threading.Event();self.computed=threading.Event();self.release_compute=threading.Event();self.release_compute.set()
        self.blocked=threading.Event();self.release_publish=threading.Event();self.publish_errors=[];self.errors=[];self.times={}
        self.worker=SimpleNamespace(root=self.root,args=SimpleNamespace(genesis=self.w['genesis'],piece_limit=10**12),sequence=0,device_lease=None,current=None,cpu_only=False,
            running=threading.Event(),quitting=threading.Event(),state_lock=threading.RLock(),network_lock=self.chain.lock,audits=SimpleNamespace(presentation=None))
        self.worker.running.set();self.worker.rpc=self;self.worker.acquire_device=lambda:setattr(self.worker,'device_lease','fixture');self.worker.release_device=lambda:None
        self.worker.message=lambda *args:None;self.worker.progress=lambda *args:None
        host=self
        class Raw:
            def require_report_finalization(inner):host.a.cfg['preseal_required']=True
            def finalize_report(inner,work,raw):return raw
            def run_ticket(inner,ticket,limit):
                host.started.set();host.times.setdefault('gpu_start',time.monotonic_ns())
                host.release_compute.wait(3)
                if not (host.a.folder/'computed-once').exists():host.compute_calls+=1;(host.a.folder/'computed-once').write_text('exact fixture completed')
                host.a.meta['state']='COMPLETE';host.a.original=archive.canonical(host.a.meta)
                (host.a.folder/'session.json').write_bytes(host.a.original);(host.a.folder/'report.json').write_bytes(archive.canonical(host.a.report))
                host.computed.set();return copy.deepcopy(host.a.report)
            def run_scheduled_ticket(inner,ticket,limit,should_yield):return inner.run_ticket(ticket,limit)
        class Cpu:
            def call(inner,action,**kw):host.cpu_calls.append((action,kw));raise AssertionError('CPU execution before production grant')
        self.raw=Raw();self.source=MixedNormalSource(self.raw,Cpu(),self.root/'cpu');self.source.enabled=True
        self.worker.source=self.source
        self.flow=MiningFlow.__new__(MiningFlow);f=self.flow;f.worker=self.worker;f.closed=False;f.finalizer=self.source.finalize_report;self.source.require_report_finalization()
        self.a.cfg['threshold_policy']=fields(int(self.w['start']),int(self.a.cfg['end']))['threshold_policy']
        self.a.report['body']['policy_sha256']=self.a.cfg['threshold_policy']['sha256'];self.a.report['sha256']=hashlib.sha256(archive.canonical(self.a.report['body'])).hexdigest()
        self.a.original=archive.canonical(self.a.meta);(self.a.folder/'session.json').write_bytes(self.a.original);(self.a.folder/'report.json').write_bytes(archive.canonical(self.a.report))
        f.cycles=WorkCycleJournal(self.root/'work-cycles',self.w['genesis']);test.addCleanup(f.cycles.close)
        f.cycles.prepare(self.w,native_service=self.receipt);f.physical_audits=CycleExecution(self.root/'work-cycles',self.w['genesis'])
        p=f.physical_audits;p.mark_native(self.w['request_id'],[]);p.prepare(self.w['request_id'],[self.w['request_id']])
        for i,ref in enumerate(self.receipt['credits']):
            task=dict(request_id=ref['request_id'],slot=i,attempt=0);p.bind(self.w['request_id'],i,task);p.committed(self.w['request_id'],i,task,'e'*64)
        f.outbox=ReportOutbox(self.root);f.preseal=PresealQueue(self.root);f.recovered_report=None;f.changed=threading.Event();f.prior_active={};f.view_sequence=0
        f.unbound_service_lock=threading.RLock();f.unbound_service_audits=set();f.unbound_service_completed=set();f.extra_audit_view_refs={};f.cycle_audit_positions={}
        f.allocator=NetworkWorker.__new__(NetworkWorker)
        f.allocator.root=self.root/'prepared-ticket';f.allocator.root.mkdir(exist_ok=True)
        for key in('args','rpc','sequence','quitting','audits','state_lock','network_lock'):setattr(f.allocator,key,getattr(self.worker,key))
        f.allocator.message=lambda *args:None
        f.prefetch=TicketPrefetch(f.allocator,self.worker.running,self.worker.quitting,self.worker.network_lock)
        f.prefetch.ready=copy.deepcopy(self.w);save(f.allocator.root/'controller.json',dict(stage='ASSIGNED',sequence=0,work=self.w))
        f.gpu=ResidentGpuQueue(self.source,self.worker.running,self.worker.quitting,f.activate,f.completed,lambda:None,self.errors.append,
            may_start=f.may_start_work,check_start=f.confirm_gpu_start)
        from horizon_reassignment_queue import ReassignmentQueue
        f.reassignments=ReassignmentQueue(f)
        first=self.chain.peers[0];first.worker.root=first.folder.parent
        self.lane=NetworkAudits(first.worker,on_committed=lambda audit,block:f.audit_committed(0,audit,block),
            on_retired=lambda audit,anchor:f.retire_audit_cycle(0,audit,anchor))
        self.lane.event=first.event;f.audit_lanes=[self.lane]
        first.worker.audit_publishers=(self.lane,);first.worker.audit_group_journal=self.root/'group.json'
        self.ranges=[dict(index=i,start=str(160000000000000000000+i*10),end=str(160000000000000000000+(i+1)*10))for i in range(5)]
        self.audit=dict(request_id=REQUEST,slot=0,attempt=0,ticket='d'*64,report_root=self.chain.report_root,actor='0014'+'1'*40,ranges=self.ranges,counts=[1,2,3,4,5],commit_ready=True)
        self.audit['answer']=audit_commitment(self.audit['ticket'],0,list(range(5)),['1','2','3','4','5'],'6'*64)
        self.chain.jobs[0]=dict(state='assigned',actor=self.audit['actor'],expired=False,actor_policy_accepted=True,ranges=self.ranges)
        self.seal(self.audit);self.extra=audit_view_ref(REQUEST,0,0)['audit_id'];f.cycles.reserve_extra_audit(self.w['request_id'],self.extra)
        self.native_before=copy.deepcopy(f.cycles.snapshot(self.w['request_id'])['native_service'])
        self.future=f.gpu.submit(f.job(self.w),dict(role='search',work=copy.deepcopy(self.w),prepared=True,work_cycle=self.w['request_id']),10**12)
        self.job=f.gpu.jobs[self.w['ticket']]
        self.thread=None;self.publisher=None
        test.addCleanup(self.stop)
    def seal(self,audit):
        save(self.lane.folder/'current.json',audit);name=f"{audit['request_id']}-{audit['slot']}-{audit['attempt']}"
        save(self.lane.folder/(name+'.json'),audit);self.lane.answers.register([name]);self.lane.current_audit=copy.deepcopy(audit)
    def call(self,method,args=None,**kw):
        self.calls.append((method,args));args=args or []
        if method=='getpgnscientificwork'and args[0]==self.w['request_id']:
            if self.absent:raise CoreRpcError(-8,'Request has no current authenticated automatic ticket')
            work=dict(self.w,schema='PGN_NATIVE_SCIENTIFIC_WORK1',accepted=False,parent=self.chain.tip,height=101)
            work['native_audit_service']=dict(self.receipt,view_parent=self.chain.tip)
            if self.changed:work.update(ticket='8'*64,start=str(int(self.w['start'])+int(self.w['length'])),ticket_number='2')
            return work
        if method=='getpgnworkview'and args[0]=='tickets':
            lease=dict(request=self.w['request_id'],ticket_number='1',state='assigned',issued=20,deadline=99 if self.expired else 500,
                start=self.w['start'],end=str(int(self.w['start'])+int(self.w['length'])),miner_script=self.w['miner_script'],reward_script=self.w['reward_script'])
            if self.changed:lease.update(start=str(int(self.w['start'])+int(self.w['length'])),end=str(int(self.w['start'])+2*int(self.w['length'])),ticket_number='2')
            return dict(schema='PGN_WORK_VIEW_V1',kind='tickets',request_id=self.w['request_id'],offset=0,limit=1,reports=[],
                state=dict(genesis=self.w['genesis'],bestblockhash=self.chain.tip,height=100,lease_offset=0,leases=[]if self.absent else[lease]))
        if method=='getpgnstate':return self.call('getpgnworkview',['tickets',0,1,self.w['request_id']])['state']
        if method=='getblockheader'and args[0]=='a'*64:return dict(hash=args[0],height=90,confirmations=-1 if self.changed else 10)
        if method=='getblockhash'and args[0]==90:return '4'*64 if self.changed else 'a'*64
        if method=='getblockheader'and args[0]=='e'*64:return dict(hash=args[0],height=16,confirmations=50)
        if method=='getblockhash'and args[0]==16:return 'e'*64
        return self.chain.call(method,args,**kw)
    def publish(self,reject=False):
        original=self.chain.ledger
        def blocked(envelopes,**kw):
            self.blocked.set()
            if not self.release_publish.wait(4):raise TimeoutError('fixture publication release absent')
            if reject:raise CoreRpcError(-25,'injected rejection')
            result=original(envelopes,**kw);self.times['publication_confirmed']=time.monotonic_ns();return result
        self.lane.worker.ledger=blocked
        def run():
            try:self.lane.process_one()
            except BaseException as e:self.publish_errors.append(e)
        self.publisher=threading.Thread(target=run);self.publisher.start();wait_for(self.blocked.is_set)
    def start(self):
        self.thread=threading.Thread(target=self.flow.gpu.run);self.thread.start()
    def stop(self):
        self.release_compute.set();self.release_publish.set();self.worker.quitting.set();self.flow.gpu.wake()
        self.flow.prefetch.close(1)
        for t in(self.thread,self.publisher):
            if t:t.join(3);self.test.assertFalse(t.is_alive())
    def assert_blocked(self):
        t=self.test;f=self.flow;t.assertEqual(f.cycles.snapshot(self.w['request_id'])['stage'],'prepared');t.assertIsNotNone(f.prefetch.ready)
        t.assertEqual(f.preseal.entries(),[]);t.assertIsNone(f.outbox.first());t.assertIsNone(self.worker.current)
        t.assertEqual(f.cycles.snapshot(self.w['request_id'])['native_service'],self.native_before);t.assertEqual(self.cpu_calls,[]);t.assertIsNone(self.source.grant)
        t.assertFalse(f.physical_audits.native[self.w['request_id']].get('empty'));t.assertEqual(self.a.report.get('network_accepted'),False)
        t.assertFalse(self.a.compact()['compacted']);self.a.intact()
    def assert_promoted(self):
        t=self.test;wait_for(self.future.done);self.future.result();t.assertEqual(self.errors,[]);t.assertEqual(self.publish_errors,[])
        t.assertEqual(self.flow.cycles.snapshot(self.w['request_id'])['stage'],'completed');t.assertIsNone(self.flow.prefetch.ready)
        t.assertEqual(self.source.grant['stage'],'search');t.assertEqual(self.cpu_calls,[]);t.assertEqual(self.compute_calls,1)
        t.assertIsNone(self.flow.outbox.first());t.assertEqual(len(self.flow.preseal.entries()),1)
        t.assertEqual(self.flow.cycles.snapshot(self.w['request_id'])['native_service'],self.native_before)

class OverlapTests(unittest.TestCase):
    def test_exact_committed_and_authenticated_waiver_shape_can_overlap(self):
        h=Harness(self);p=h.flow.physical_audits
        def waived(records):records[h.w['request_id']][1]=dict(stage='unavailable',anchor=h.chain.tip,reason='no_eligible_independent_work')
        p.update(waived);h.publish();h.start();wait_for(h.computed.is_set);h.assert_blocked()
        h.release_publish.set();h.assert_promoted()
    def test_regular_reorg_retains_prepared_replacement_and_blocks_report(self):
        h=Harness(self);h.release_compute.clear();h.start();wait_for(h.started.is_set)
        ref=h.receipt['credits'][0];audit=dict(request_id=ref['request_id'],slot=0,attempt=0,physical_cycle=h.w['request_id'])
        h.flow.reopen_audit_cycle(0,audit,'e'*64);h.release_compute.set();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None)
        h.flow.audit_committed(0,h.audit,'e'*64);h.assert_blocked()
        self.assertEqual(h.flow.physical_audits.snapshot(h.w['request_id'])[0]['stage'],'assigned')
        h.flow.retire_audit_cycle(0,audit,h.chain.tip)
        self.assertEqual(h.flow.physical_audits.pending(0,h.flow.local_audit_cycles()),h.w['request_id'])
        replacement=dict(request_id='4'*64,slot=0,attempt=0,physical_cycle=h.w['request_id'])
        h.flow.physical_audits.bind(h.w['request_id'],0,replacement);h.flow.audit_committed(0,replacement,'e'*64);h.assert_promoted()
    def test_orphaned_regular_waiver_cannot_authorize_speculation(self):
        h=Harness(self);p=h.flow.physical_audits;block='b'*64;h.chain.intents[block]={}
        p.update(lambda rows:rows[h.w['request_id']].__setitem__(1,dict(stage='unavailable',anchor=block,reason='no_eligible_independent_work')))
        self.assertTrue(h.flow.may_start_work(h.job));self.assertFalse(verify_start(h.flow,h.job));self.assertNotIn('extra_publication_overlap',h.job.context)
    def test_start_rejects_ticket_anchor_a_then_regular_or_extra_view_b(self):
        import horizon_extra_publication_overlap as overlap
        for phase in('regular','extra'):
            with self.subTest(phase=phase):
                h=Harness(self)
                if phase=='regular':
                    original=overlap.ticket_status
                    def moved(flow,work):
                        status=original(flow,work);h.chain.tip='b'*64;return status
                    with patch.object(overlap,'ticket_status',side_effect=moved):self.assertFalse(verify_start(h.flow,h.job))
                else:
                    original=overlap.ready_audit_view
                    def moved(lane,audit,anchor):h.chain.tip='b'*64;return original(lane,audit,anchor)
                    with patch.object(overlap,'ready_audit_view',side_effect=moved):
                        self.assertFalse(h.flow.confirm_gpu_start(h.job))
                self.assertNotIn('extra_publication_overlap',h.job.context);self.assertEqual(h.flow.preseal.entries(),[])
    def test_start_requires_exact_canonical_extra_not_just_retained_seal(self):
        for change in('expired','actor','attempt','ranges','report','wrong_commitment','exact_commitment'):
            with self.subTest(change=change):
                h=Harness(self);job=h.chain.jobs[0]
                if change=='expired':job['expired']=True
                elif change=='actor':job['actor']='0014'+'2'*40
                elif change=='attempt':job['attempt']=1
                elif change=='ranges':job['ranges']=copy.deepcopy(job['ranges']);job['ranges'][0]['end']=str(int(job['ranges'][0]['end'])+1)
                elif change=='report':h.chain.report_root='4'*64
                else:job.update(state='committed',commitment='4'*64 if change=='wrong_commitment'else h.audit['answer']['commitment'])
                self.assertEqual(verify_start(h.flow,h.job),change=='exact_commitment')
                self.assertEqual(h.flow.physical_audits.completed_extras(h.w['request_id']),[])
                self.assertEqual(h.flow.cycles.snapshot(h.w['request_id'])['stage'],'prepared');self.assertEqual(h.flow.preseal.entries(),[])
    def test_tip_change_between_final_checks_cannot_promote(self):
        h=Harness(self);self.assertTrue(verify_start(h.flow,h.job));h.job.completed_result=copy.deepcopy(h.a.report)
        h.flow.audit_committed(0,h.audit,'e'*64)
        def moved(flow,cycle,anchor=None):h.chain.tip='b'*64;return True
        with patch('horizon_extra_publication_overlap._extra_gate',side_effect=moved):self.assertFalse(finish(h.flow,h.job,h.a.report))
        self.assertEqual(h.flow.cycles.snapshot(h.w['request_id'])['stage'],'prepared');self.assertEqual(h.flow.preseal.entries(),[])
        self.assertIsNone(h.source.grant);self.assertIsNotNone(h.flow.prefetch.ready)
    @unittest.skipUnless(BASELINE_FLOW, 'Requires exact GPU112 flow source')
    def test_exact_old_admission_refuses_same_positive_fixture(self):
        import importlib.util
        spec=importlib.util.spec_from_file_location('baseline_overlap_control',BASELINE_FLOW)
        baseline=importlib.util.module_from_spec(spec);spec.loader.exec_module(baseline)
        h=Harness(self);self.assertFalse(baseline.MiningFlow.may_start_work(h.flow,h.job))
        self.assertTrue(h.flow.may_start_work(h.job));self.assertTrue(verify_start(h.flow,h.job))
    def test_pending_publication_paces_reads_and_sidecar_purge_waits_for_seal(self):
        h=Harness(self);h.publish();h.start();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None)
        h.calls.clear();time.sleep(1.2)
        reads=[c for c in h.calls if c[0]=='getpgnscientificwork'];self.assertLessEqual(len(reads),2);h.assert_blocked()
        h.release_publish.set();h.assert_promoted()
        self.assertFalse(h.a.compact()['compacted']);h.a.intact()
        self.assertTrue(h.flow.preseal.pump(h.flow.finalizer,h.flow.outbox))
        entry=h.flow.outbox.first();self.assertIsNotNone(entry);self.assertTrue(h.flow.report_ready(entry))
        outcome=h.a.compact();self.assertTrue(outcome['compacted']);self.assertEqual(archive.result(h.a.folder,h.a.cfg),h.a.report)
    def test_real_gpu_queue_starts_while_actual_extra_publisher_blocks(self):
        h=Harness(self);h.publish();h.start();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None)
        h.assert_blocked();self.assertFalse(h.future.done());self.assertLess(h.times['gpu_start'],time.monotonic_ns());self.assertEqual(h.chain.sent,[])
        observed=time.monotonic_ns();h.release_publish.set();h.assert_promoted();self.assertLess(h.times['gpu_start'],h.times['publication_confirmed'])
        receipt=dict(schema='PGN_EXTRA_PUBLICATION_OVERLAP_WITNESS_V1',boundary='Real host queue/journals/publisher; synthetic engine and Core; no GPU kernel or network gain claim',
            work_request=h.w['request_id'],ticket=h.w['ticket'],extra_audit_id=h.extra,
            event_order=dict(gpu_start_ns=h.times['gpu_start'],blocked_observation_ns=observed,publication_confirmed_ns=h.times['publication_confirmed'],promoted_observation_ns=time.monotonic_ns()),
            while_blocked=dict(cycle_stage='prepared',report_sends=0,preseal_entries=0,outbox_entries=0,cpu_execute_calls=0,cpu_grant=None,native_receipt_unchanged=True,source_compaction_refused=True),
            after_confirmation=dict(cycle_stage=h.flow.cycles.snapshot(h.w['request_id'])['stage'],compute_calls=h.compute_calls,preseal_entries=len(h.flow.preseal.entries())))
        (HERE/'TEMPORAL-WITNESS.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf8')
    def test_rejection_retains_same_seal_and_retry_promotes_without_recompute(self):
        h=Harness(self);h.publish(reject=True);h.start();wait_for(h.computed.is_set);h.release_publish.set();h.publisher.join(2)
        self.assertTrue(h.publish_errors);h.assert_blocked();seal=h.lane.answers.recover_seal(f'{REQUEST}-0-0',h.audit,require_durable=True)
        self.assertEqual(seal['answer'],h.audit['answer']);h.publish_errors=[];h.lane.worker.ledger=h.chain.ledger
        self.assertEqual(h.lane.process_one(),'committed');h.assert_promoted()
    def test_expired_extra_replacement_uses_prepared_path_and_same_gpu_result(self):
        h=Harness(self);h.start();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None)
        h.chain.jobs[0]['expired']=True;self.assertEqual(h.lane.process_one(),'retired');h.assert_blocked()
        self.assertEqual(h.flow.cycles.extra_audits(h.w['request_id']),[])
        replacement=copy.deepcopy(h.audit);replacement['request_id']='f'*64;replacement['answer']=audit_commitment(replacement['ticket'],0,list(range(5)),['1','2','3','4','5'],'5'*64)
        h.chain.additional_jobs[0]=dict(state='assigned',actor=replacement['actor'],expired=False,actor_policy_accepted=True,ranges=h.ranges)
        h.seal(replacement);newid=audit_view_ref('f'*64,0,0)['audit_id'];h.flow.cycles.reserve_extra_audit(h.w['request_id'],newid)
        self.assertEqual(h.lane.process_one(),'committed');h.assert_promoted()
    def test_orphan_completion_cache_is_cleared_then_exact_replay_can_promote(self):
        h=Harness(self);h.start();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None)
        block='b'*64;h.chain.intents[block]={};h.flow.physical_audits.completed_extra(h.w['request_id'],h.extra,block)
        self.assertFalse(_extra_gate(h.flow,h.w['request_id']));self.assertEqual(h.flow.physical_audits.completed_extras(h.w['request_id']),[]);h.assert_blocked()
        h.flow.audit_committed(0,h.audit,'e'*64);h.assert_promoted()
    def test_expired_lease_stays_retained_and_absence_uses_existing_allocator_recovery(self):
        h=Harness(self);h.start();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None);h.expired=True
        self.assertEqual(ticket_status(h.flow,h.w)['status'],'expiry_pending');h.assert_blocked()
        h.absent=True
        wait_for(lambda:h.job.retired);self.assertIsNone(h.flow.prefetch.ready);self.assertIsInstance(h.future.exception(),CancelledError)
        paths=list((h.root/'speculative-search-retained').glob('*/result.json'));self.assertEqual(len(paths),1);saved=json.loads(paths[0].read_bytes());self.assertEqual(saved['result'],h.a.report)
        self.assertEqual(h.flow.preseal.entries(),[]);self.assertEqual(h.flow.cycles.snapshot(h.w['request_id'])['stage'],'prepared');self.assertFalse(h.a.compact()['compacted']);h.a.intact()
    def test_ticket_reorg_reuses_real_prepared_reassignment_handoff(self):
        h=Harness(self);h.start();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None)
        old={p.name:p.read_bytes()for p in h.a.folder.iterdir()if p.is_file()}
        h.changed=True;h.chain.tip='b'*64
        wait_for(lambda:h.job.retired);self.assertIsInstance(h.future.exception(),CancelledError)
        self.assertIsNone(h.flow.prefetch.ready);self.assertIsNone(h.flow.outbox.first());self.assertEqual(h.flow.preseal.entries(),[])
        self.assertEqual(old,{p.name:p.read_bytes()for p in h.a.folder.iterdir()if p.is_file()})
        current=h.flow.cycles.snapshot(h.w['request_id']);self.assertEqual(current['work']['ticket'],'8'*64);self.assertEqual(current['stage'],'prepared')
        successor=h.flow.reassignments.next_work();self.assertEqual(successor['ticket'],'8'*64)
        parked=list((h.root/'speculative-search-retained').glob('*/result.json'));self.assertEqual(len(parked),1)
        self.assertEqual(json.loads(parked[0].read_bytes())['result']['body']['ticket_id'],h.w['ticket'])
    def test_partial_reassignment_write_rolls_forward_without_losing_result(self):
        h=Harness(self);h.start();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None)
        h.changed=True;h.chain.tip='b'*64;original=h.flow.reassignments.finish;injected=[False]
        def broken(record):
            original(record)
            if not injected[0]:injected[0]=True;raise OSError('crash after durable reassignment')
        with patch.object(h.flow.reassignments,'finish',side_effect=broken):wait_for(lambda:h.job.retired)
        self.assertTrue(injected[0]);self.assertIsInstance(h.future.exception(),CancelledError);self.assertEqual(h.flow.preseal.entries(),[])
        self.assertEqual(h.flow.reassignments.next_work()['ticket'],'8'*64)
    def test_cpu_only_cannot_speculate_and_prepared_cpu_grant_is_rejected(self):
        h=Harness(self);h.worker.cpu_only=True;self.assertIsNone(candidate(h.flow,h.job));self.assertFalse(h.flow.may_start_work(h.job));self.assertFalse(verify_start(h.flow,h.job))
        with self.assertRaises(ValueError):cycle_request(h.job.ticket,h.job.context,h.flow.cycles.snapshot(h.w['request_id']))
        h.flow.audit_committed(0,h.audit,'e'*64);h.start();h.assert_promoted()
    def test_no_seal_wrong_actor_changed_ticket_or_uncertain_anchor_cannot_start(self):
        for kind in('seal','actor','ticket','anchor'):
            with self.subTest(kind=kind):
                h=Harness(self)
                if kind=='seal':
                    for p in(h.lane.folder/'sealed-answers').glob('*'):p.unlink()
                    (h.lane.folder/(f'{REQUEST}-0-0.json')).unlink()
                elif kind=='actor':h.lane.current_audit['actor']='0014'+'2'*40
                elif kind=='ticket':h.changed=True
                else:
                    original=h.call
                    def changed(method,args=None,**kw):return '4'*64 if method=='getbestblockhash' else original(method,args,**kw)
                    h.worker.rpc=SimpleNamespace(call=changed)
                try:allowed=verify_start(h.flow,h.job)
                except (ValueError,RuntimeError):allowed=False
                self.assertFalse(allowed);self.assertNotIn('extra_publication_overlap',h.job.context)
    def test_restart_reuses_durable_complete_source_before_same_publication(self):
        h=Harness(self);h.start();wait_for(h.computed.is_set);wait_for(lambda:h.job.completed_result is not None);h.assert_blocked();h.stop();h.flow.cycles.close()
        retained={p.name:hashlib.sha256(p.read_bytes()).hexdigest()for p in h.a.folder.iterdir()if p.is_file()}
        again=Harness(self,root=h.root,chain=h.chain,archive_fixture=h.a)
        again.flow.prior_active=json.loads((again.root/'gpu-active.json').read_bytes())
        with again.flow.gpu.condition:again.flow.gpu.jobs.clear();again.flow.gpu.search.clear()
        again.flow.prefetch.ready=None;again.flow.prefetch.on_ready=again.flow.prepared
        again.flow.prefetch.start();wait_for(lambda:again.w['ticket']in again.flow.gpu.jobs)
        again.job=again.flow.gpu.jobs[again.w['ticket']];again.future=again.job.future
        again.publish();again.start();wait_for(again.computed.is_set);again.assert_blocked()
        self.assertEqual(again.compute_calls,0);self.assertEqual(retained,{p.name:hashlib.sha256(p.read_bytes()).hexdigest()for p in h.a.folder.iterdir()if p.is_file()})
        again.release_publish.set();wait_for(again.future.done);again.future.result();self.assertEqual(again.errors,[]);self.assertEqual(again.flow.cycles.snapshot(again.w['request_id'])['stage'],'completed');self.assertEqual(again.compute_calls,0)

if __name__=='__main__':unittest.main(verbosity=2)
