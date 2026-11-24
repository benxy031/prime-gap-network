"""Read-only progress for the actual speculative GPU ticket (no engine launch)."""
from copy import deepcopy
import json
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from horizon_engine_host import ticket_configuration
from horizon_gpu_queue import GpuJob
from horizon_mining_flow import MiningFlow
from horizon_network_worker import NetworkWorker, ticket_frame
from pgn_native_contract import fields
from pgn_session import SessionStore


class SpeculativeProgressPresentation(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="pgn-speculative-progress-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.old = dict(ticket="a"*64, request_id="b"*64, ticket_number="300")
        self.work = dict(ticket="c"*64, request_id="d"*64, ticket_number="4547")
        self.regular = dict(folder=self.root/"previous-audit", request_id="e"*64)
        self.worker = object.__new__(NetworkWorker)
        self.worker.root = self.root
        self.worker.sequence = 23
        self.worker.current = deepcopy(self.old)
        self.worker.audits = SimpleNamespace(presentation=self.regular)
        self.worker.network_notice = ""
        self.frames = []
        self.worker.emit = self.frames.append
        self.worker.audit_frame = lambda *_args, **_kwargs: self.fail("previous audit was presented")
        self.flow = object.__new__(MiningFlow)
        self.flow.worker = self.worker
        old = self._session(self.old, 17)
        speculative = self._session(self.work, 4)
        self.flow.gpu = SimpleNamespace(condition=threading.Condition(), active=GpuJob(
            speculative,
            dict(role="search", work=deepcopy(self.work), work_cycle=self.work["request_id"],
                 prepared=True, extra_publication_overlap={"audit_id": "f"*64, "commitment": "1"*64}),
            maximum_piece=10**7, view_sequence=23,
        ))
        self.worker.speculative_progress_snapshot = getattr(
            self.flow, "speculative_progress_snapshot", lambda: None)
        self.guards = {
            self.root/"controller.json": dict(stage="ASSIGNED", work=self.old),
            self.root/"prepared-ticket"/"controller.json": dict(stage="ASSIGNED", work=self.work),
            self.root/"cycles.json": dict(cycles=[dict(stage="prepared", work=self.work)]),
        }
        for path, record in self.guards.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(json.dumps(record).encode())
        self.before = {path: path.read_bytes() for path in self.guards}
        self.assertEqual(old["ticket_id"], self.old["ticket"])

    def _session(self, work, done):
        start, end = 10**20, 10**20+20
        job = dict(schema="PGN_HORIZON_ENGINE_TICKET_V1", ticket_id=work["ticket"],
                   start=str(start), end=str(end), target_merit_q16=29*65536,
                   threshold_policy=fields(start, end)["threshold_policy"])
        store = SessionStore.create(self.root/"tickets"/work["ticket"],
                                    ticket_configuration(job), dict(uuid="PROGRESS-FIXTURE"))
        try:
            store.accepted_progress(done, 0)
            store.meta["state"] = "RUNNING"
            store.save()
        finally:
            store.close()
        return job

    def test_active_search_overrides_old_audit_and_official_ticket_without_promotion(self):
        self.worker._present_progress(dict(phase="SEARCHING", ticket_id=self.work["ticket"]))
        self.assertEqual(len(self.frames), 1)
        actual = self.frames[0]
        self.assertEqual((actual["ticket_id"], actual["request_id"], actual["title"],
                          actual["positions"], actual["work_role"], actual["phase"]),
                         (self.work["ticket"], self.work["request_id"], "Ticket 4547",
                          "4", "search", "Searching"))
        self.assertEqual(self.worker.current, self.old)
        self.assertIs(self.worker.audits.presentation, self.regular)
        self.assertEqual({path: path.read_bytes() for path in self.guards}, self.before)

    def test_exact_pause_is_visible_with_no_official_ticket(self):
        self.worker.current = None
        self.worker.audits.presentation = None
        self.worker._present_progress(dict(phase="PAUSED_ACK", ticket_id=self.work["ticket"]))
        self.assertEqual(len(self.frames), 1)
        self.assertEqual(self.frames[0]["ticket_id"], self.work["ticket"])
        self.assertEqual(self.frames[0]["phase"], "Paused")
        self.assertEqual(self.frames[0]["controller_state"], "off")
        self.assertIsNone(self.frames[0]["eta_rate"])

    def test_foreign_event_and_mismatched_cycle_do_not_fall_back(self):
        self.worker.audits.presentation = None
        self.worker._present_progress(dict(phase="SEARCHING", ticket_id=self.old["ticket"]))
        self.worker._present_progress(dict(phase="SEARCHING"))
        self.flow.gpu.active.context["work_cycle"] = "9"*64
        self.worker._present_progress(dict(phase="SEARCHING", ticket_id=self.work["ticket"]))
        self.assertEqual(self.frames, [])

    def test_revoked_identity_during_frame_read_is_not_emitted(self):
        self.worker.audits.presentation = None
        def read_then_revoke(*args):
            actual = ticket_frame(*args)
            self.flow.gpu.active = None
            return actual
        with patch("horizon_network_worker.ticket_frame", side_effect=read_then_revoke):
            self.worker._present_progress(dict(phase="SEARCHING", ticket_id=self.work["ticket"]))
        self.assertEqual(self.frames, [])

    def test_revoked_identity_during_a_missing_frame_cannot_send_old_pause(self):
        self.worker.audits.presentation = None
        def revoke_then_raise(*_args):
            self.flow.gpu.active = None
            raise FileNotFoundError("speculative checkpoint changed")
        with patch("horizon_network_worker.ticket_frame", side_effect=revoke_then_raise):
            self.worker._present_progress(dict(phase="PAUSED_ACK", ticket_id=self.work["ticket"]))
        self.assertEqual(self.frames, [])

    def test_unmarked_job_preserves_the_normal_worker_path(self):
        self.worker.audits.presentation = None
        self.flow.gpu.active.context.pop("extra_publication_overlap")
        self.worker._present_progress(dict(phase="SEARCHING", ticket_id=self.old["ticket"]))
        self.assertEqual((self.frames[0]["ticket_id"], self.frames[0]["positions"]),
                         (self.old["ticket"], "17"))


if __name__ == "__main__":
    unittest.main()
