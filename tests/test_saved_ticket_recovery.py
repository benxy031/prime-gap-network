"""Cold assignment recovery with typed current Core leases; no engine or RPC."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from threading import Event

sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'src/engine/runtime_host'),
              str(Path(__file__).resolve().parents[1]/'src/protocol')]
from horizon_network_worker import NetworkWorker, save
from horizon_core_rpc import CoreRpcError, CoreRpcUnavailable

GENESIS='7'*64
REQUEST='2'*64
TICKET='1'*64
ROOT='3'*64


class SourceRpc:
    def __init__(self,page):self.page=page
    def call(self,method,params):
        assert method=='getpgnstate' and params==[0,1,REQUEST]
        return copy.deepcopy(self.page)
    def verify(self):raise AssertionError('Recover-only may not request new network work')


class Audits:
    def __init__(self):self.calls=[]
    def track_owner_report(self,*args):self.calls.append(args)


class SavedTicketRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.work=dict(request_id=REQUEST,ticket=TICKET,start='100',length='20')
        self.lease=dict(request=REQUEST,state='assigned',start='100',end='120',issued=1,
                        deadline=10,deadline_clock='ledger_height',ticket_number='35')

    def worker(self,lease=None,**page):
        worker=NetworkWorker.__new__(NetworkWorker);worker.root=self.root
        worker.args=SimpleNamespace(genesis=GENESIS);worker.audits=Audits()
        worker.quitting=Event()
        worker.rpc=SourceRpc(dict(genesis=GENESIS,height=5,leases=[] if lease is None else [copy.deepcopy(lease)],**page))
        save(self.root/'controller.json',dict(stage='ASSIGNED',sequence=4,work=copy.deepcopy(self.work)))
        return worker

    def journal(self):return json.loads((self.root/'controller.json').read_bytes())

    def report(self):
        folder=self.root/'tickets'/TICKET;folder.mkdir(parents=True)
        save(folder/'network-report.json',dict(report_root=ROOT))

    def test_legacy_resume_keeps_sequence_and_global_number(self):
        value=self.worker(self.lease).assign(recover_only=True)
        self.assertEqual('35',value['ticket_number']);self.assertEqual(4,self.journal()['sequence'])

    def test_timed_resume_does_not_fall_back_to_block_height(self):
        lease=dict(self.lease,deadline_clock='median_chain_time',issued_time=1000,deadline_time=87400)
        del lease['deadline']
        worker=self.worker(lease,median_time_past=1001);worker.rpc.page['height']=1000000
        self.assertEqual('35',worker.assign(recover_only=True)['ticket_number'])

    def test_missing_issue_snapshot_range_or_genesis_refuses_without_rewriting(self):
        for mutation in ('issued','range','genesis','future_issue'):
            with self.subTest(mutation=mutation):
                lease=dict(self.lease)
                if mutation=='issued':del lease['issued']
                if mutation=='range':lease['end']='121'
                if mutation=='future_issue':lease['issued']=6
                worker=self.worker(lease)
                if mutation=='genesis':worker.rpc.page['genesis']='8'*64
                before=(self.root/'controller.json').read_bytes()
                with self.assertRaises(ValueError):worker.assign(recover_only=True)
                self.assertEqual(before,(self.root/'controller.json').read_bytes())

    def test_expired_still_advertised_lease_is_rejected(self):
        worker=self.worker(dict(self.lease,deadline=5))
        with self.assertRaises(ValueError):worker.assign(recover_only=True)

    def test_retry_reads_current_median_time_before_resuming_saved_lease(self):
        lease=dict(self.lease, deadline_clock='median_chain_time', issued_time=1000, deadline_time=87400)
        for current_time in (87399, 87400):
            with self.subTest(current_time=current_time):
                worker=self.worker(lease, median_time_past=current_time)
                worker.args.wait_for_network_sync=True
                worker.message=Mock()
                worker.quitting.wait=Mock(return_value=False)
                journal=self.root/'controller.json'
                original=journal.read_bytes()
                replies=[CoreRpcUnavailable(503), worker.rpc.page]
                with patch.object(worker.rpc, 'call', side_effect=replies) as rpc, patch(
                    'horizon_network_worker.wait_for_network', return_value=True
                ) as synchronized:
                    if current_time==87400:
                        with self.assertRaisesRegex(ValueError, 'Saved ticket no longer matches'):
                            worker.assign(recover_only=True)
                        self.assertEqual(journal.read_bytes(),original)
                    else:
                        resumed=worker.assign(recover_only=True)
                        self.assertEqual(resumed['ticket'],TICKET)
                        self.assertEqual(resumed['ticket_number'],'35')
                    self.assertEqual(rpc.call_count,2)
                    self.assertTrue(all(args.args==('getpgnstate',[0,1,REQUEST]) for args in rpc.call_args_list))
                    synchronized.assert_called_once_with(worker)
                    worker.message.assert_called_once()
                    self.assertEqual(worker.message.call_args.args[1],'Waiting for connection')

    def test_missing_state_leaves_original_ticket_unconfirmed(self):
        worker=self.worker(self.lease)
        original=(self.root/'controller.json').read_bytes()
        with patch.object(worker.rpc,'call',return_value=None) as rpc:
            with self.assertRaisesRegex(ValueError,'Core did not return a state'):
                worker.assign(recover_only=True)
        rpc.assert_called_once()
        self.assertEqual((self.root/'controller.json').read_bytes(),original)

    def test_saved_lease_read_refusal_is_terminal(self):
        worker=self.worker(self.lease)
        original=(self.root/'controller.json').read_bytes()
        with patch.object(worker.rpc,'call',side_effect=CoreRpcError(-5,'Invalid state')) as rpc:
            with self.assertRaises(CoreRpcError):
                worker.assign(recover_only=True)
        rpc.assert_called_once()
        self.assertEqual((self.root/'controller.json').read_bytes(),original)

    def test_shutdown_while_waiting_for_saved_lease_keeps_journal(self):
        worker=self.worker(self.lease)
        worker.message=Mock()
        def stop(_seconds):
            worker.quitting.set()
            return True
        worker.quitting.wait=stop
        original=(self.root/'controller.json').read_bytes()
        with patch.object(worker.rpc,'call',side_effect=TimeoutError('read timed out')) as rpc:
            self.assertIsNone(worker.assign(recover_only=True))
        rpc.assert_called_once()
        self.assertEqual((self.root/'controller.json').read_bytes(),original)

    def test_disappeared_lease_is_not_reused_and_local_work_is_retained(self):
        self.report();before=(self.root/'tickets'/TICKET/'network-report.json').read_bytes()
        self.assertIsNone(self.worker().assign(recover_only=True))
        self.assertEqual('LEASE_ENDED',self.journal()['stage'])
        self.assertTrue(self.journal()['local_work_retained'])
        self.assertEqual(before,(self.root/'tickets'/TICKET/'network-report.json').read_bytes())

    def test_submitted_report_is_preserved_and_handed_to_audit_tracking(self):
        self.report();worker=self.worker(dict(self.lease,state='submitted_pending_audits',report_root=ROOT))
        self.assertIsNone(worker.assign(recover_only=True))
        self.assertEqual(1,len(worker.audits.calls));self.assertEqual('REPORT_CONFIRMED',self.journal()['stage'])
        self.assertFalse(self.journal()['scientific_accepted'])

    def test_missing_or_different_confirmed_report_is_not_accepted(self):
        worker=self.worker(dict(self.lease,state='submitted_pending_audits',report_root=ROOT))
        with self.assertRaises(ValueError):worker.assign(recover_only=True)
        self.report();worker=self.worker(dict(self.lease,state='submitted_pending_audits',report_root='4'*64))
        with self.assertRaises(ValueError):worker.assign(recover_only=True)
        self.assertEqual([],worker.audits.calls)


if __name__=='__main__':unittest.main()
