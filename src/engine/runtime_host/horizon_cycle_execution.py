"""Fresh physical audit service per issued GPU cycle, separate from spent credits.

Native receipts authorize a request; they are never rewritten here. They may
refer to a computation already counted as an earlier cycle's extra. A new
cycle therefore executes fresh assigned work rather than counting those
receipts a second time. Only four live cycle records are retained; no answers,
wallet material, all-time history or new network authority is stored here.
"""
import copy
import json
from pathlib import Path
import threading
from horizon_work_cycle import identity
from pgn_session import atomic_bytes
from report_commitment import canonical_bytes


class CycleExecution:
    schema = 'PGN_PHYSICAL_AUDIT_CYCLES_V1'

    def __init__(self, folder, genesis):
        self.path = Path(folder) / 'physical-audits.json'
        self.genesis = genesis
        self.lock = threading.RLock()
        self.cycles = {}
        self.extra_path = Path(folder) / 'physical-extra-audits.json'
        self.extras = {}
        self.native_path = Path(folder) / 'physical-native-cycles.json'
        self.native = {}
        if self.native_path.exists():
            if self.native_path.stat().st_size > 4096:
                raise ValueError('Invalid native physical cycle journal')
            value = json.loads(self.native_path.read_bytes())
            if (set(value) != {'schema', 'genesis', 'cycles'}
                    or value['schema'] != 'PGN_NATIVE_PHYSICAL_CYCLES_V1'
                    or value['genesis'] != genesis or type(value['cycles']) is not dict
                    or len(value['cycles']) > 5
                    or any(not identity(key) or type(record) is not dict
                           or set(record) - {'empty', 'commitments'}
                           or ('empty' in record and not identity(record['empty']))
                           or type(record.get('commitments', {})) is not dict
                           or len(record.get('commitments', {})) > 3
                           or any(not identity(audit) or not identity(commitment)
                                  for audit, commitment in record.get('commitments', {}).items())
                           for key, record in value['cycles'].items())):
                raise ValueError('Invalid native physical cycle journal')
            self.native = value['cycles']
        if self.extra_path.exists():
            if self.extra_path.stat().st_size > 32768:raise ValueError('Invalid physical-extra journal; retained unchanged')
            extra = json.loads(self.extra_path.read_bytes())
            if set(extra) != {'schema','genesis','cycles'} or extra['schema'] != 'PGN_PHYSICAL_EXTRA_COMPLETIONS_V1' or extra['genesis'] != genesis:raise ValueError('Physical-extra journal belongs to another network')
            self.validate_extras(extra['cycles']);self.extras = extra['cycles']
        if self.path.exists():
            if self.path.stat().st_size > 32768:
                raise ValueError('Physical audit journal is malformed; retained unchanged')
            value = json.loads(self.path.read_bytes())
            if set(value) != {'schema', 'genesis', 'cycles'} or value['schema'] != self.schema or value['genesis'] != genesis:
                raise ValueError('Physical audit journal belongs to another network')
            self.validate(value['cycles'])
            self.cycles = value['cycles']

    @staticmethod
    def validate_extras(cycles):
        if type(cycles) is not dict or len(cycles) > 4:raise ValueError('Invalid extra completion set')
        for cycle, records in cycles.items():
            if not identity(cycle) or type(records) is not dict or len(records) > 32:raise ValueError('Invalid extra cycle identity')
            for audit, block in records.items():
                if not identity(audit) or not identity(block):raise ValueError('Extra completion requires exact audit and block identities')

    def save_extras(self, candidate):
        self.validate_extras(candidate)
        if candidate == self.extras:return
        raw=canonical_bytes(dict(schema='PGN_PHYSICAL_EXTRA_COMPLETIONS_V1',genesis=self.genesis,cycles=candidate))+b'\n'
        if len(raw)>32768:raise ValueError('Invalid oversized extra completion set')
        atomic_bytes(self.extra_path,raw);self.extras=candidate

    def completed_extra(self, cycle, audit, block):
        with self.lock:
            candidate=copy.deepcopy(self.extras);candidate.setdefault(cycle,{})[audit]=block;self.save_extras(candidate)

    def reconcile_extra_completion(self, cycle, audit, block, hot):
        """Refresh this derived live view; authoritative receipts stay archived.

        The caller holds WorkCycleJournal.lock while taking hot and calling us.
        This prevents a late callback from reinstating a retired cycle, and
        serializes retirement with completion rather than increasing a limit.
        """
        hot = set(hot)
        if len(hot) > 4 or any(not identity(key) for key in hot):
            raise ValueError('Invalid authoritative live cycle set')
        if not all(identity(value) for value in (cycle, audit, block)):
            raise ValueError('Extra completion requires exact cycle/audit/block identities')
        with self.lock:
            candidate = {key: copy.deepcopy(value) for key, value in self.extras.items() if key in hot}
            if cycle in hot:
                candidate.setdefault(cycle, {})[audit] = block
            self.save_extras(candidate)
            return cycle in hot

    def completed_extras(self, cycle):
        with self.lock:return list(self.extras.get(cycle,{}))

    @staticmethod
    def validate(cycles):
        if type(cycles) is not dict or len(cycles) > 4:
            raise ValueError('Invalid active physical cycle set')
        seen = set()
        for cycle, tasks in cycles.items():
            if not identity(cycle) or type(tasks) is not list or len(tasks) != 2:
                raise ValueError('Two physical audit slots required')
            for task in tasks:
                if task is None:
                    continue
                if type(task) is not dict or task.get('stage') not in ('assigned', 'committed', 'unavailable'):
                    raise ValueError('Invalid physical audit state')
                if task['stage'] == 'unavailable':
                    if set(task) != {'stage', 'anchor', 'reason'} or not identity(task['anchor']) or task['reason'] != 'no_eligible_independent_work':
                        raise ValueError('Physical service waiver requires an exact Core availability anchor')
                    continue
                keys = {'stage', 'request', 'slot', 'attempt'} | ({'block'} if task['stage'] == 'committed' else set())
                if set(task) != keys or not identity(task['request']) or type(task['slot']) is not int or task['slot'] not in (0, 1) or type(task['attempt']) is not int or not 0 <= task['attempt'] < 2**32:
                    raise ValueError('Invalid physical audit assignment identity')
                if task['stage'] == 'committed' and not identity(task['block']):
                    raise ValueError('Physical service has no commit receipt')
                key = (task['request'], task['slot'], task['attempt'])
                if key in seen:
                    raise ValueError('A physical audit cannot fill two cycle slots')
                seen.add(key)

    def update(self, mutation):
        with self.lock:
            candidate = copy.deepcopy(self.cycles)
            answer = mutation(candidate)
            self.validate(candidate)
            raw = canonical_bytes(dict(schema=self.schema, genesis=self.genesis, cycles=candidate)) + b'\n'
            if len(raw) > 32768:
                raise ValueError('Invalid oversized physical audit journal')
            if candidate != self.cycles:
                atomic_bytes(self.path, raw)
                self.cycles = candidate
            return answer

    def prepare(self, cycle, hot):
        def change(records):
            records.update({cycle: records.get(cycle, [None, None])})
            for key in list(records):
                if key not in hot:
                    del records[key]  # Old cycle is already archived by WorkCycleJournal.
        self.update(change)
        with self.lock:self.save_extras({key:value for key,value in self.extras.items() if key in hot})

    def mark_native(self, cycle, hot):
        with self.lock:
            candidate = {key: record for key, record in self.native.items() if key in hot}
            candidate.setdefault(cycle, {})
            self.save_native(candidate)

    def save_native(self, candidate):
        if candidate != self.native:
            raw = canonical_bytes(dict(schema='PGN_NATIVE_PHYSICAL_CYCLES_V1',
                                       genesis=self.genesis, cycles=candidate)) + b'\n'
            atomic_bytes(self.native_path, raw)
            self.native = candidate

    def is_native(self, cycle):
        with self.lock:
            return cycle in self.native

    def skip_catchup(self, cycle, anchor):
        if not identity(anchor):
            raise ValueError('Catch-up availability requires an exact Core anchor')
        with self.lock:
            if cycle in self.native and 'empty' not in self.native[cycle]:
                candidate = copy.deepcopy(self.native)
                candidate[cycle]['empty'] = anchor
                self.save_native(candidate)

    def catchup_ready(self, cycle):
        with self.lock:
            return bool(self.native.get(cycle, {}).get('empty') or self.extras.get(cycle))

    def waiver_anchors(self, cycle):
        with self.lock:
            anchors = {item['anchor'] for item in self.cycles.get(cycle, [])
                       if item and item['stage'] == 'unavailable'}
            empty = self.native.get(cycle, {}).get('empty')
            if empty:
                anchors.add(empty)
            return anchors

    def clear_waivers(self, cycle):
        def change(records):
            if cycle in records:
                records[cycle] = [None if item and item['stage'] == 'unavailable' else item
                                  for item in records[cycle]]
        with self.lock:
            self.update(change)
            if 'empty' in self.native.get(cycle, {}):
                candidate = copy.deepcopy(self.native)
                del candidate[cycle]['empty']
                self.save_native(candidate)

    def native_commitment(self, cycle, audit):
        with self.lock:
            return self.native.get(cycle, {}).get('commitments', {}).get(audit)

    def remember_native_commitment(self, cycle, audit, digest, live):
        if not identity(audit) or not identity(digest):
            return
        with self.lock:
            if cycle not in self.native:
                return
            candidate = copy.deepcopy(self.native)
            known = candidate[cycle].get('commitments', {})
            candidate[cycle]['commitments'] = {key: value for key, value in known.items() if key in live}
            candidate[cycle]['commitments'][audit] = digest
            self.save_native(candidate)

    def snapshot(self, cycle):
        with self.lock:
            return copy.deepcopy(self.cycles.get(cycle))

    def pending(self, position, hot):
        with self.lock:
            return next((c['work']['request_id'] for c in hot
                         if c['stage'] == 'prepared' and not c['hold']
                         and c['work']['request_id'] in self.cycles
                         and self.cycles[c['work']['request_id']][position] is None), None)

    @staticmethod
    def ref(audit):
        return dict(stage='assigned', request=audit['request_id'], slot=audit['slot'], attempt=audit['attempt'])

    def bound_audit(self, audit, position):
        """Recover a persisted exact binding when current.json missed its update."""
        if type(position) is not int or position not in (0, 1):
            raise ValueError('Physical audit position zero or one required')
        ref = self.ref(audit)
        with self.lock:
            matches = [cycle for cycle, tasks in self.cycles.items()
                       if tasks[position] is not None and all(
                           tasks[position].get(k) == ref[k] for k in ('request', 'slot', 'attempt'))]
        if len(matches) > 1:
            raise ValueError('Physical audit is bound to multiple cycles')
        return matches[0] if matches else None

    def bind(self, cycle, position, audit, *, replace_unavailable=False):
        task = self.ref(audit)
        def change(records):
            previous = records[cycle][position]
            if replace_unavailable and previous and previous['stage'] == 'unavailable':
                records[cycle][position] = task
                return
            if previous is not None and any(previous.get(k) != task[k] for k in ('request', 'slot', 'attempt')):
                raise ValueError('Physical cycle slot is already assigned')
            if previous is None:
                records[cycle][position] = task
        self.update(change)

    def committed(self, cycle, position, audit, block):
        def change(records):
            task = records[cycle][position]
            ref = self.ref(audit)
            if task is None or any(task.get(k) != ref[k] for k in ('request', 'slot', 'attempt')):
                raise ValueError('Physical commit does not match assigned computation')
            if task['stage'] == 'committed' and task['block'] != block:
                if audit.get('replay_previous_block') != task['block']:
                    raise ValueError('Physical audit commit changed without verified recovery')
            task.update(stage='committed', block=block)
        self.update(change)

    def retire(self, cycle, position, audit):
        def change(records):
            task = records[cycle][position]
            ref = self.ref(audit)
            if task is not None and all(task.get(k) == ref[k] for k in ('request', 'slot', 'attempt')):
                records[cycle][position] = None
        self.update(change)

    def unavailable(self, cycle, position, anchor):
        def change(records):
            if records[cycle][position] is None:
                records[cycle][position] = dict(stage='unavailable', anchor=anchor, reason='no_eligible_independent_work')
        self.update(change)

    def may_audit(self, cycle, position):
        tasks = self.snapshot(cycle)
        return bool(tasks and tasks[position] and tasks[position]['stage'] == 'assigned'
                    and all(t and t['stage'] in ('committed', 'unavailable') for t in tasks[:position]))

    def ready(self, cycle):
        tasks = self.snapshot(cycle)
        return tasks is None or all(t and t['stage'] in ('committed', 'unavailable') for t in tasks)
