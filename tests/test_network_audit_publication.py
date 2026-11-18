"""Real publication journal with controlled RPC transitions; no network or GPU work."""
from pathlib import Path
from types import SimpleNamespace
import copy
import json
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src/protocol'))
sys.path.insert(0, str(ROOT / 'src/engine/runtime_host'))
sys.path.insert(0, str(ROOT / 'tests'))
from horizon_network_audits import NetworkAudits, save
from audit_publication_chain_fixture import ChainRPC
from report_commitment import digest


class AuditPublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.worker = SimpleNamespace(root=self.root,rpc=ChainRPC())
        self.audits = NetworkAudits(self.worker)
        self.request = 'a' * 64
        self.name = self.request + '-0-0'
        body = dict(counts=['7'], nonce='b' * 64)
        self.answer = dict(request_id=self.request, slot=0, attempt=0, actor='0014' + '42'*20,
                           answer=dict(body=body, commitment=digest(body)))
        self.worker.rpc.audit_job.update(actor=self.answer['actor'], commitment=digest(body))
        save(self.audits.folder / (self.name + '.json'), self.answer)
        save(self.audits.folder / 'pending.json', [self.name])
        self.committed = dict(details_root='c' * 64,
                              jobs=[dict(self.worker.rpc.audit_job, state='committed')])
        self.before = copy.deepcopy(self.committed)
        self.after = copy.deepcopy(self.committed)
        self.calls = []
        self.available = True
        self.error = None
        self.audits.states = lambda requests: {self.request: copy.deepcopy(self.before)}
        self.audits.state = lambda request: copy.deepcopy(self.after)
        def apply(*args):
            self.calls.append(args)
            if self.error:
                raise self.error
            return dict(available=self.available)
        self.audits.apply = apply

    def pending(self):
        return json.loads((self.audits.folder / 'pending.json').read_bytes())

    def test_unavailable_publication_retains_answer_across_restart(self):
        self.available = False
        original = (self.audits.folder / (self.name + '.json')).read_bytes()
        self.audits.reveal_committed()
        self.assertEqual(self.pending(), [self.name])
        reloaded = NetworkAudits(self.worker)
        self.assertEqual((reloaded.folder / (self.name + '.json')).read_bytes(), original)
        self.assertEqual(len(self.calls), 1)

    def test_submitted_but_not_observed_terminal_stays_pending(self):
        self.audits.reveal_committed()
        self.assertEqual(self.pending(), [self.name])
        self.before['jobs'][0]['state'] = 'passed'
        self.audits.reveal_committed()
        self.assertEqual(self.pending(), [])
        self.assertEqual(len(self.calls), 1)

    def test_confirmed_pass_or_failure_acknowledges_exact_attempt(self):
        for verdict in ('passed', 'failed'):
            with self.subTest(verdict=verdict):
                save(self.audits.folder / 'pending.json', [self.name])
                self.before = copy.deepcopy(self.committed)
                self.after = copy.deepcopy(self.committed)
                self.after['jobs'][0]['state'] = verdict
                self.audits.reveal_committed()
                self.assertEqual(self.pending(), [])
                self.assertTrue((self.audits.folder / (self.name + '.json')).is_file())

    def test_reorganization_during_publication_retains_answer(self):
        self.after = {}
        self.audits.reveal_committed()
        self.assertEqual(self.pending(), [self.name])

    def test_another_attempt_verdict_cannot_acknowledge_this_answer(self):
        self.after['jobs'][0].update(attempt=1, state='passed')
        self.audits.reveal_committed()
        self.assertEqual(self.pending(), [self.name])

    def test_unverified_named_verdict_keeps_exact_seal_until_correct_confirmation(self):
        path = self.audits.folder / (self.name + '.json')
        original = path.read_bytes()
        expected = dict(self.worker.rpc.audit_job, state='failed')
        self.before = dict(self.committed, jobs=[expected])
        for field, value in [('commitment', None), ('commitment', 'd'*64), ('actor', '0014'+'43'*20)]:
            with self.subTest(field=field, value=value):
                self.worker.rpc.audit_job = dict(expected, **{field:value})
                self.audits.reveal_committed()
                self.assertEqual(self.pending(), [self.name])
                self.assertEqual(self.audits.answers.recover([]), [self.name])
                self.assertEqual(path.read_bytes(), original)
        self.worker.rpc.audit_job = expected
        self.audits.reveal_committed()
        self.assertEqual(self.pending(), [])
        self.assertEqual(self.audits.answers.recover([]), [])
        self.assertEqual(path.read_bytes(), original)

    def test_transport_failure_preserves_index_and_payload(self):
        original = (self.audits.folder / 'pending.json').read_bytes()
        self.error = ConnectionError('Injected connection loss')
        with self.assertRaises(ConnectionError):
            self.audits.reveal_committed()
        self.assertEqual((self.audits.folder / 'pending.json').read_bytes(), original)
        self.assertEqual(json.loads((self.audits.folder / (self.name + '.json')).read_bytes()), self.answer)

    def test_failed_post_publication_read_does_not_acknowledge(self):
        def failed_read(request):
            raise ConnectionError('Injected post-publication connection loss')
        self.audits.state = failed_read
        with self.assertRaises(ConnectionError):
            self.audits.reveal_committed()
        self.assertEqual(self.pending(), [self.name])


if __name__ == '__main__':
    unittest.main()
