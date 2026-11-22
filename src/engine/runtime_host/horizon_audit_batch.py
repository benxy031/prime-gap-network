"""Coalesce already-ready audit operations; Core owns selection and validation.

No timer, quorum, credit or scientific result is created here. The existing
coordination journal owns ambiguous sends; immutable answers remain on disk.
"""
import hashlib
import json
from pathlib import Path
from time import monotonic

from horizon_assigned_audit_recovery import AssignmentRecoveryRpcUnavailable
from horizon_audit_answers import AuditAnswers
from horizon_coordination import CoordinationParentChanged, active_candidate, confirm_candidate, validate_intent
from horizon_core_rpc import CoreRpcError
from horizon_audit_snapshot import identity as hash_identity
from pgn_session import atomic_bytes
from report_commitment import canonical_bytes

ASSIGNMENT_RECOVERY_RETRY_SECONDS = 5
LEGACY_AUDIT_USAGE = 'getpgnauditevent "action" "miner" ( "request_id" slot attempt "body" "signature" )'
_AUDIT_TAG = b'\x6a\x10PGNAUDIT-EVENT-1'


def _field_size(size):
    if size < 253:
        return bytes([size])
    return b'\xfd' + size.to_bytes(2, 'little')


def _commit_output_matches(output, audit, parent):
    # Core has accepted the block. Match the full commitment identity in its
    # canonical OP_RETURN, including the script signer and exact digest.
    text = output.get('scriptPubKey', {}).get('hex')
    if (output.get('value') != 0 or not isinstance(text, str)
            or len(text) > 160000 or len(text) % 2):
        return False
    try:
        raw = bytes.fromhex(text)
        actor = bytes.fromhex(audit['actor'])
        commitment = bytes.fromhex(audit['answer']['commitment'])
    except ValueError:
        return False
    if not raw.startswith(_AUDIT_TAG) or not 0 < len(actor) <= 4096 or len(commitment) != 32:
        return False
    offset = len(_AUDIT_TAG)
    if offset >= len(raw):
        return False
    opcode = raw[offset]
    width = {76: 1, 77: 2, 78: 4}.get(opcode, 0)
    if opcode > 78 or len(raw) < offset+1+width:
        return False
    start = offset+1+width
    size = int.from_bytes(raw[offset+1:start], 'little') if width else opcode
    expected_opcode = size if size <= 75 else 76 if size <= 255 else 77 if size <= 65535 else 78
    if size != len(raw)-start or opcode != expected_opcode:
        return False
    expected = (b'\x01' + bytes.fromhex(audit['request_id'])[::-1] + bytes.fromhex(parent)[::-1]
                + bytes([1, audit['slot']]) + audit['attempt'].to_bytes(4, 'little')
                + _field_size(len(actor)) + actor + b'\x20' + commitment)
    payload = raw[start:]
    if not payload.startswith(expected):
        return False
    signature = payload[len(expected):]
    if not signature:
        return False
    width = 3 if signature[0] == 253 else 1
    size = int.from_bytes(signature[1:3], 'little') if width == 3 else signature[0]
    return size <= 4096 and signature[:width] == _field_size(size) and len(signature) == width+size


def verify_committed_event(rpc, audit, intent, anchor):
    if not active_candidate(rpc, intent):
        raise CoordinationParentChanged('Retained audit block is no longer active')
    block = rpc.call('getblock', [intent['hash'], 2])
    txs = block.get('tx', [])
    coinbase = txs[0] if txs else {}
    inputs = coinbase.get('vin', [])
    outputs = coinbase.get('vout', [])
    if (block.get('hash') != intent['hash'] or block.get('previousblockhash') != intent['parent']
            or not inputs or 'coinbase' not in inputs[0] or len(outputs) > 4096
            or sum(_commit_output_matches(out, audit, intent['parent']) for out in outputs) != 1):
        raise ValueError('Active block lacks this exact audit commitment; seal retained')
    if rpc.call('getbestblockhash') != anchor:
        raise CoordinationParentChanged('Audit commitment evidence changed during reconciliation')


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


def apply_group(operations, *, on_intent=None, on_prepared=None, required_parent=None):
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
                if required_parent is not None and parent != required_parent:
                    raise CoordinationParentChanged('Audit assignment view changed; seal retained')
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
            if retry or required_parent is not None:
                raise
        except CoreRpcError as error:
            if rpc.call('getbestblockhash') == parent:
                raise
            if required_parent is not None:
                raise CoordinationParentChanged('Audit assignment view changed; seal retained') from error
            if retry:
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
        # The journal above handles our own ambiguous sends. Also look for a
        # pre-existing Core assignment when starting with empty lane journals.
        # Each idle lane scans at most one page per turn. A page without a job
        # leaves the fresh assignment path available and retains its cursor.
        for peer in idle:
            recover = getattr(peer.worker, 'recover_assigned_audit', None)
            if not callable(recover) or getattr(peer, 'assignment_recovery_retry_at', 0) > monotonic():
                continue
            try:
                recovered, complete = recover(peer.folder, verify_signer=True)
            except AssignmentRecoveryRpcUnavailable:
                # A failed optional read must not delay another lane's
                # normal assignment. Pace retries while the RPC recovers.
                retry_at = monotonic() + ASSIGNMENT_RECOVERY_RETRY_SECONDS
                for lane in idle:
                    lane.assignment_recovery_retry_at = retry_at
                break
            if recovered is None and complete:
                # The shared durable cursor may have resumed near the end.
                # A later round can still find work on preceding pages.
                peer.assignment_recovery_retry_at = monotonic() + ASSIGNMENT_RECOVERY_RETRY_SECONDS
            elif recovered is not None and peer is publisher:
                return recovered
        idle = [p for p in idle if not (p.folder / 'current.json').exists()]
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


def ready_audit_view(peer, audit, anchor=None):
    request = audit['request_id']
    value = peer.worker.rpc.call('getpgnworkview', ['audits', 0, 1, request])
    state = value.get('state', {})
    current = state.get('bestblockhash')
    reports, audits = value.get('reports'), value.get('audits')
    genesis = getattr(getattr(peer.worker, 'args', None), 'genesis', state.get('genesis'))
    if (value.get('schema') != 'PGN_WORK_VIEW_V1' or value.get('kind') != 'audits'
            or value.get('request_id') != request or not hash_identity(current)
            or not hash_identity(genesis) or state.get('genesis') != genesis
            or not isinstance(reports, list) or len(reports) > 1
            or not isinstance(audits, list) or len(audits) > 1
            or any(row.get('request_id') != request for row in reports+audits)):
        raise ValueError('Invalid anchored audit work view; seal retained')
    if anchor is not None and current != anchor:
        raise CoordinationParentChanged('Audit work view changed; seal retained')
    row = audits[0] if audits else {}
    report = reports[0] if reports else {}
    job = next((j for j in row.get('jobs', []) if (j['slot'], j['attempt']) ==
                (audit['slot'], audit['attempt'])), None)
    exact = report.get('ticket') == audit['ticket'] and report.get('report_root') == audit['report_root']
    return current, row, job if exact else None


def commit_ready(publisher, audit, retain_self):
    """Include only seals whose own before_commit boundary has already passed."""
    peers = getattr(publisher.worker, 'audit_publishers', (publisher,))
    with publisher.worker.network_lock:
        anchor = publisher.worker.rpc.call('getbestblockhash')
        selected = []
        for peer in peers:
            path = peer.folder / 'current.json'
            if not path.exists() or path.stat().st_size > 262144:
                continue
            row = json.loads(path.read_bytes())
            if not row.get('commit_ready') or not row.get('answer'):
                continue
            _, _, job = ready_audit_view(peer, row, anchor)
            if (job and job['state'] == 'assigned' and job.get('expired') is False
                    and job.get('actor_policy_accepted') is True
                    and row.get('actor') and job.get('actor') == row['actor']
                    and job.get('ranges') == row['ranges']):
                selected.append((peer, row))
        if not any(peer is publisher for peer, _ in selected):
            current = json.loads((publisher.folder / 'current.json').read_bytes())
            _, _, job = ready_audit_view(publisher, audit, anchor)
            if (identity(current) == identity(audit) and current.get('answer') == audit['answer']
                    and job and job.get('actor') == audit.get('actor')
                    and job.get('ranges') == audit['ranges']
                    and job.get('commitment') == audit['answer']['commitment']
                    and current.get('publication_intent')):
                verify_committed_event(publisher.worker.rpc, audit, current['publication_intent'], anchor)
                audit['publication_intent'] = current['publication_intent']
                return dict(available=True, confirmed_block=current['publication_intent']['hash'])
            raise RuntimeError('Ready audit changed before grouped commitment; seal retained')
        # A lost or duplicated local cursor cannot put two commitments for
        # the same canonical attempt in a single signed prefix.
        own = identity(audit)
        copies = [(peer, row) for peer, row in selected if identity(row) == own]
        if len(copies) > 1:
            for _, row in copies:
                AuditAnswers.check_binding(row, audit)
            # Both rows now prove the exact same ranges and counts. The
            # requesting lane commits its existing seal under the group lock.
            # Keep the other lane's seal intact for canonical reconciliation.
            selected = [(peer, row) for peer, row in selected if identity(row) != own or peer is publisher]
        included = []
        def retain_included(proposals):
            included[:] = [entry for entry, proposal in zip(selected, proposals) if proposal.get('available')]
        def retain(intent):
            for peer, row in included:
                row['publication_intent'] = dict(intent)
                name = '-'.join(str(v) for v in identity(row))
                save(peer.folder / (name+'.json'), row)
                save(peer.folder / 'current.json', row)
                if peer is publisher:
                    audit['publication_intent'] = dict(intent)
            if any(peer is publisher for peer, _ in included):
                retain_self(intent)
        outcomes = apply_group([(peer, 'commit', *identity(row), row['answer']['commitment']) for peer, row in selected],
                               on_intent=retain, on_prepared=retain_included, required_parent=anchor)
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
