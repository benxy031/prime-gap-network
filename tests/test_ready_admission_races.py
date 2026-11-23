"""Exercise READY authorization at the GPU queue boundary without a device."""

from concurrent.futures import CancelledError, Future
from pathlib import Path
from threading import Event, RLock, Thread
from types import SimpleNamespace
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / path) for path in ('src/engine/runtime_host', 'src/protocol')]

from horizon_audit_progress import partition_identity
from horizon_gpu_queue import ResidentGpuQueue
from horizon_mining_flow import MiningFlow
from horizon_ticket_prefetch import TicketPrefetch


class ReadyFixture:
    def __init__(self, required=False):
        self.enabled = Event()
        self.enabled.set()
        self.stopping = Event()
        self.anchor = 'b' * 64
        self.entered_anchor = None
        self.release_anchor = None
        self.genesis = 'a' * 64
        self.signer = 'signer-a'
        self.actor = '0014' + '1' * 40
        self.rejected_requests = set()
        self.incomplete_requests = set()
        self.required = required
        self.lane = SimpleNamespace(current_audit=self.audit('1', self.actor))
        flow = MiningFlow.__new__(MiningFlow)
        flow.worker = SimpleNamespace(args=SimpleNamespace(genesis=self.genesis),
                                      address=self.address, rpc=SimpleNamespace(call=self.rpc))
        flow.unbound_service_lock = RLock()
        flow.unbound_service_audits = set()
        flow.unbound_service_completed = set()
        flow.unbound_service_seeded = False
        flow.audit_lanes = [self.lane]
        self.lane.worker = flow.worker
        flow.prefetch = TicketPrefetch(None, self.enabled, self.stopping, RLock())
        flow.gpu = SimpleNamespace(jobs={})
        self.flow = flow
        observation = flow.read_unbound_service()
        if not observation or not flow.prefetch.publish_service_observation(0, observation):
            raise AssertionError('READY observation was not established')

    @staticmethod
    def audit(key, actor):
        return dict(request_id=key * 64, slot=0, attempt=1, actor=actor,
                    ticket='9' * 64, report_root='8' * 64,
                    ranges=[dict(index=0, start='100', end='200')])

    def address(self, *, create):
        if create:
            raise AssertionError('READY must use the existing signer')
        return self.signer

    def rpc(self, method, args=None):
        if method == 'validateaddress':
            return dict(isvalid=True, scriptPubKey=self.actor)
        if method == 'getbestblockhash':
            if self.entered_anchor is not None:
                self.entered_anchor.set()
                if not self.release_anchor.wait(3):
                    raise AssertionError('Test did not release the anchor read')
            return self.anchor
        if method == 'getpgnworkdutystatus':
            return dict(schema='PGN_NATIVE_WORK_DUTY_STATUS_V1', genesis=self.genesis,
                        bestblockhash=self.anchor, enforced=True,
                        status='audit_work_required' if self.required else 'ready',
                        may_request=not self.required,
                        credits=[dict(request_id=key * 64, slot=1, attempt=0)
                                 for key in ('d' if self.required else 'de')])
        if method == 'getpgnworkview':
            request = args[3]
            audit = next(lane.current_audit for lane in self.flow.audit_lanes
                         if lane.current_audit is not None and lane.current_audit['request_id'] == request)
            return dict(schema='PGN_WORK_VIEW_V1', kind='audits', request_id=request,
                        state=dict(bestblockhash=self.anchor, genesis=self.genesis),
                        reports=[dict(request_id=request, ticket=audit['ticket'],
                                      report_root=audit['report_root'])],
                        audits=[] if request in self.incomplete_requests else [dict(request_id=request, jobs=[dict(state='assigned',
                                     expired=request in self.rejected_requests,
                                     actor_policy_accepted=True, **audit)])])
        raise AssertionError(method)

    @staticmethod
    def job(audit, position=0):
        part = audit['ranges'][0]
        return SimpleNamespace(ticket=dict(ticket_id=partition_identity(audit, part),
                                           start=part['start'], end=part['end']),
                               future=Future(), retired=False, storage_retry_after=0,
                               context=dict(role='audit', audit_position=position, audit_actor=audit['actor'],
                                            audit_request_id=audit['request_id'], audit_slot=audit['slot'],
                                            audit_attempt=audit['attempt']))


class ReadyAdmissionRaceTests(unittest.TestCase):
    def test_one_core_credit_does_not_allow_expired_lane_to_block_live_assignment(self):
        fixture = ReadyFixture(required=True)
        flow = fixture.flow
        old = fixture.lane.current_audit
        old['counts'] = ['0', '0', '0']
        flow.unbound_service_audits.add(flow.service_identity(old))
        fixture.rejected_requests.add(old['request_id'])
        next_audit = fixture.audit('2', fixture.actor)
        flow.audit_lanes.append(SimpleNamespace(current_audit=next_audit, worker=flow.worker))
        flow.prefetch.invalidate_native_service_status()
        generation = flow.prefetch.native_service_generation
        self.assertTrue(flow.prefetch.publish_service_observation(generation, flow.read_unbound_service()))
        expired, live = fixture.job(old), fixture.job(next_audit, 1)
        retired = []
        flow.gpu.retire_audit = lambda *args: retired.append(args)

        self.assertTrue(flow.may_unbound_audit(live))
        self.assertTrue(flow.confirm_ready_audit(live))
        self.assertEqual(flow.unbound_service_audits, {flow.service_identity(next_audit)})
        self.assertTrue(flow.may_unbound_audit(expired))
        self.assertFalse(flow.confirm_ready_audit(expired))
        self.assertEqual(retired, [(0, *flow.service_identity(old))])
        self.assertEqual(old['counts'], ['0', '0', '0'])

    def test_partial_view_does_not_reset_sibling_observation_or_spend_slot(self):
        fixture = ReadyFixture(required=True)
        flow = fixture.flow
        waiting = fixture.lane.current_audit
        sibling = fixture.audit('2', fixture.actor)
        flow.audit_lanes.append(SimpleNamespace(current_audit=sibling, worker=flow.worker))
        fixture.incomplete_requests.add(waiting['request_id'])
        flow.prefetch.invalidate_native_service_status()
        status = flow.read_unbound_service()
        self.assertTrue(flow.prefetch.publish_service_observation(flow.prefetch.native_service_generation, status))
        current = fixture.job(sibling, 1)
        flow.gpu.jobs[current.ticket['ticket_id']] = current
        self.assertFalse(flow.may_unbound_audit(fixture.job(waiting)))
        self.assertIs(flow.prefetch.service_observation(), status)
        self.assertTrue(flow.may_unbound_audit(current))
        self.assertTrue(flow.confirm_ready_audit(current))

    def test_backoff_retirement_and_wrong_range_do_not_reserve_last_slot(self):
        for reason in ('storage_retry_after', 'retired', 'wrong_range', 'bound_cycle'):
            with self.subTest(reason=reason):
                fixture = ReadyFixture(required=True)
                flow = fixture.flow
                prior = fixture.lane.current_audit
                prior['counts'] = ['0', '0', '0']
                sibling = fixture.audit('2', fixture.actor)
                flow.audit_lanes.append(SimpleNamespace(current_audit=sibling, worker=flow.worker))
                flow.prefetch.invalidate_native_service_status()
                status = flow.read_unbound_service()
                self.assertTrue(flow.prefetch.publish_service_observation(flow.prefetch.native_service_generation, status))
                old = fixture.job(prior)
                if reason == 'wrong_range':
                    old.ticket['end'] = '201'
                elif reason == 'bound_cycle':
                    old.context['work_cycle'] = 'c' * 64
                else:
                    setattr(old, reason, True if reason == 'retired' else time.monotonic() + 20)
                flow.gpu.jobs[old.ticket['ticket_id']] = old
                self.assertTrue(flow.may_unbound_audit(fixture.job(sibling, 1)))

    def test_unknown_lane_refreshes_when_the_observed_sibling_has_disappeared(self):
        fixture = ReadyFixture(required=True)
        flow = fixture.flow
        sibling = fixture.audit('2', fixture.actor)
        flow.audit_lanes.append(SimpleNamespace(current_audit=sibling, worker=flow.worker))
        fixture.incomplete_requests.add(sibling['request_id'])
        flow.prefetch.invalidate_native_service_status()
        status = flow.read_unbound_service()
        self.assertTrue(flow.prefetch.publish_service_observation(flow.prefetch.native_service_generation, status))
        fixture.lane.current_audit = None
        fixture.incomplete_requests.clear()
        job = fixture.job(sibling, 1)
        self.assertFalse(flow.may_unbound_audit(job))
        self.assertTrue(flow.prefetch.refresh_requested_service_observation(flow.read_unbound_service))
        self.assertTrue(flow.may_unbound_audit(job))

    def test_confirmed_journal_without_future_does_not_reserve_last_slot(self):
        fixture = ReadyFixture(required=True)
        flow = fixture.flow
        waiting = fixture.lane.current_audit
        waiting['counts'] = ['0', '0', '0']
        sibling = fixture.audit('2', fixture.actor)
        flow.audit_lanes.append(SimpleNamespace(current_audit=sibling, worker=flow.worker))
        flow.prefetch.invalidate_native_service_status()
        status = flow.read_unbound_service()
        self.assertTrue(flow.prefetch.publish_service_observation(flow.prefetch.native_service_generation, status))
        current = fixture.job(sibling, 1)
        self.assertTrue(flow.may_unbound_audit(current))
        self.assertTrue(flow.confirm_ready_audit(current))

    def check_control_during_anchor(self, *, stop):
        fixture = ReadyFixture()
        fixture.entered_anchor = Event()
        fixture.release_anchor = Event()
        pauses = Event()
        attempts = []
        activations = []
        errors = []

        def run_ticket(ticket, _limit):
            attempts.append(ticket['ticket_id'])
            return dict(state='COMPLETED', ticket_id=ticket['ticket_id'])

        queue = ResidentGpuQueue(SimpleNamespace(run_ticket=run_ticket), fixture.enabled,
                                 fixture.stopping, activations.append, lambda *_: None, pauses.set,
                                 errors.append, may_start=fixture.flow.may_start_work,
                                 check_start=fixture.flow.confirm_ready_audit)
        job = fixture.job(fixture.lane.current_audit)
        future = queue.submit(job.ticket, job.context, 100)
        thread = Thread(target=queue.run, daemon=True)
        thread.start()
        try:
            self.assertTrue(fixture.entered_anchor.wait(3))
            self.assertTrue(queue.condition.acquire(timeout=1), 'Anchor RPC holds the queue mutex')
            try:
                if stop:
                    fixture.stopping.set()
                    queue.wake()
                else:
                    fixture.enabled.clear()
                    queue.request_pause_ack()
            finally:
                queue.condition.release()
                fixture.release_anchor.set()

            if stop:
                thread.join(3)
                self.assertFalse(thread.is_alive())
                with self.assertRaises(CancelledError):
                    future.result(timeout=1)
            else:
                self.assertTrue(pauses.wait(3))
                self.assertFalse(future.done())
                with queue.condition:
                    self.assertEqual(len(queue.audits), 1)
                    self.assertIs(queue.audits[0].future, future)

            self.assertEqual(activations, [])
            self.assertEqual(attempts, [])
            self.assertEqual(errors, [])

            if not stop:
                fixture.enabled.set()
                queue.wake()
                self.assertEqual(future.result(timeout=3)['ticket_id'], job.ticket['ticket_id'])
                self.assertEqual(len(activations), 1)
                self.assertEqual(attempts, [job.ticket['ticket_id']])
        finally:
            fixture.stopping.set()
            fixture.release_anchor.set()
            queue.wake()
            thread.join(3)

    def test_off_during_anchor_preserves_future_without_activating(self):
        self.check_control_during_anchor(stop=False)

    def test_stop_during_anchor_cancels_waiter_without_activating(self):
        self.check_control_during_anchor(stop=True)

    def test_actor_renewal_requires_current_lane_and_new_core_observation(self):
        fixture = ReadyFixture()
        flow = fixture.flow
        observation = flow.prefetch.service_observation()
        old = fixture.job(fixture.lane.current_audit)
        actor_b = '0014' + '2' * 40
        replacement = fixture.audit('1', actor_b)
        new = fixture.job(replacement)

        self.assertTrue(flow.may_unbound_audit(old))
        self.assertFalse(flow.may_unbound_audit(new))
        self.assertIs(flow.prefetch.service_observation(), observation)

        fixture.lane.current_audit = replacement
        fixture.actor = actor_b
        fixture.signer = 'signer-b'
        self.assertFalse(flow.may_unbound_audit(old))
        self.assertIs(flow.prefetch.service_observation(), observation)
        self.assertFalse(flow.may_unbound_audit(new))
        self.assertIsNone(flow.prefetch.service_observation())
        self.assertTrue(flow.prefetch.refresh_requested_service_observation(flow.read_unbound_service))
        self.assertEqual(flow.prefetch.service_observation()['observed_actor'], actor_b)
        self.assertEqual(flow.prefetch.service_observation()['ready_signer'], 'signer-b')
        self.assertFalse(flow.may_unbound_audit(old))
        self.assertTrue(flow.may_unbound_audit(new))
        self.assertTrue(flow.confirm_ready_audit(new))

    def test_expired_lane_releases_own_future_without_activating(self):
        fixture = ReadyFixture()
        flow = fixture.flow
        expired = fixture.audit('2', fixture.actor)
        fixture.rejected_requests.add(expired['request_id'])
        flow.audit_lanes.append(SimpleNamespace(current_audit=expired, worker=flow.worker))
        flow.prefetch.invalidate_native_service_status()
        flow.prefetch.request_missing_service_observation()
        self.assertTrue(flow.prefetch.refresh_requested_service_observation(flow.read_unbound_service))
        blocked = fixture.job(expired, position=1)
        valid = fixture.job(fixture.lane.current_audit)
        blocked_id = blocked.ticket['ticket_id']
        valid_id = valid.ticket['ticket_id']
        events = []
        errors = []

        def check_start(job):
            events.append(('check', job.ticket['ticket_id']))
            return flow.confirm_ready_audit(job)

        def run_ticket(ticket, _limit):
            events.append(('compute', ticket['ticket_id']))
            return dict(state='COMPLETED', ticket_id=ticket['ticket_id'])

        def activate(job):
            events.append(('activate', job.ticket['ticket_id']))

        flow.gpu = ResidentGpuQueue(SimpleNamespace(run_ticket=run_ticket), fixture.enabled,
                                   fixture.stopping, activate, lambda *_: None, lambda: None,
                                   errors.append, may_start=flow.may_start_work, check_start=check_start)
        blocked_future = flow.gpu.submit(blocked.ticket, blocked.context, 100)
        valid_future = flow.gpu.submit(valid.ticket, valid.context, 100)
        thread = Thread(target=flow.gpu.run, daemon=True)
        thread.start()
        try:
            with self.assertRaises(CancelledError):
                blocked_future.result(timeout=3)
            self.assertEqual(valid_future.result(timeout=3)['ticket_id'], valid_id)
            self.assertEqual(events, [('check', blocked_id), ('check', valid_id),
                                      ('activate', valid_id), ('compute', valid_id)])
            self.assertEqual(errors, [])
        finally:
            fixture.stopping.set()
            flow.gpu.wake()
            thread.join(3)
            self.assertFalse(thread.is_alive())


if __name__ == '__main__':
    unittest.main()
