"""Recover chain-assigned work after loss of its local scheduling journal.

This reader never selects a new assignment, invents an answer or waives duty.
One bounded coherent page is scanned per call; the cursor survives restarts.
"""
from http.client import HTTPException
from pathlib import Path
import hashlib
import json
from horizon_audit_answers import AuditAnswers, AuditSealBindingMismatch, AuditSealRecoveryRequired
from horizon_audit_recovery import RetiredAuditSealConflict, progress_path, recover_retired_progress
from horizon_core_rpc import CoreRpcError, CoreRpcUnavailable
from pgn_session import atomic_bytes
from report_commitment import canonical_bytes, audit_commitment


class AssignmentRecoveryRpcUnavailable(RuntimeError):
    """An optional assignment recovery read could not complete."""


class LegacyAuditSignerUnresolved(AuditSealBindingMismatch):
    """The original actor is missing from a retained legacy record."""


def digest(value):
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


def canonical_signing_script(value):
    return (isinstance(value, str) and 0 < len(value) <= 20000 and len(value) % 2 == 0
            and all(c in '0123456789abcdef' for c in value))


class AssignedAuditRecovery:
    def __init__(self, worker, active_root='service-audit-lanes'):
        if active_root not in ('service-audit-lanes', 'audit-lanes'):
            raise ValueError('Explicit active audit lane root required')
        self.worker = worker
        self.cursor = worker.root / 'assigned-audit-recovery.json'
        self.active_root = active_root

    def retain_legacy_answer(self, candidate):
        """Inactive journals are retained data, never reservations of a GPU lane."""
        other = 'audit-lanes' if self.active_root == 'service-audit-lanes' else 'service-audit-lanes'
        paths = [self.worker.root / 'audits/current.json']
        paths += [self.worker.root / other / str(i) / 'audits/current.json' for i in range(2)]
        recovered = None
        for path in paths:
            if not path.is_file():
                continue
            if path.stat().st_size > 262144:
                raise ValueError('Inactive audit journal exceeds budget')
            raw = path.read_bytes()
            value = json.loads(raw)
            if any(value.get(k) != candidate[k] for k in ('request_id', 'slot', 'attempt')):
                continue
            if any(value.get(k) != candidate[k] for k in ('ticket', 'report_root', 'ranges')):
                continue
            if value.get('actor') is None and (value.get('answer') or value.get('publication_intent')):
                raise LegacyAuditSignerUnresolved('Legacy audit seal retained while its original signer is unresolved')
            if value.get('actor', candidate['actor']) != candidate['actor']:
                continue
            counts = value.get('counts')
            if (not isinstance(counts, list) or len(counts) > 5
                    or any(type(n) is not int or not 0 <= n < 2**64 for n in counts)):
                raise ValueError('Invalid retained inactive audit counts')
            target = value.get('target_merit_q16')
            if counts or target is not None:
                # Preserve the original display classification, as in the
                # immutable partition archive. Today's Gold target is mutable;
                # the audit commits exact ranges/counts, not that target.
                if type(target) is not int or not 0 < target < 2**31:
                    raise ValueError('Invalid retained inactive audit classification')
            answer = value.get('answer')
            if answer is not None:
                nonce = answer.get('body', {}).get('nonce')
                if len(counts) != 5 or not digest(nonce):
                    raise ValueError('Incomplete retained inactive audit answer')
                expected = audit_commitment(candidate['ticket'], candidate['attempt'],
                    [r['index'] for r in candidate['ranges']], [str(n) for n in counts], nonce)
                if answer != expected:
                    raise ValueError('Retained inactive audit commitment differs from its counts')
            retained = {k: value[k] for k in ('counts', 'target_merit_q16', 'answer', 'publication_intent') if k in value}
            if 'publication_intent' in retained:
                from horizon_coordination import validate_intent
                validate_intent(retained['publication_intent'])
            if recovered is not None and recovered != retained:
                raise ValueError('Conflicting retained inactive audit answers')
            recovered = retained
            candidate = dict(candidate, **retained,
                legacy_answer_recovery=dict(source=path.relative_to(self.worker.root).as_posix(),
                                            sha256=hashlib.sha256(raw).hexdigest()))
        return candidate

    def claim(self, folder, *, verify_signer=False):
        worker = self.worker
        def read(operation, *args):
            try:
                return operation(*args)
            except (CoreRpcError, CoreRpcUnavailable, ConnectionError, TimeoutError, HTTPException) as error:
                if not verify_signer:
                    raise
                raise AssignmentRecoveryRpcUnavailable() from error

        with worker.network_lock:
            address = read(worker.address)
            identity = read(worker.rpc.call, 'validateaddress', [address])
            actor = identity.get('scriptPubKey')
            if identity.get('isvalid') is not True or not canonical_signing_script(actor):
                raise ValueError('Authenticated miner signing script required for audit recovery')
            offset = 0
            if self.cursor.is_file():
                if self.cursor.stat().st_size > 1024:
                    raise ValueError('Assigned audit recovery cursor exceeds budget')
                saved = json.loads(self.cursor.read_bytes())
                if not (isinstance(saved, dict) and digest(saved.get('genesis'))
                        and canonical_signing_script(saved.get('actor'))):
                    raise ValueError('Invalid assigned audit recovery cursor identity')
                previous_offset = saved.get('offset')
                if type(previous_offset) is not int or not 0 <= previous_offset < 2**63 - 100:
                    raise ValueError('Invalid assigned audit recovery cursor')
                # This cursor only locates a page for one chain and signer.
                # A different authenticated identity starts a new scan.
                if (saved['genesis'], saved['actor']) == (worker.args.genesis, actor):
                    offset = previous_offset
            view = read(worker.rpc.call, 'getpgnworkview', ['audits', offset, 100])
            state = view.get('state', {})
            reports, rows = view.get('reports'), view.get('audits')
            if (view.get('schema') != 'PGN_WORK_VIEW_V1' or view.get('kind') != 'audits'
                    or view.get('offset') != offset or view.get('limit') != 100
                    or state.get('genesis') != worker.args.genesis
                    or not digest(state.get('bestblockhash'))
                    or type(state.get('height')) is not int or state['height'] < 0
                    or not isinstance(reports, list) or len(reports) > 100
                    or not isinstance(rows, list) or len(rows) != len(reports)):
                raise ValueError('Invalid coherent assigned audit recovery page')
            by_id = {r.get('request_id'): r for r in reports}
            if len(by_id) != len(reports) or any(not digest(r) for r in by_id):
                raise ValueError('Duplicate or invalid recovered audit report')
            reserved = set()
            # Only the two execution lanes can reserve a job. A retained
            # journal from a retired signer/flow is an input to migration.
            journals = [worker.root / self.active_root / str(i) / 'audits/current.json' for i in range(2)]
            for journal in journals:
                if not journal.is_file():
                    continue
                if journal.stat().st_size > 262144:
                    raise ValueError('Reserved audit journal exceeds budget')
                value = json.loads(journal.read_bytes())
                reserved.add((value.get('request_id'), value.get('slot'), value.get('attempt')))
            candidate = None
            waiting_for_seal_lane = False
            progress = worker.root / 'work-cycles' / 'retained-audit-progress'
            seen = set()
            for row in rows:
                request = row.get('request_id')
                if request not in by_id or request in seen:
                    raise ValueError('Recovered audit page report mismatch')
                seen.add(request)
                report = by_id[request]
                for job in row.get('jobs', []):
                    if job.get('actor') != actor or job.get('state') != 'assigned' or job.get('expired') is not False:
                        continue
                    slot, attempt = job.get('slot'), job.get('attempt')
                    if (type(slot) is not int or slot not in (0, 1) or type(attempt) is not int
                            or not 0 <= attempt < 2**32 or not digest(report.get('ticket'))
                            or not digest(report.get('report_root'))):
                        raise ValueError('Invalid authenticated audit assignment')
                    # Core derives this permission from the accepted assignment
                    # and its network/activation height. Never infer permission
                    # from local miner count, a GUI label or an alias address.
                    accepted = job.get('actor_policy_accepted')
                    if (accepted is not None and type(accepted) is not bool
                            or accepted is False
                            or report.get('miner_script') == actor and accepted is not True):
                        raise ValueError('Assigned actor lacks authenticated audit policy permission')
                    if (request, slot, attempt) in reserved:
                        continue
                    name = f'{request}-{slot}-{attempt}'
                    ranges = job.get('ranges')
                    if not isinstance(ranges, list) or len(ranges) != 5:
                        raise ValueError('Recovered audit requires five exact partitions')
                    previous = -1
                    for part in ranges:
                        index = part.get('index')
                        if type(index) is not int or not previous < index < 100:
                            raise ValueError('Recovered audit partition indices changed')
                        previous = index
                        for bound in ('start', 'end'):
                            text = part.get(bound)
                            if (not isinstance(text, str) or not text or len(text) > 160
                                    or not text.isascii() or not text.isdecimal() or str(int(text)) != text):
                                raise ValueError('Noncanonical recovered audit range')
                        if int(part['start']) >= int(part['end']):
                            raise ValueError('Empty recovered audit partition')
                    selected = dict(schema='PGN_HORIZON_ASSIGNED_AUDIT_V1', request_id=request,
                                    ticket=report['ticket'], report_root=report['report_root'],
                                    slot=slot, attempt=attempt, actor=actor, ranges=ranges, counts=[],
                                    recovered_assignment_anchor=state['bestblockhash'])
                    retained, _ = progress_path(progress, worker.args.genesis, selected)
                    if retained is not None and retained.with_suffix('.conflict').is_file():
                        continue
                    seal_lanes = [AuditAnswers(path.parent) for path in journals]
                    holders = [lane.folder for lane in seal_lanes if lane.records_seal(name)]
                    if len(holders) > 1:
                        raise AuditSealRecoveryRequired('Audit seal recorded in both active lanes')
                    if holders and holders[0] != AuditAnswers(folder).folder:
                        waiting_for_seal_lane = True
                        continue
                    try:
                        candidate = self.retain_legacy_answer(selected)
                        candidate = recover_retired_progress(progress, worker.args.genesis, candidate)
                    except (LegacyAuditSignerUnresolved, RetiredAuditSealConflict):
                        candidate = None
                        continue
                    break
                if candidate is not None:
                    break
            if candidate is not None:
                if verify_signer:
                    from horizon_audit_batch import signable_actor
                    if not read(signable_actor, worker, actor):
                        return None, False
            if read(worker.rpc.call, 'getbestblockhash') != state['bestblockhash']:
                return None, False  # No cursor/journal mutation from a stale page.
            complete = len(reports) < 100 and not waiting_for_seal_lane
            # Stay on a page with a candidate so the second lane can recover
            # another job there. Each successful claim is durable under the
            # shared publication lock before another lane can inspect it.
            next_offset = offset if candidate or waiting_for_seal_lane else (0 if complete else offset + 100)
            if candidate is not None:
                journal = Path(folder) / 'current.json'
                if journal.exists():
                    raise RuntimeError('Audit recovery cannot overwrite an active journal')
                atomic_bytes(journal, canonical_bytes(candidate) + b'\n')
            atomic_bytes(self.cursor, canonical_bytes(dict(genesis=worker.args.genesis,
                         actor=actor, offset=next_offset)) + b'\n')
            return candidate, complete
