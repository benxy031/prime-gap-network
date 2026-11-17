"""Bounded, read-only clues about installations known to the running app."""
from functools import lru_cache
from pathlib import Path
from itertools import islice
import ctypes
import hashlib
import json
import os
import re
import shutil
import stat
import tempfile

_STORE_OWNER = b'PGN_COMPONENT_INSTALL_STORE_V1\n'
_COHORT = re.compile(r'[0-9a-f]{20}')
_VERSION = re.compile(r'[0-9]{1,10}(?:\.[0-9]{1,10}){1,5}')
_LIMIT = 128


def _local_path(raw):
    if not isinstance(raw, str) or not raw or len(raw) > 2048 or '\x00' in raw:
        return None
    path = Path(raw)
    if not path.is_absolute() or '..' in path.parts or len(path.parts) > 128:
        return None
    if os.name == 'nt':
        if not re.fullmatch(r'[a-zA-Z]:', path.drive):
            return None
        if ctypes.windll.kernel32.GetDriveTypeW(ctypes.c_wchar_p(path.anchor)) not in (3, 6):
            return None
    return path


def _same(first, second):
    return first is not None and second is not None and os.path.normcase(str(first)) == os.path.normcase(str(second))


def _tag(path):
    if path is None:
        return None
    normalized = os.path.normcase(str(path)).encode('utf-8', 'surrogatepass')
    return hashlib.sha256(normalized).hexdigest()[:20]


def inspect_installations(locations, observed_pid):
    if not isinstance(locations, dict):
        return {'available': False}
    app, store, runtime, data = [
        _local_path(locations.get(name))
        for name in ('program', 'installation', 'core_runtime', 'application_data')
    ]

    @lru_cache(maxsize=1024)
    def status(path):
        if path is None:
            return 'unavailable'
        if path != path.parent:
            parent = status(path.parent)
            if parent != 'directory':
                return parent if parent != 'file' else 'unavailable'
        try:
            info = path.lstat()
        except FileNotFoundError:
            return 'missing'
        except OSError:
            return 'unavailable'
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 1024:
            return 'linked'
        if stat.S_ISDIR(info.st_mode):
            return 'directory'
        return 'file' if stat.S_ISREG(info.st_mode) else 'unavailable'

    def bounded(path, limit):
        if status(path) != 'file':
            return b''
        with path.open('rb') as stream:
            raw = stream.read(limit + 1)
        return raw if len(raw) <= limit else b''

    def document(path):
        try:
            raw = bounded(path, 128 * 1024)
            value = json.loads(raw)
        except (OSError, ValueError):
            return {}, None
        return (value, hashlib.sha256(raw).hexdigest()) if isinstance(value, dict) else ({}, None)

    def inspect_program(directory):
        snapshot = {'id': _tag(directory), 'state': status(directory)}
        if directory is not None:
            snapshot['path_length'] = len(str(directory))
        if snapshot['state'] != 'directory':
            return snapshot
        manifest, digest = document(directory / 'components.json')
        components = manifest.get('components')
        if manifest.get('schema') == 'PGN_COMPONENT_MANIFEST_V1' and isinstance(components, list) and len(components) <= 4:
            snapshot['inventory_hash'] = digest
            snapshot['versions'] = {
                row['id']: row['version']
                for row in components if isinstance(row, dict)
                and row.get('id') in ('ui', 'core', 'cpu', 'gpu')
                and isinstance(row.get('version'), str) and _VERSION.fullmatch(row['version'])
            }
        ext = '.exe' if os.name == 'nt' else ''
        snapshot['interface_file'] = status(directory / ('PGN-Horizon' + ext))
        snapshot['core_file'] = status(directory / 'Core' / ('pgn-core' + ext))
        snapshot['updater_file'] = status(directory / 'UpdateRuntime' / ('horizon-updater' + ext))
        return snapshot

    def selector_program(reference):
        if not isinstance(reference, dict):
            return {}
        directory = _local_path(reference.get('cohort'))
        result = inspect_program(directory)
        result['matches_running_program'] = _same(app, directory)
        if directory is not None and store is not None:
            result['inside_managed_store'] = _same(directory.parent, store / 'cohorts')
        if result.get('inventory_hash'):
            result['inventory_matches_selection'] = result['inventory_hash'] == reference.get('inventory_sha256')
        if type(reference.get('sequence')) is int:
            result['sequence'] = reference['sequence']
        return result

    records = {'available': True, 'running_program': inspect_program(app),
               'installation_state': status(store), 'retained_programs': []}
    try:
        owned = store is not None and bounded(store / '.owner', 128) == _STORE_OWNER
    except OSError:
        owned = False
    records['owned_store'] = owned
    if owned:
        pointer, _ = document(store / 'current.json')
        records['selection_file'] = status(store / 'current.json')
        if pointer.get('schema') == 'PGN_ACTIVE_COHORT_V1':
            records['portable_baseline'] = 'previous' in pointer and pointer['previous'] is None
            records['selected_program'] = selector_program(pointer)
            records['previous_program'] = selector_program(pointer.get('previous'))
        cohorts = store / 'cohorts'
        if status(cohorts) == 'directory':
            try:
                with os.scandir(cohorts) as children:
                    entries = list(islice(children, _LIMIT + 1))
                records['retained_list_limited'] = len(entries) > _LIMIT
                records['retained_programs'] = [
                    inspect_program(cohorts / item.name) for item in sorted(entries[:_LIMIT], key=lambda e: e.name)
                    if _COHORT.fullmatch(item.name)
                ]
            except OSError:
                records['retained_list_unavailable'] = True
        transaction, _ = document(store / 'transaction.json')
        if transaction.get('schema') == 'PGN_COHORT_ACTIVATION_V1':
            stage = transaction.get('state')
            if stage in ('starting_candidate', 'committed', 'shutdown_failed', 'shutdown_refused',
                         'startup_failed', 'activation_failed', 'recovered_previous'):
                records['activation_state'] = stage

    if runtime is not None:
        profile, _ = document(runtime / 'wallet-core.json')
        owner, _ = document(runtime / 'wallet-owned.json')
        configured = _local_path(profile.get('executable'))
        running = _local_path(owner.get('executable'))
        core_data = _local_path(profile.get('data_directory'))
        known_owner = owner.get('schema') in ('pgn-core-owned-1', 'pgn-core-owned-2', 'pgn-core-owned-3')
        bundle = (app / 'Core' / ('pgn-core.exe' if os.name == 'nt' else 'pgn-core')) if app is not None else None
        records['core'] = {
            'configured_program': _tag(configured), 'configured_file': status(configured),
            'owned_program': _tag(running), 'owned_file': status(running),
            'ownership_record_present': known_owner,
            'configured_program_matches_owner': _same(configured, running),
            'bundle_program_matches_owner': _same(bundle, running),
            'configured_data_matches_owner': _same(core_data, _local_path(owner.get('data_directory'))),
            'configured_port_matches_owner': type(profile.get('rpc_port')) is int and profile['rpc_port'] == owner.get('rpc_port'),
            'observed_process_matches_owner': type(observed_pid) is int and observed_pid > 0 and str(observed_pid) == owner.get('pid'),
            'configured_data_state': status(core_data),
        }
    records['free_mib'] = {}
    update_cache = store.parent / 'cache' if store is not None else None
    volumes = (('program', app), ('installation', store), ('update_cache', update_cache),
               ('application_data', data), ('temporary', _local_path(tempfile.gettempdir())))
    for name, root in volumes:
        if status(root) == 'directory':
            try:
                records['free_mib'][name] = shutil.disk_usage(root).free // 1048576
            except OSError:
                pass
    return records
