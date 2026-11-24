from pathlib import Path
from types import SimpleNamespace
import sys,threading,unittest
from tempfile import TemporaryDirectory
from unittest.mock import Mock
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'src/protocol'),str(ROOT/'src/engine/runtime_host')]
from horizon_mining_flow import MiningFlow
from horizon_gpu_queue import ResidentGpuQueue
from horizon_work_cycle import WorkCycleJournal

def audit(n,done=0):
    return dict(request_id=f'{n:064x}',slot=0,attempt=0,counts=[1]*done)
def job(n):
    return SimpleNamespace(context=dict(audit_request_id=f'{n:064x}',audit_slot=0,audit_attempt=0))
def make(credits=(),lanes=()):
    f=object.__new__(MiningFlow)
    f.prefetch=SimpleNamespace(native_service_status=dict(enforced=True,may_request=False,credits=list(credits)))
    f.audit_lanes=[SimpleNamespace(current_audit=x)for x in lanes]
    f.unbound_service_audits=set();f.unbound_service_seeded=False
    f.unbound_service_completed=set();f.unbound_service_lock=threading.RLock()
    return f

class FixedAuditCycleTests(unittest.TestCase):
    def test_current_ticket_yields_only_for_confirmed_service_needed_by_successor(self):
        from horizon_gpu_queue import GpuJob

        actor, predecessor = 'actor', 'predecessor'
        current, alternate = (dict(audit(n, done), actor=actor, ticket=str(n), report_root='root',
                                   ranges=[dict(index=0, start='0', end='10')])
                              for n, done in ((100, 4), (101, 0)))
        flow = make(lanes=[current, alternate])
        flow.prefetch.ready = None
        flow.prefetch.refresh_ready_service_observation = Mock()
        flow.physical_audits = SimpleNamespace(is_native=lambda key: key == predecessor,
                                               bound_audit=lambda *_: None)
        flow.physical_assignment_target = lambda _: None
        flow.cycles = SimpleNamespace(extra_audit_cycle=lambda _: None,
                                      reserve_extra_audit=Mock(),
                                      may_search=lambda _: True,
                                      snapshot=lambda _: dict(stage='running', hold=False))
        flow.physical_ready = lambda key: key == predecessor
        flow.outbox = SimpleNamespace(capacity_available=lambda: True)
        flow.finalizer = None
        enabled, stop = threading.Event(), threading.Event()
        enabled.set()
        flow.gpu = ResidentGpuQueue(None, enabled, stop, None, None, None, None,
                                    clock_ns=lambda: 2_000_000_000, may_start=flow.may_start_work)
        search = GpuJob({'ticket_id': 'current'}, dict(role='search', work_cycle=predecessor), 10)
        flow.gpu.active = search
        scopes = [flow.ready_audit_scope(item) for item in (current, alternate)]
        for index, scope in enumerate(scopes):
            request, slot, attempt = scope[:3]
            identity, start, end = scope[-1][0]
            flow.gpu.submit(dict(ticket_id=identity, start=start, end=end),
                            dict(role='audit', audit_position=index, audit_request_id=request,
                                 audit_slot=slot, audit_attempt=attempt, audit_actor=actor,
                                 presentation={'audit_id': request}), 10)
        first, second = flow.gpu.audits
        tip = 'anchor'
        tip_rpc = Mock(return_value=tip)
        flow.worker = SimpleNamespace(address=lambda **_: 'signer', rpc=SimpleNamespace(call=tip_rpc))
        status = dict(schema='PGN_NATIVE_WORK_DUTY_STATUS_V1', enforced=True,
                      may_request=False, status='audit_work_required', credits=[audit(99)],
                      ready_audits={index: (scope, True) for index, scope in enumerate(scopes)},
                      observed_actor=actor, ready_signer='signer', bestblockhash=tip)
        flow.prefetch.native_service_status = dict(status, may_request=True, status='ready')
        self.assertFalse(flow.gpu.should_yield(search, 0))
        flow.prefetch.native_service_status = status
        flow.prefetch.ready = {'ticket': 'already-prepared'}
        self.assertFalse(flow.gpu.should_yield(search, 0))
        flow.prefetch.ready = None
        self.assertFalse(flow.may_start_work(second), 'One missing credit prioritizes the audit with progress')
        self.assertTrue(flow.gpu.should_yield(search, 0))
        tip_rpc.assert_not_called()
        flow.gpu.active = None
        flow.gpu.search.append(search)
        self.assertIs(flow.gpu._next(), first)
        self.assertEqual(first.context['preparing_ticket_service'], predecessor)

        def next_ticket_arrives(_):
            flow.prefetch.ready = {'ticket': 'already-prepared'}
            return tip

        tip_rpc.side_effect = next_ticket_arrives
        self.assertFalse(flow.confirm_ready_audit(first))
        flow.prefetch.ready = None
        tip_rpc.side_effect = None
        tip_rpc.reset_mock()
        flow.gpu.audits.appendleft(first)
        self.assertIs(flow.gpu._next(), first)
        self.assertTrue(flow.confirm_ready_audit(first))
        tip_rpc.assert_called_once_with('getbestblockhash')
        with TemporaryDirectory() as temporary:
            flow.view_sequence, flow.prior_active = 0, {}
            flow.worker.root = Path(temporary)
            flow.worker.device_lease = object()
            flow.worker.audits = SimpleNamespace()
            flow.worker.source = SimpleNamespace()
            flow.activate(first)
            self.assertNotIn('work_cycle', first.context)
            self.assertNotIn('audit_budget_cycle', first.context)
            flow.cycles.reserve_extra_audit.assert_not_called()
        flow.prefetch.native_service_status = dict(status, may_request=True, status='ready')
        self.assertIs(flow.gpu._next(), search)

    def test_confirmed_then_failed_service_is_replaced_without_restart(self):
        f=make(lanes=[audit(100),audit(101)])
        f.cycles=SimpleNamespace(bound_audit=lambda*a:None)
        self.assertTrue(f.may_unbound_audit(job(100)))
        self.assertTrue(f.may_unbound_audit(job(101)))
        f.audit_committed(0,audit(100),'a'*64)
        f.audit_committed(1,audit(101),'b'*64)
        self.assertIsNone(f.prefetch.native_service_status)
        self.assertFalse(f.may_unbound_audit(job(102)))
        f.audit_lanes=[SimpleNamespace(current_audit=audit(102,4)),SimpleNamespace(current_audit=audit(103))]
        f.prefetch.native_service_status=dict(enforced=True,may_request=False,credits=[audit(101)])
        self.assertTrue(f.may_unbound_audit(job(102)))
        self.assertFalse(f.may_unbound_audit(job(103)))
        self.assertNotIn(f.service_identity(audit(100)),f.unbound_service_audits)

    def test_transient_empty_ram_lane_does_not_release_running_reservation(self):
        f=make(lanes=[audit(100),audit(101)])
        f.may_unbound_audit(job(100));f.may_unbound_audit(job(101))
        f.audit_lanes=[SimpleNamespace(current_audit=None),SimpleNamespace(current_audit=None)]
        self.assertTrue(f.may_unbound_audit(job(100)))
        self.assertFalse(f.may_unbound_audit(job(102)))

    def test_old_core_observation_cannot_reappear_after_confirmation(self):
        from horizon_ticket_prefetch import TicketPrefetch
        old_observation_waiting=threading.Event();publish_old=threading.Event()
        class DelayedNetworkExit:
            def __enter__(self):return self
            def __exit__(self,*a):
                old_observation_waiting.set()
                if not publish_old.wait(3):raise RuntimeError('Controlled interleaving timed out')
        class Denied(Exception):
            native_service_status=dict(enforced=True,may_request=False,credits=[])
        def assign():raise Denied('audit required')
        enabled=threading.Event();enabled.set();stop=threading.Event()
        p=TicketPrefetch(SimpleNamespace(assign=assign),enabled,stop,DelayedNetworkExit())
        p.start()
        try:
            self.assertTrue(old_observation_waiting.wait(3))
            p.invalidate_native_service_status()
            stop.set();publish_old.set()
            self.assertTrue(p.close(3))
            self.assertIsNone(p.service_observation())
            self.assertTrue(p.publish_service_observation(p.native_service_generation,Denied.native_service_status))
            self.assertIsNotNone(p.service_observation())
        finally:stop.set();publish_old.set();p.close(3)

    def test_confirmed_audit_wakes_blocked_ticket_request(self):
        from horizon_ticket_prefetch import TicketPrefetch
        from horizon_native_work_duty import NativeAuditServiceRequired
        observed=threading.Event()
        ready=threading.Event()
        enabled=threading.Event()
        enabled.set()
        stop=threading.Event()
        attempts=[]
        def assign():
            attempts.append(1)
            if len(attempts)==1:
                raise NativeAuditServiceRequired("Audit required",dict(enforced=True,may_request=False))
            return {"ticket":"admitted"}
        prefetch=TicketPrefetch(SimpleNamespace(assign=assign),enabled,stop,threading.RLock(),lambda *_:ready.set())
        publish=prefetch.publish_service_observation
        def watch(generation,status):
            result=publish(generation,status)
            observed.set()
            return result
        prefetch.publish_service_observation=watch
        prefetch.start()
        try:
            self.assertTrue(observed.wait(3))
            self.assertFalse(ready.wait(.05))
            prefetch.invalidate_native_service_status()
            self.assertTrue(ready.wait(.6))
            self.assertEqual(prefetch.ready,{"ticket":"admitted"})
        finally:
            stop.set()
            self.assertTrue(prefetch.close(3))

    def test_two_startup_deliveries_cannot_become_unlimited_drain(self):
        f=make(lanes=[audit(100),audit(101)])
        self.assertTrue(f.may_unbound_audit(job(100)));self.assertTrue(f.may_unbound_audit(job(101)))
        f.prefetch.native_service_status['credits']=[audit(100)]
        f.audit_lanes[0].current_audit=audit(102)
        self.assertFalse(f.may_unbound_audit(job(102)))
        self.assertTrue(f.may_unbound_audit(job(101)))
        f.prefetch.native_service_status['credits']=[audit(100),audit(101)]
        f.prefetch.native_service_status['may_request']=True
        self.assertFalse(f.may_unbound_audit(job(103)))

    def test_restart_counts_core_commit_and_keeps_most_advanced_retained_audit(self):
        f=make(credits=[audit(100)],lanes=[audit(101,4),audit(102)])
        self.assertTrue(f.may_unbound_audit(job(101)))
        self.assertFalse(f.may_unbound_audit(job(102)))
        self.assertEqual(len(f.unbound_service_audits),2)

    def test_only_exact_consumed_service_references_release_local_dispatch(self):
        f=make(lanes=[audit(100),audit(101)])
        f.may_unbound_audit(job(100));f.may_unbound_audit(job(101))
        f.prefetch.native_service_status['credits']=[audit(100)]
        f.unbound_service_audits.difference_update([f.service_identity(audit(100))])
        self.assertFalse(f.may_unbound_audit(job(102))) # Core still says100 is available.
        f.prefetch.native_service_status['credits']=[]
        self.assertTrue(f.may_unbound_audit(job(102)));self.assertFalse(f.may_unbound_audit(job(103)))

    def test_missing_permission_never_becomes_automatic_backlog_draining(self):
        f=make();f.prefetch.native_service_status=None
        self.assertFalse(f.may_unbound_audit(job(100)))
        f.prefetch.native_service_status=dict(enforced=True,may_request=True,credits=[])
        self.assertFalse(f.may_unbound_audit(job(100)))

    def test_native_service_already_ready_allows_one_extra_not_two(self):
        cycle=dict(audits=[dict(stage='committed'),dict(stage='committed')])
        a='a'*64;b='b'*64
        self.assertTrue(WorkCycleJournal.extra_audit_available(cycle,a,[]))
        self.assertTrue(WorkCycleJournal.extra_audit_available(cycle,a,[a]))
        self.assertFalse(WorkCycleJournal.extra_audit_available(cycle,b,[a]))

    def test_queue_has_no_backlog_pressure_or_bulk_mode(self):
        q=ResidentGpuQueue(SimpleNamespace(),threading.Event(),threading.Event(),lambda*a:None,lambda*a:None,lambda*a:None,lambda*a:None)
        view=q.audit_service_view()
        self.assertEqual((view['regular_audits'],view['catch_up_audits']),(2,1))
        self.assertFalse(hasattr(q,'audit_pressure'));self.assertFalse(hasattr(q,'update_audit_backlog'))

if __name__=='__main__':unittest.main()
