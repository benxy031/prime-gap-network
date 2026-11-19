from pathlib import Path
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src/engine/runtime_host'))
from horizon_diagnostics import export
from horizon_diagnostics_install import inspect_installations


class DiagnosticInstallationsTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'simulated-local-user'
        self.store = self.root / 'updates/installation'
        self.runtime = self.root / 'runtime'
        self.active = self.root / 'portable-new'
        self.previous = self.root / 'portable-old'
        self.selected = self.store / 'cohorts' / ('1' * 20)
        self.other = self.store / 'cohorts' / ('2' * 20)
        self.ext = '.exe' if os.name == 'nt' else ''
        for path in (self.runtime, self.selected, self.other, self.active, self.previous):
            path.mkdir(parents=True, exist_ok=True)
        (self.store / '.owner').write_bytes(b'PGN_COMPONENT_INSTALL_STORE_V1\n')
        self.locations = {
            'program': str(self.active), 'installation': str(self.store),
            'core_runtime': str(self.runtime), 'application_data': str(self.root),
        }
        selected_digest = self.inventory(self.selected, '2026.11.17.176')
        self.inventory(self.active, '2026.11.17.177')
        self.inventory(self.other, '2026.11.15.160')
        self.pointer = dict(schema='PGN_ACTIVE_COHORT_V1', cohort=str(self.selected), sequence=42,
                            inventory_sha256=selected_digest, previous={'cohort': str(self.previous)})
        self.write_json(self.store / 'current.json', self.pointer)

    def write_json(self, path, value):
        raw = json.dumps(value).encode()
        path.write_bytes(raw)
        return hashlib.sha256(raw).hexdigest()

    def inventory(self, directory, version):
        return self.write_json(directory / 'components.json', {
            'schema': 'PGN_COMPONENT_MANIFEST_V1',
            'components': [{'id': 'ui', 'version': version}],
        })

    def test_capture_after_startup_includes_earlier_stages_and_inventories(self):
        for root in (self.active, self.previous):
            (root / 'Core').mkdir()
            (root / 'Core' / ('pgn-core' + self.ext)).write_bytes(b'inert diagnostic fixture')
        wallet_directory = self.root / 'wallets'
        wallet_directory.mkdir()
        (wallet_directory / 'wallet.dat').write_bytes(b'INERT WALLET MUST NOT ENTER DIAGNOSTICS')
        self.write_json(self.runtime / 'wallet-core.json', dict(
            executable=str(self.active / 'Core' / ('pgn-core' + self.ext)),
            data_directory=str(self.root), rpc_port=111, wallet='INERT PRIVATE LABEL',
        ))
        self.write_json(self.runtime / 'wallet-owned.json', dict(
            schema='pgn-core-owned-2', executable=str(self.previous / 'Core' / ('pgn-core' + self.ext)),
            data_directory=str(self.root), rpc_port=222, pid='7',
        ))
        self.write_json(self.store / 'transaction.json', dict(
            schema='PGN_COHORT_ACTIVATION_V1', state='shutdown_failed',
        ))
        raw = json.dumps(dict(schema='PGN_DIAGNOSTIC_CAPTURE_V1', samples=[{'network': {'height': 4245}}],
                              context=dict(startup=[{'stage': 1, 'elapsed_ms': 0}],
                                           observed_core_pid=7, local_paths=self.locations))).encode()
        payload = export(raw, self.locations)
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            result = archive.read('diagnostics.json')
        self.assertNotIn(b'simulated-local-user', result)
        self.assertNotIn(b'INERT', result)
        context = json.loads(result)['context']
        self.assertEqual(context['startup'], [{'stage': 1, 'elapsed_ms': 0}])
        install = context['installation']
        self.assertFalse(install['selected_program']['matches_running_program'])
        self.assertTrue(install['selected_program']['inventory_matches_selection'])
        self.assertFalse(install['previous_program']['inside_managed_store'])
        self.assertEqual(install['previous_program']['core_file'], 'file')
        self.assertEqual(install['activation_state'], 'shutdown_failed')
        self.assertEqual(len(install['retained_programs']), 2)
        self.assertTrue(install['core']['observed_process_matches_owner'])
        self.assertFalse(install['core']['configured_program_matches_owner'])
        self.assertFalse(install['core']['configured_port_matches_owner'])
        self.assertIn('temporary', install['free_mib'])

    def test_export_distinguishes_core_storage_and_unfinished_startup(self):
        core_data = self.root / 'core-data-on-other-volume'
        core_data.mkdir()
        self.write_json(self.runtime / 'wallet-core.json', {'data_directory': str(core_data)})
        (self.runtime / 'wallet-launch-intent.json').write_bytes(b'INERT LAUNCH CONTENT')
        (self.runtime / 'wallet-core-rebind.json').write_bytes(b'INERT REBIND CONTENT')
        raw = json.dumps(dict(schema='PGN_DIAGNOSTIC_CAPTURE_V1', samples=[{}], context={})).encode()

        real_disk_usage = shutil.disk_usage

        def probe_volume(directory):
            if directory == core_data:
                return real_disk_usage(directory)._replace(free=12 * 1024 * 1024)
            return real_disk_usage(directory)

        with patch('horizon_diagnostics_install.shutil.disk_usage', side_effect=probe_volume):
            payload = export(raw, self.locations)
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            data = archive.read('diagnostics.json')
        self.assertNotIn(b'core-data-on-other-volume', data)
        self.assertNotIn(b'INERT', data)
        result = json.loads(data)['context']['installation']
        self.assertEqual(result['free_mib']['core_data'], 12)
        self.assertIn('core_runtime', result['free_mib'])
        self.assertEqual(result['core']['startup_transactions'], {
            'pending_setup': 'missing', 'rebind': 'file', 'launch_intent': 'file',
        })

    def test_portable_baseline_with_a_removed_folder_is_distinguishable(self):
        self.pointer.update(cohort=str(self.root / 'removed'), previous=None)
        self.write_json(self.store / 'current.json', self.pointer)
        snap = inspect_installations(self.locations, 0)
        self.assertTrue(snap['portable_baseline'])
        self.assertEqual(snap['selected_program']['state'], 'missing')
        self.assertFalse(snap['selected_program']['inside_managed_store'])
        self.assertEqual(snap['running_program']['state'], 'directory')

    def test_scope_limits_and_linked_cohorts(self):
        for number in range(130):
            directory = self.store / 'cohorts' / format(number + 10, '020x')
            directory.mkdir()
        result = inspect_installations(self.locations, 0)
        self.assertTrue(result['retained_list_limited'])
        self.assertLessEqual(len(result['retained_programs']), 128)
        link = self.root / 'old-link'
        try:
            link.symlink_to(self.active, target_is_directory=True)
        except OSError:
            self.skipTest('This Windows account cannot create disposable symbolic links')
        self.pointer['previous'] = {'cohort': str(link)}
        self.write_json(self.store / 'current.json', self.pointer)
        self.assertEqual(inspect_installations(self.locations, 0)['previous_program']['state'], 'linked')

    def test_malformed_old_selector_does_not_abort_the_capture(self):
        (self.store / 'current.json').write_bytes(b'[' * 10000 + b']' * 10000)
        raw = json.dumps(dict(schema='PGN_DIAGNOSTIC_CAPTURE_V1', samples=[{}], context={})).encode()
        with zipfile.ZipFile(io.BytesIO(export(raw, self.locations))) as archive:
            captured = json.loads(archive.read('diagnostics.json'))
        install = captured['context']['installation']
        self.assertEqual(install['selection_file'], 'file')
        self.assertEqual(install['selection_document'], 'unreadable')
        self.assertNotIn('selected_program', install)
        self.assertEqual(install['running_program']['versions']['ui'], '2026.11.17.177')

    def test_old_capture_and_unrecognized_local_context_still_export(self):
        for extra in ({}, {'context': {'local_paths': {'program': str(self.active)}}}):
            raw = json.dumps(dict(schema='PGN_DIAGNOSTIC_CAPTURE_V1', samples=[{}], **extra)).encode()
            with zipfile.ZipFile(io.BytesIO(export(raw))) as archive:
                self.assertNotIn(b'simulated-local-user', archive.read('diagnostics.json'))


if __name__ == '__main__':
    unittest.main()
