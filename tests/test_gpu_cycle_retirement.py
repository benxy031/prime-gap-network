"""Four-orphan GPU capacity qualification. No native/GPU/live network activity."""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import copy,json,sys,tempfile,threading,unittest
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
PROJECT=HERE.parent
SOURCE=PROJECT if (PROJECT/'src/engine/runtime_host').is_dir()else HERE.parent/'gpu111-joint-private'
sys.path[:0]=[str(HERE),str(SOURCE/'src/engine/runtime_host'),str(SOURCE/'src/protocol')]
import horizon_cycle_retirement as retirement
from horizon_work_cycle import WorkCycleJournal
from horizon_cycle_execution import CycleExecution
from horizon_native_work_duty import tasks
from horizon_mining_flow import MiningFlow
from horizon_ticket_archive import ARCHIVE_NAME

CHAIN='a'*64;ANCHOR='b'*64
OBSERVATIONS={}

def native(work,n=1000):
    return dict(schema='PGN_NATIVE_ASSIGNED_AUDIT_SERVICE_V1',enforced=True,request_id=work['request_id'],
                issuance_block=ANCHOR,issuance_height=1,credits=[
                    dict(request_id=f'{n:064x}',slot=0,attempt=0,commitment='c'*64),
                    dict(request_id=f'{n+1:064x}',slot=1,attempt=0,commitment='d'*64)])

class Queue:
    def __init__(self):self.lock=threading.RLock();self.rows=[]
    def _read(self):return copy.deepcopy(self.rows)
    def entries(self):return copy.deepcopy(self.rows)

class Rpc:
    def __init__(self,flow):
        self.flow=flow;self.tip=ANCHOR;self.leases={};self.reports={};self.calls=[];self.after=None;self.mutate=None
    def call(self,method,args=None):
        assert not self.flow.gpu.condition._is_owned(),'Network I/O under GPU scheduler lock'
        self.calls.append((method,args))
        if method=='getpgnworkview':
            request=args[3]
            value=dict(schema='PGN_WORK_VIEW_V1',kind='tickets',offset=0,limit=1,request_id=request,
                       state=dict(genesis=CHAIN,bestblockhash=ANCHOR,height=999,median_time_past=100000,lease_offset=0,
                                  leases=copy.deepcopy(self.leases.get(request,[]))),
                       reports=copy.deepcopy(self.reports.get(request,[])))
        elif method=='getbestblockhash':value=self.tip
        else:raise AssertionError((method,args))
        if method=='getpgnworkview'and self.mutate:self.mutate(value)
        if self.after:self.after(method,args)
        return value

class GpuRetirementTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=HERE,prefix='linux-shaped-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);folder=self.root/'work-cycles';folder.mkdir()
        snapshot=dict(schema='PGN_WORK_CYCLES_V1',genesis=CHAIN,cycles=[])
        for number,ticket_number in enumerate(('2317','10039','5844','4379'),1):
            work=dict(request_id=f'{number:064x}',ticket=f'{number+100:064x}',ticket_number=ticket_number,
                      start='150000000000000000000',length='200000000000000',target_numerator='1846001',target_denominator='65536')
            receipt=native(work,1000+number*2)
            snapshot['cycles'].append(dict(work=work,stage='running',hold='',native_service=receipt,audits=tasks(receipt,work)))
        (folder/'cycles.json').write_text(json.dumps(snapshot),encoding='utf8')
        self.j=WorkCycleJournal(folder,CHAIN);self.addCleanup(self.j.close)
        self.old=copy.deepcopy(self.j.value['cycles']);self.cycle=self.old[0];self.request=self.cycle['work']['request_id']
        self.incoming=dict(self.cycle['work'],genesis=CHAIN,request_id='e'*64,ticket='f'*64,ticket_number='20000')
        self.flow=MiningFlow.__new__(MiningFlow);self.flow.cycles=self.j
        self.flow.gpu=SimpleNamespace(condition=threading.Condition(),jobs={},active=None,resume_job=None)
        self.flow.worker=SimpleNamespace(root=self.root,cpu_only=False,current=None,network_lock=threading.RLock(),args=SimpleNamespace(genesis=CHAIN))
        self.flow.outbox=Queue();self.flow.preseal=Queue();self.flow.recovered_report=None
        self.flow.prefetch=SimpleNamespace(condition=threading.Condition(),ready=None)
        self.flow.audit_lanes=[];self.flow.reassignments=None
        self.flow.physical_audits=CycleExecution(folder,CHAIN)
        self.flow.unbound_service_lock=threading.RLock();self.flow.unbound_service_audits=set();self.flow.unbound_service_completed=set()
        self.rpc=Rpc(self.flow);self.flow.worker.rpc=self.rpc
        self.frontier=self.root/'retained-frontier.bin';self.frontier.write_bytes(b'exact retained frontier - do not prune or rewrite')
    def reconcile(self):return retirement.reconcile_capacity(self.flow,self.incoming)
    def block_others(self):
        for c in self.old[1:]:
            req=c['work']['request_id'];self.rpc.leases[req]=[dict(request=req,state='assigned')]
    def protected_check(self):
        self.block_others();before=self.j.file.read_bytes()
        self.assertFalse(self.reconcile());self.assertEqual(self.j.file.read_bytes(),before)
    def saved(self,path,value):
        path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value),encoding='utf8')
    def job(self,role='search',key='work_cycle'):
        return SimpleNamespace(context=dict(role=role,**{key:self.request}))
    def test_exact_four_running_committed_cycles_prepare_fifth_without_false_completion(self):
        before=self.frontier.read_bytes();incoming_receipt=native(self.incoming,2000)
        self.incoming['native_audit_service']=incoming_receipt
        with patch('horizon_native_work_duty.observe',return_value=incoming_receipt):self.flow.prepare_cycle(self.incoming,new_gpu_work=True)
        self.assertEqual(len(self.j.value['cycles']),4);self.assertFalse(any(c['stage']=='completed'for c in self.j.value['cycles']))
        self.assertEqual([c['work']['ticket_number']for c in self.old],['2317','10039','5844','4379'])
        self.assertEqual([a['stage']for a in self.cycle['audits']],['committed','committed'])
        with self.j.history.database()as db:
            rows=db.execute('SELECT receipt FROM inactive_native_cycles').fetchall()
        self.assertEqual(len(rows),1);receipt=json.loads(rows[0][0]);self.assertEqual(receipt['cycle'],self.cycle)
        self.assertEqual(receipt['cycle']['stage'],'running');self.assertEqual(self.frontier.read_bytes(),before)
        OBSERVATIONS['four_cycles']=dict(hot_after=4,retired=1,retired_stage='running',false_completed=0,
                                         rpc_calls=len(self.rpc.calls),science_frontier_unchanged=True)
    def test_active_gpu_search_blocks_retirement_even_if_jobs_dict_is_empty(self):
        self.flow.gpu.active=self.job();self.protected_check()
    def test_queued_gpu_audit_blocks_its_bound_cycle(self):
        self.flow.gpu.jobs['audit']=self.job('audit');self.protected_check()
    def test_paused_gpu_search_blocks_its_cycle(self):
        self.flow.gpu.resume_job=self.job();self.protected_check()
    def test_extra_audit_reservation_protects_budget_owner(self):
        self.flow.gpu.jobs['extra']=self.job('audit','audit_budget_cycle');self.protected_check()
    def test_live_current_work_is_protected(self):
        self.flow.worker.current=self.cycle['work'];self.protected_check()
    def test_prefetched_work_is_protected(self):
        self.flow.prefetch.ready=self.cycle['work'];self.protected_check()
    def test_recovered_report_handoff_is_protected(self):
        self.flow.recovered_report=(self.cycle['work'],dict(body={}));self.protected_check()
    def test_saved_current_work_is_protected(self):
        self.saved(self.root/'controller.json',dict(stage='ASSIGNED',work=self.cycle['work']));self.protected_check()
    def test_saved_prepared_work_is_protected(self):
        self.saved(self.root/'prepared-ticket'/'controller.json',dict(stage='ASSIGNED',work=self.cycle['work']));self.protected_check()
    def test_consumed_prepared_cursor_is_not_an_executor(self):
        self.saved(self.root/'prepared-ticket'/'controller.json',dict(stage='CONSUMED',work=self.cycle['work']))
        self.assertTrue(self.reconcile())
    def test_saved_gpu_audit_binding_is_protected(self):
        self.saved(self.root/'gpu-active.json',dict(role='audit',context=dict(work_cycle=self.request,work=None)))
        self.protected_check()
    def test_dejan_null_saved_audit_context_does_not_claim_every_old_cycle(self):
        self.saved(self.root/'gpu-active.json',dict(role='audit',context=dict(work=dict(ticket_number=None,request_id=None))))
        self.assertTrue(self.reconcile())
    def test_live_lane_audit_binding_is_protected(self):
        self.flow.audit_lanes=[SimpleNamespace(current_audit=dict(physical_cycle=self.request),folder=self.root/'lane')]
        self.protected_check()
    def test_saved_lane_audit_binding_is_protected(self):
        lane=SimpleNamespace(current_audit=None,folder=self.root/'lane');self.flow.audit_lanes=[lane]
        self.saved(lane.folder/'current.json',dict(physical_cycle=self.request));self.protected_check()
    def test_outbox_report_is_protected(self):
        self.flow.outbox.rows=[dict(work=self.cycle['work'])];self.protected_check()
    def test_preseal_candidate_is_protected(self):
        self.flow.preseal.rows=[dict(request=self.request,ticket=self.cycle['work']['ticket'],input_sha256='c'*64)]
        self.protected_check()
    def test_unindexed_report_or_compact_result_is_protected(self):
        folder=self.root/'tickets'/self.cycle['work']['ticket'];folder.mkdir(parents=True)
        for name in ('report.json','network-report.json','preseal-input.json','preseal-final.json',ARCHIVE_NAME):
            with self.subTest(name=name):
                file=folder/name;file.write_bytes(b'existing immutable result');self.protected_check();file.unlink()
    def test_complete_session_without_report_index_is_protected(self):
        self.saved(self.root/'tickets'/self.cycle['work']['ticket']/'session.json',dict(state='COMPLETE'))
        self.protected_check()
    def test_reassignment_ready_work_is_protected(self):
        queue=SimpleNamespace(lock=threading.RLock(),read=lambda:['1'*64],record=lambda key:dict(previous=self.cycle['work'],work=self.incoming))
        self.flow.reassignments=queue;self.protected_check()
    def test_unbound_gpu_audit_owner_is_protected(self):
        self.flow.gpu.jobs['audit']=self.job('audit','audit_request_id');self.protected_check()
    def test_pending_physical_assignment_without_live_lane_is_protected(self):
        physical=self.flow.physical_audits;physical.prepare(self.request,{self.request})
        physical.bind(self.request,0,dict(request_id='6'*64,slot=0,attempt=0));self.protected_check()
    def test_unfinished_extra_reservation_is_protected_without_a_job(self):
        physical=self.flow.physical_audits;physical.prepare(self.request,{self.request})
        self.j.reserve_extra_audit(self.request,'6'*64);self.protected_check()
    def test_finished_physical_receipts_are_retained_before_cache_pruning(self):
        physical=self.flow.physical_audits;physical.prepare(self.request,{self.request});physical.mark_native(self.request,{self.request})
        for slot in (0,1):
            audit=dict(request_id=str(slot+6)*64,slot=slot,attempt=0)
            physical.bind(self.request,slot,audit);physical.committed(self.request,slot,audit,str(slot+8)*64)
        before=retirement._physical_service(self.flow,self.request)
        self.assertTrue(self.reconcile())
        physical.prepare(self.incoming['request_id'],{self.incoming['request_id']})
        physical.mark_native(self.incoming['request_id'],{self.incoming['request_id']})
        with self.j.history.database()as db:
            record=json.loads(db.execute('SELECT receipt FROM inactive_native_cycles WHERE request=?',(self.request,)).fetchone()[0])
        self.assertEqual(record['physical_service'],before)
        self.assertEqual(record['cycle']['stage'],'running')
    def test_unfinished_extra_without_any_physical_snapshot_is_protected(self):
        self.flow.physical_audits=None
        self.j.reserve_extra_audit(self.request,'6'*64);self.protected_check()
    def test_empty_confirmed_extra_set_still_protects_an_unfinished_reservation(self):
        self.flow.physical_audits.extras[self.request]={}
        self.j.reserve_extra_audit(self.request,'6'*64);self.protected_check()
    def test_missing_or_wrong_exact_view_slice_is_not_absence(self):
        for owner,key,value in [('outer','offset',100),('outer','limit',100),('state','lease_offset',100),
                                ('outer','offset',None),('state','lease_offset',None)]:
            with self.subTest(owner=owner,key=key,value=value):
                def mutate(view):
                    target=view if owner=='outer'else view['state']
                    if value is None:target.pop(key)
                    else:target[key]=value
                self.rpc.mutate=mutate
                with self.assertRaises(ValueError):self.reconcile()
                self.assertEqual(len(self.j.value['cycles']),4)
    def test_reorg_reintroduced_exact_allocation_is_not_silently_recreated(self):
        self.assertTrue(self.reconcile());self.assertFalse(self.j.contains(self.request))
        with self.assertRaises(ValueError):self.j.snapshot(self.request)
        with self.j.history.database()as db:
            before=db.execute('SELECT receipt FROM inactive_native_cycles WHERE request=?',(self.request,)).fetchone()[0]
        old=dict(self.cycle['work'],genesis=CHAIN,native_audit_service=self.cycle['native_service'])
        self.rpc.leases[self.request]=[dict(request=self.request,state='assigned')]
        with patch('horizon_native_work_duty.observe',return_value=self.cycle['native_service']):
            with self.assertRaisesRegex(RuntimeError,'Retained inactive allocation reappeared'):
                self.flow.prepare_cycle(old)
        self.assertFalse(self.j.contains(self.request));self.assertEqual(len(self.j.value['cycles']),3)
        with self.j.history.database()as db:
            after=db.execute('SELECT receipt FROM inactive_native_cycles WHERE request=?',(self.request,)).fetchone()[0]
        self.assertEqual(before,after);self.assertEqual(json.loads(after)['cycle']['stage'],'running')
    def test_hot_authority_survives_archive_then_write_crash_on_exact_resume(self):
        with patch.object(retirement,'atomic_bytes',side_effect=OSError('power loss')):
            with self.assertRaises(OSError):self.reconcile()
        old=dict(self.cycle['work'],genesis=CHAIN)
        self.assertFalse(retirement.reconcile_capacity(self.flow,old))
        self.assertEqual(self.j.snapshot(self.request),self.cycle)
    def test_another_ticket_does_not_match_the_exact_inactive_allocation_guard(self):
        self.assertTrue(self.reconcile())
        other=dict(self.cycle['work'],genesis=CHAIN,ticket='9'*64)
        self.assertFalse(retirement.reconcile_capacity(self.flow,other))
    def test_actual_core_live_lease_blocks_retirement(self):
        for c in self.old:self.rpc.leases[c['work']['request_id']]=[dict(request=c['work']['request_id'],state='assigned')]
        self.assertFalse(self.reconcile());self.assertEqual(len(self.j.value['cycles']),4)
    def test_present_expired_or_archived_lease_is_not_treated_as_absence(self):
        self.block_others()
        for state in ('assigned','archived','expired'):
            self.rpc.leases[self.request]=[dict(request=self.request,state=state,deadline_clock='median_chain_time',deadline_time=1)]
            self.assertFalse(self.reconcile())
    def test_existing_core_report_blocks_retirement(self):
        self.block_others();self.rpc.reports[self.request]=[dict(request_id=self.request)]
        self.assertFalse(self.reconcile())
    def test_reorg_during_inactivity_read_creates_no_retirement(self):
        self.block_others();self.rpc.tip='c'*64
        self.assertFalse(self.reconcile());self.assertEqual(len(self.j.value['cycles']),4)
    def test_scheduler_claim_during_rpc_is_rechecked(self):
        self.block_others()
        def claim(method,args):
            if method=='getbestblockhash':self.flow.gpu.active=self.job('audit')
        self.rpc.after=claim
        self.assertFalse(self.reconcile());self.assertEqual(self.j.snapshot(self.request),self.cycle)
    def test_outbox_arrival_during_rpc_is_rechecked(self):
        self.block_others()
        def arrive(method,args):
            if method=='getbestblockhash':self.flow.outbox.rows=[dict(work=self.cycle['work'])]
        self.rpc.after=arrive;self.assertFalse(self.reconcile())
    def test_saved_prepared_arrival_during_rpc_is_rechecked(self):
        self.block_others()
        def arrive(method,args):
            if method=='getbestblockhash':self.saved(self.root/'prepared-ticket'/'controller.json',dict(stage='ASSIGNED',work=self.cycle['work']))
        self.rpc.after=arrive;self.assertFalse(self.reconcile())
    def test_changed_local_cycle_after_observation_is_not_retired(self):
        self.block_others()
        def changed(method,args):
            if method=='getbestblockhash':self.j.hold(self.request,'retained reconciliation')
        self.rpc.after=changed
        with self.assertRaises(ValueError):self.reconcile()
        self.assertEqual(len(self.j.value['cycles']),4)
    def test_archive_then_hot_write_crash_keeps_both_copies_and_replays(self):
        before=self.j.file.read_bytes()
        with patch.object(retirement,'atomic_bytes',side_effect=OSError('disk full')):
            with self.assertRaises(OSError):self.reconcile()
        self.assertEqual(self.j.file.read_bytes(),before)
        self.assertTrue(self.reconcile());self.assertEqual(len(self.j.value['cycles']),3)
        with self.j.history.database()as db:self.assertEqual(db.execute('SELECT count(*)FROM inactive_native_cycles').fetchone()[0],1)
    def test_no_rpc_when_existing_completed_eviction_or_spare_capacity_suffices(self):
        self.j.advance(self.request,'completed');self.rpc.call=lambda*a:(_ for _ in()).throw(AssertionError('unneeded RPC'))
        self.assertFalse(self.reconcile())
    def test_holds_or_unfinished_audit_are_never_scheduled_for_retirement(self):
        for c in self.j.value['cycles']:c['hold']='reconciliation'
        self.assertFalse(self.reconcile());self.assertEqual(self.rpc.calls,[])

if __name__=='__main__':unittest.main(verbosity=2)
