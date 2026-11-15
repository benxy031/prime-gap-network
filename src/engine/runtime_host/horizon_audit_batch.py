"""Coalesce already-ready audit operations; Core owns selection and validation.

No timer, quorum, credit or scientific result is created here. The existing
coordination journal owns ambiguous sends; immutable answers remain on disk.
"""
import hashlib
import json
from pathlib import Path

from horizon_coordination import CoordinationParentChanged, confirm_candidate, validate_intent
from horizon_core_rpc import CoreRpcError
from pgn_session import atomic_bytes
from report_commitment import canonical_bytes


def save(path, value):
    atomic_bytes(path, canonical_bytes(value) + b'\n')


def identity(audit):
    return audit['request_id'], audit['slot'], audit['attempt']


def apply_group(operations, *, on_intent=None, on_prepared=None):
    """Ordered (publisher, action, request, slot, attempt, body) ready rows.

    Re-sign the entire group after a parent conflict. Never re-use signatures
    from an abandoned parent, and never wait for another row to become ready.
    """
    if not operations or len(operations) > 64:
        raise ValueError('Audit publication requires one bounded ready page')
    first = operations[0][0]
    rpc, lock = first.worker.rpc, first.worker.network_lock
    stage = lambda action: 'disclose' if action in ('reveal', 'reveal_report') else action
    expected = stage(operations[0][1])
    if expected not in ('assign', 'commit', 'disclose'):
        raise ValueError('Unsupported audit publication stage')
    for publisher, action, *_ in operations:
        if publisher.worker.rpc is not rpc or publisher.worker.network_lock is not lock:
            raise ValueError('Audit group crosses Core or publication lane')
        if stage(action) != expected:
            raise ValueError('Audit group crosses confirmation stages')
    for retry in range(2):
        try:
            with lock:
                proposals, envelopes = [], []
                parent = rpc.call('getbestblockhash')
                for publisher, action, request, slot, attempt, body in operations:
                    if getattr(publisher.worker, 'quitting', None) is not None and publisher.worker.quitting.is_set():
                        raise RuntimeError('Audit publication stopped; durable work retained')
                    proposal = publisher.event(action, request, slot, attempt, body,
                                               preceding_events=envelopes)
                    if not proposal.get('available'):
                        proposals.append(proposal)
                        continue
                    if proposal.get('proposal_context_version') != 1:
                        raise RuntimeError('Core signed audit-prefix API required; work retained')
                    if proposal['parent'] != parent:
                        raise CoordinationParentChanged('Audit proposal parent changed')
                    envelope = proposal['envelope']
                    raw = bytes.fromhex(envelope)
                    if raw.hex() != envelope or not raw or sum(len(e)//2 for e in envelopes)+len(raw) > 262144:
                        raise ValueError('Audit group exceeds existing durable publication budget')
                    envelopes.append(envelope)
                    proposals.append(proposal)
                if envelopes:
                    if on_prepared is not None:
                        on_prepared(proposals)
                    receipt = first.worker.ledger(envelopes, on_intent=on_intent) if on_intent else first.worker.ledger(envelopes)
                    block = receipt.get('block_hash') if isinstance(receipt, dict) else None
                    if not isinstance(block, str) or len(block) != 64 or bytes.fromhex(block).hex() != block or block == '0'*64:
                        raise RuntimeError('Audit group lacks canonical block receipt; work retained')
                    proposals = [dict(p, confirmed_block=block) if p.get('available') else p for p in proposals]
                return proposals
        except CoordinationParentChanged:
            if retry:
                raise
        except CoreRpcError:
            if retry or rpc.call('getbestblockhash') == parent:
                raise


def reserve_assignments(publisher):
    """Reserve idle physical lanes atomically with their grouped confirmation.

    One shared write-ahead journal closes the crash window between Core's
    acceptance and retaining both per-lane identities (the anti-race boundary).
    """
    peers = getattr(publisher.worker, 'audit_publishers', (publisher,))
    journal = getattr(publisher.worker, 'audit_group_journal', None)
    if journal is None:
        return None
    journal = Path(journal)
    with publisher.worker.network_lock:
        if journal.is_file():
            if journal.stat().st_size > 262144:
                raise ValueError('Assignment group journal exceeds recovery budget')
            previous = json.loads(journal.read_bytes())
            if not previous.get('retained') and previous.get('intent'):
                validate_intent(previous['intent'])
                try:
                    confirm_candidate(publisher.worker.rpc, previous['intent'], seconds=0, rebase=True)
                except CoordinationParentChanged:
                    archive = journal.parent / 'audit-assignment-history'
                    archive.mkdir(exist_ok=True)
                    save(archive / (previous['intent']['hash']+'.json'), previous)
                else:
                    retain_assignments(peers, previous)
                previous['retained'] = True
                save(journal, previous)
        own = publisher.folder / 'current.json'
        if own.is_file():
            return json.loads(own.read_bytes())
        idle = [p for p in peers if not (p.folder / 'current.json').exists()]
        if publisher not in idle:
            return None
        # No peer is added later, and no busy lane is polled or waited for.
        # Native RPC may decline a lane; no explicit report/slot is selected.
        operations = [(p, 'assign', None, None, None, None) for p in idle]
        pending = {}
        def prepared(proposals):
            pending.clear()
            rows = []
            for p, proposal in zip(idle, proposals):
                if proposal.get('available'):
                    report = p.worker.rpc.call('getpgnreports', [0, 1, proposal['request_id']])
                    if len(report) != 1:
                        raise ValueError('Assigned audit has no immutable report')
                    audit = dict(schema='PGN_HORIZON_ASSIGNED_AUDIT_V1', request_id=proposal['request_id'],
                        ticket=report[0]['ticket'], report_root=report[0]['report_root'], slot=proposal['slot'],
                        attempt=proposal['attempt'], ranges=proposal['ranges'], counts=[])
                    rows.append(dict(lane=peers.index(p), audit=audit))
            pending.update(rows=rows, retained=False)
            save(journal, pending)
        def retain_intent(intent):
            validate_intent(intent)
            pending.update(intent=dict(intent), retained=False)
            save(journal, pending)
        proposals = apply_group(operations, on_intent=retain_intent, on_prepared=prepared)
        if pending.get('intent'):
            retain_assignments(peers, pending)
            pending['retained'] = True
            save(journal, pending)
        publisher.last_unavailable = proposals[idle.index(publisher)]
        return json.loads(own.read_bytes()) if own.is_file() else None


def retain_assignments(peers, group):
    for row in group['rows']:
        lane = row['lane']
        if type(lane) is not int or not 0 <= lane < len(peers):
            raise ValueError('Retained assignment has unknown physical lane')
        path = peers[lane].folder / 'current.json'
        if path.exists():
            if identity(json.loads(path.read_bytes())) != identity(row['audit']):
                raise ValueError('Assignment reservation conflicts with retained lane')
        else:
            save(path, row['audit'])


def commit_ready(publisher, audit, retain_self):
    """Include only seals whose own before_commit boundary has already passed."""
    peers = getattr(publisher.worker, 'audit_publishers', (publisher,))
    with publisher.worker.network_lock:
        selected = []
        for peer in peers:
            path = peer.folder / 'current.json'
            if not path.exists() or path.stat().st_size > 262144:
                continue
            row = json.loads(path.read_bytes())
            if not row.get('commit_ready') or not row.get('answer'):
                continue
            state = peer.state(row['request_id'])
            job = next((j for j in state.get('jobs', []) if (j['slot'], j['attempt']) == (row['slot'], row['attempt'])), None)
            if job and job['state'] == 'assigned':
                selected.append((peer, row))
        if not any(peer is publisher for peer, _ in selected):
            current = json.loads((publisher.folder / 'current.json').read_bytes())
            state = publisher.state(audit['request_id'])
            job = next((j for j in state.get('jobs', []) if (j['slot'], j['attempt']) == (audit['slot'], audit['attempt'])), None)
            if job and job.get('commitment') == audit['answer']['commitment'] and current.get('publication_intent'):
                confirm_candidate(publisher.worker.rpc, current['publication_intent'], seconds=0, rebase=True)
                audit['publication_intent'] = current['publication_intent']
                return dict(available=True, confirmed_block=current['publication_intent']['hash'])
            raise RuntimeError('Ready audit changed before grouped commitment; seal retained')
        def retain(intent):
            for peer, row in selected:
                row['publication_intent'] = dict(intent)
                name = '-'.join(str(v) for v in identity(row))
                save(peer.folder / (name+'.json'), row)
                save(peer.folder / 'current.json', row)
                if peer is publisher:
                    audit['publication_intent'] = dict(intent)
            retain_self(intent)
        outcomes = apply_group([(peer, 'commit', *identity(row), row['answer']['commitment']) for peer, row in selected], on_intent=retain)
        return outcomes[next(i for i, (peer, _) in enumerate(selected) if peer is publisher)]
