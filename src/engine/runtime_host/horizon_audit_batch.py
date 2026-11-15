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

LEGACY_AUDIT_USAGE = 'getpgnauditevent "action" "miner" ( "request_id" slot attempt "body" "signature" )'


def prefix_capability(publisher):
    try:
        probe = publisher.worker.rpc.call('getpgnauditevent',
            ['assign', publisher.worker.address(), None, None, None, None, None, []])
    except CoreRpcError as error:
        # Core19's actual excess-parameter reply is -1 plus this exact legacy
        # seven-argument usage line. Other -1/-8 errors are NOT capabilities.
        if error.code == -1 and str(error).splitlines()[0] == LEGACY_AUDIT_USAGE:
            return False
        if error.code == -32601 and str(error) == 'Method not found':
            return False
        raise
    if probe.get('proposal_context_version') != 1:
        raise RuntimeError('Core returned an unrecognized audit-prefix contract; work retained')
    return True


def save(path, value):
    atomic_bytes(path, canonical_bytes(value) + b'\n')


def identity(audit):
    return audit['request_id'], audit['slot'], audit['attempt']


def signable_actor(worker, actor):
    address = worker.rpc.call('decodescript', [actor]).get('address')
    if not isinstance(address, str) or not address:
        return False
    owned = worker.rpc.call('getaddressinfo', [address], wallet=True)
    wallet = worker.rpc.call('getwalletinfo', wallet=True)
    return (owned.get('ismine') is True and owned.get('iswatchonly') is not True
            and owned.get('scriptPubKey') == actor
            and wallet.get('private_keys_enabled') is True
            and wallet.get('unlocked_until') != 0)


def retain_group_history(journal, raw, observation):
    """Keep the original bytes plus an immutable anchored resolution receipt."""
    folder = journal.parent / 'audit-assignment-history'
    folder.mkdir(exist_ok=True)
    digest = hashlib.sha256(raw).hexdigest()
    source = folder / (digest+'.json')
    if source.exists():
        if source.read_bytes() != raw:
            raise ValueError('Retained assignment proof changed')
    else:
        atomic_bytes(source, raw)
    observed = canonical_bytes(observation)+b'\n'
    receipt = folder / (digest+'-'+hashlib.sha256(observed).hexdigest()+'.resolution.json')
    if receipt.exists():
        if receipt.read_bytes() != observed:
            raise ValueError('Retained assignment resolution changed')
    else:
        atomic_bytes(receipt, observed)


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
            previous_raw = journal.read_bytes()
            previous = json.loads(previous_raw)
            if not previous.get('retained') and previous.get('intent'):
                validate_intent(previous['intent'])
                try:
                    confirm_candidate(publisher.worker.rpc, previous['intent'], seconds=0, rebase=True)
                except CoordinationParentChanged:
                    observation = dict(anchor=publisher.worker.rpc.call('getbestblockhash'),status='parent_changed')
                    retain_group_history(journal, previous_raw, observation)
                else:
                    observation = retain_assignments(peers, previous)
                    if any(row['status'] != 'active' for row in observation['rows']):
                        retain_group_history(journal, previous_raw, observation)
                previous['reservation_resolution'] = observation
                previous['retained'] = True
                save(journal, previous)
        own = publisher.folder / 'current.json'
        if own.is_file():
            return json.loads(own.read_bytes())
        supported = getattr(publisher.worker, 'audit_prefix_supported', None)
        if supported is None:
            supported = prefix_capability(publisher)
            for peer in peers:
                peer.worker.audit_prefix_supported = supported
        if not supported:
            return NotImplemented  # Existing Core keeps its original single path.
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
                        attempt=proposal['attempt'], actor=proposal['actor'], ranges=proposal['ranges'], counts=[])
                    rows.append(dict(lane=peers.index(p), audit=audit))
            pending.update(rows=rows, retained=False)
            save(journal, pending)
        def retain_intent(intent):
            validate_intent(intent)
            pending.update(intent=dict(intent), retained=False)
            save(journal, pending)
        proposals = apply_group(operations, on_intent=retain_intent, on_prepared=prepared)
        if pending.get('intent'):
            pending['reservation_resolution'] = retain_assignments(peers, pending)
            if any(row['status'] != 'active' for row in pending['reservation_resolution']['rows']):
                retain_group_history(journal, journal.read_bytes(), pending['reservation_resolution'])
            pending['retained'] = True
            save(journal, pending)
        publisher.last_unavailable = proposals[idle.index(publisher)]
        return json.loads(own.read_bytes()) if own.is_file() else None


def retain_assignments(peers, group):
    anchor = peers[0].worker.rpc.call('getbestblockhash')
    writes, observations = [], []
    for row in group['rows']:
        lane = row['lane']
        if type(lane) is not int or not 0 <= lane < len(peers):
            raise ValueError('Retained assignment has unknown physical lane')
        path = peers[lane].folder / 'current.json'
        audit = row['audit']
        state = peers[lane].state(audit['request_id'])
        job = next((j for j in state.get('jobs', []) if j['slot'] == audit['slot']), None)
        reports = peers[lane].worker.rpc.call('getpgnreports', [0, 1, audit['request_id']])
        observed = dict(lane=lane, request_id=audit['request_id'], slot=audit['slot'], attempt=audit['attempt'])
        if not job:
            status = 'missing'
        elif job['attempt'] != audit['attempt']:
            status = 'reassigned'
        elif not audit.get('actor'):
            status = 'legacy_actor_unbound'
        elif job.get('actor') != audit['actor']:
            status = 'actor_changed'
        elif len(reports) != 1 or reports[0]['ticket'] != audit['ticket'] or reports[0]['report_root'] != audit['report_root']:
            status = 'report_changed'
        elif job['state'] in ('committed','passed','failed'):
            status = job['state']
        elif job.get('expired') is True:
            status = 'expired'
        elif job['state'] != 'assigned' or job.get('actor_policy_accepted') is not True:
            status = 'permission_unavailable'
        elif not signable_actor(peers[lane].worker, job['actor']):
            status = 'key_unavailable'
        else:
            status = 'active'
        if job:
            observed.update(canonical_actor=job.get('actor'),canonical_attempt=job['attempt'],canonical_state=job['state'],expired=job.get('expired'))
        observed['status'] = status
        observations.append(observed)
        if status != 'active':
            continue
        if job.get('ranges') != audit['ranges']:
            raise ValueError('Retained reservation differs from confirmed audit identity/ranges')
        if path.exists():
            if identity(json.loads(path.read_bytes())) != identity(row['audit']):
                raise ValueError('Assignment reservation conflicts with retained lane')
        else:
            writes.append((path, audit))
    if peers[0].worker.rpc.call('getbestblockhash') != anchor:
        raise CoordinationParentChanged('Assignment recovery anchor changed; journal retained')
    for path, audit in writes:
        save(path, audit)
    return dict(anchor=anchor,rows=observations)


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


def disclose_local_ready(owner, queued):
    """Include an already-retained local owner disclosure with ready seals.

    Remote owners are never impersonated. Their rows stay in the durable
    catalogue for the ordinary next observation. Gold handling is untouched.
    """
    from horizon_network_audits import RetainedAuditPublicationError
    remaining, withheld, errors = [], {}, []
    for publisher, name, audit in queued:
        state = publisher.state(audit['request_id'])
        if state.get('details_root'):
            remaining.append((publisher, name, audit))
        else:
            withheld.setdefault(audit['request_id'], []).append((publisher, name, audit))
    for request, rows in withheld.items():
        state = owner.state(request)
        jobs = state.get('jobs', [])
        if len(jobs) != 2 or any(job['state'] != 'committed' for job in jobs):
            continue
        reports = owner.worker.rpc.call('getpgnreports', [0, 1, request])
        if len(reports) != 1:
            continue
        source = owner.worker.root / 'tickets' / reports[0]['ticket'] / 'network-report.json'
        if not source.is_file():
            continue
        if source.stat().st_size > 262144:
            raise ValueError('Local owner disclosure exceeds retained report budget')
        sealed = json.loads(source.read_bytes())
        if sealed.get('report_root') != reports[0]['report_root'] or hashlib.sha256(canonical_bytes(sealed['body'])).hexdigest() != reports[0]['report_root']:
            raise ValueError('Local owner disclosure differs from confirmed commitment')
        operations = [(owner, 'reveal_report', request, 0, 0, canonical_bytes(sealed['body']).hex())]
        seen = set()
        for publisher, name, audit in rows:
            key = identity(audit)
            if key not in seen:
                seen.add(key)
                operations.append((publisher, 'reveal', *key, canonical_bytes(audit['answer']['body']).hex()))
        try:
            apply_group(operations)
        except RetainedAuditPublicationError as error:
            errors.extend((publisher, name, error) for publisher, name, _ in rows)
    return remaining, errors
