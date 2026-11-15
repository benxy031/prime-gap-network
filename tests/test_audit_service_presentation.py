from pathlib import Path
import sys,unittest,copy,threading
from types import SimpleNamespace
from collections import deque
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'src/protocol'))
sys.path.insert(0,str(ROOT/'src/engine/runtime_host'))
from horizon_service_presentation import preparation_view
from horizon_mining_flow import MiningFlow
class PreparationTests(unittest.TestCase):
    def context(self):
        return dict(role='audit',presentation=dict(audit_id='a'*64),
                    audit_request_id='b'*64,audit_slot=0,audit_attempt=1)
    def status(self,credits=()):
        return dict(schema='PGN_NATIVE_WORK_DUTY_STATUS_V1',enforced=True,
                    bestblockhash='c'*64,may_request=False,credits=list(credits))
    def credit(self,current=False):
        return dict(request_id=('b' if current else 'd')*64,slot=0,attempt=1,commitment='e'*64)
    def test_first_actual_audit_does_not_complete_either_slot(self):
        v=preparation_view(self.context(),self.status())
        self.assertEqual((v['confirmed_service_answers'],v['active_index']),(0,0))
        self.assertFalse(v['search_assigned'])
    def test_second_audit_uses_only_core_confirmed_first_answer(self):
        v=preparation_view(self.context(),self.status([self.credit()]))
        self.assertEqual((v['confirmed_service_answers'],v['active_index']),(1,1))
    def test_answer_already_recorded_is_not_counted_again(self):
        v=preparation_view(self.context(),self.status([self.credit(current=True)]))
        self.assertEqual((v['confirmed_service_answers'],v['active_index']),(1,0))
    def test_missing_observation_is_unknown(self):
        v=preparation_view(self.context(),None)
        self.assertIsNone(v['confirmed_service_answers']);self.assertIsNone(v['active_index'])
    def test_bad_or_duplicate_observation_does_not_fabricate_completion(self):
        rows=[self.status([self.credit(),self.credit()]),self.status([dict(self.credit(),slot=True)]),
              self.status([dict(self.credit(),commitment='0'*64)]),dict(self.status(),enforced=False),
              dict(self.status(),may_request=0),dict(self.status(),bestblockhash='bad')]
        for row in rows:
            with self.subTest(row=row):self.assertIsNone(preparation_view(self.context(),row)['confirmed_service_answers'])
    def test_third_unbound_audit_cannot_be_presented_as_completed_pair(self):
        other=dict(self.credit(),request_id='f'*64)
        self.assertIsNone(preparation_view(self.context(),self.status([self.credit(),other]))['confirmed_service_answers'])
    def test_bound_or_search_jobs_never_reuse_preparation_view(self):
        for context in [dict(self.context(),role='search'),dict(self.context(),work_cycle='1'*64),
                        dict(self.context(),audit_budget_cycle='1'*64)]:
            self.assertIsNone(preparation_view(context,self.status()))
    def test_wrong_active_identity_is_rejected(self):
        c=self.context();c['presentation']['audit_id']='0'*64
        self.assertIsNone(preparation_view(c,self.status()))
    def test_view_is_read_only(self):
        c,s=self.context(),self.status([self.credit()]);before=copy.deepcopy((c,s))
        preparation_view(c,s);self.assertEqual((c,s),before)


class ActualFlowPresentationTests(unittest.TestCase):
    context=PreparationTests.context
    status=PreparationTests.status
    credit=PreparationTests.credit
    def flow(self):
        flow=object.__new__(MiningFlow)
        flow.gpu=SimpleNamespace(condition=threading.Condition(),active=SimpleNamespace(context=self.context()),search=deque(),audit_service_view=lambda:dict(mode='two_regular_one_catchup',active_role='audit'))
        flow.prefetch=SimpleNamespace(service_observation=lambda:self.status([self.credit()]))
        return flow
    def test_actual_snapshot_reads_confirmed_observation_without_network_or_mutation(self):
        flow=self.flow();before=copy.deepcopy(flow.gpu.active.context)
        view=flow.audit_service_snapshot()
        self.assertNotIn('cycle',view)
        self.assertEqual(view['preparation']['confirmed_service_answers'],1)
        self.assertEqual(view['preparation']['active_index'],1)
        self.assertEqual(flow.gpu.active.context,before)
    def test_snapshot_without_cached_observation_is_explicitly_unknown(self):
        flow=self.flow();flow.prefetch=SimpleNamespace()
        self.assertIsNone(flow.audit_service_snapshot()['preparation']['confirmed_service_answers'])
    def test_actual_bound_cycle_never_inherits_preparation(self):
        flow=self.flow();flow.gpu.active.context=dict(role='search',work_cycle='f'*64)
        state=dict(work=dict(ticket_number='42',length='200000000000000'),audits=[dict(stage='committed')]*2)
        flow.cycles=SimpleNamespace(snapshot=lambda request:copy.deepcopy(state));flow.cycle_audit_positions={}
        def forbidden():raise AssertionError('Assigned search must not inspect pre-ticket service')
        flow.prefetch.service_observation=forbidden
        view=flow.audit_service_snapshot()
        self.assertEqual(view['cycle']['ticket_number'],'42');self.assertNotIn('preparation',view)

if __name__=='__main__':unittest.main()
