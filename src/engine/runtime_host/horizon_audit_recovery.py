"""Retire a stale scheduling cursor, preserving every byte of private work."""
from pathlib import Path
import hashlib,json,re
from pgn_session import atomic_bytes
from report_commitment import canonical_bytes
from horizon_audit_answers import AuditAnswers
from horizon_coordination import validate_intent
from horizon_core_rpc import CoreRpcError


PROGRESS_IDENTITY = ('request_id', 'ticket', 'report_root', 'slot', 'attempt', 'actor', 'ranges')


def progress_path(root, genesis, audit):
    if root is None or not isinstance(genesis, str) or not re.fullmatch('[0-9a-f]{64}', genesis) or not audit.get('actor'):
        return None, None
    binding = dict(genesis=genesis, **{key: audit[key] for key in PROGRESS_IDENTITY})
    return Path(root) / (hashlib.sha256(canonical_bytes(binding)).hexdigest() + '.json'), binding


def read_progress(path, binding):
    if path is None or not path.is_file():
        return None
    if path.stat().st_size > 262144:
        raise ValueError('Retired audit checkpoint exceeds budget')
    value = json.loads(path.read_bytes())
    counts, target = value.get('counts'), value.get('target_merit_q16')
    if (value.get('binding') != binding or not isinstance(counts, list)
            or not 0 < len(counts) <= len(binding['ranges'])
            or any(type(count) is not int or not 0 <= count < 2**64 for count in counts)
            or type(target) is not int or not 0 < target < 2**31):
        raise ValueError('Invalid retired audit checkpoint')
    if 'answer' in value:
        AuditAnswers.check_binding(dict(binding, counts=counts, answer=value['answer']), binding)
    return value


def recover_retired_progress(root, genesis, audit):
    path, binding = progress_path(root, genesis, audit)
    value = read_progress(path, binding)
    if value is None:
        return audit
    counts, current = value['counts'], audit['counts']
    common = min(len(counts), len(current))
    if counts[:common] != current[:common]:
        raise ValueError('Retired audit counts differ from the active prefix')
    if 'target_merit_q16' in audit and audit['target_merit_q16'] != value['target_merit_q16']:
        raise ValueError('Retired audit target differs from the active target')
    if 'answer' in value:
        if (('answer' in audit or 'publication_intent' in audit)
                and audit.get('answer') != value['answer']):
            raise ValueError('Retired audit seal differs from the active seal')
        return dict(audit, counts=counts, target_merit_q16=value['target_merit_q16'], answer=value['answer'])
    if len(current) >= len(counts):
        return audit
    if audit.get('answer') or audit.get('publication_intent'):
        raise ValueError('Retired audit counts cannot replace a sealed result')
    return dict(audit, counts=counts, target_merit_q16=value['target_merit_q16'])


def retain_progress(root, genesis, audit):
    counts = audit.get('counts')
    if not counts:
        return
    path, binding = progress_path(root, genesis, audit)
    if path is None:
        return
    target = audit.get('target_merit_q16')
    if (not isinstance(counts, list) or len(counts) > len(binding['ranges'])
            or any(type(count) is not int or not 0 <= count < 2**64 for count in counts)
            or type(target) is not int or not 0 < target < 2**31):
        raise ValueError('Invalid progress before audit retirement')
    answer = audit.get('answer')
    if answer is not None:
        AuditAnswers.check_binding(audit, binding)
    retained = read_progress(path, binding)
    if retained:
        earlier = retained['counts']
        common = min(len(counts), len(earlier))
        if counts[:common] != earlier[:common] or target != retained['target_merit_q16']:
            raise ValueError('Conflicting progress for the same audit assignment')
        if answer is not None and 'answer' in retained and retained['answer'] != answer:
            raise ValueError('Conflicting seals for the same audit assignment')
        if len(counts) <= len(earlier) and (answer is None or 'answer' in retained):
            return
    path.parent.mkdir(parents=True, exist_ok=True)
    progress = dict(binding=binding, counts=counts, target_merit_q16=target)
    if answer is not None:
        progress['answer'] = answer
    atomic_bytes(path, canonical_bytes(progress) + b'\n')


def retain_changed_assignment(folder, audit, state, anchor, rpc, callback, *, checkpoint_root=None, genesis=None):
    if not isinstance(anchor,str) or not re.fullmatch('[0-9a-f]{64}',anchor):
        raise ValueError('Explicit reconciliation anchor required')
    source=Path(folder)/'current.json'
    raw=source.read_bytes()
    if len(raw)>262144 or json.loads(raw)!=audit:
        raise ValueError('Current audit changed before retention')
    if rpc.call('getbestblockhash')!=anchor:
        raise RuntimeError('Chain changed during audit retirement; current work retained')
    digest=hashlib.sha256(raw).hexdigest()
    destination=Path(folder)/'retained-assignments'/f'{digest}.json'
    destination.parent.mkdir(parents=True,exist_ok=True)
    if destination.exists():
        if destination.read_bytes()!=raw:raise ValueError('Retained audit identity changed')
    else:atomic_bytes(destination,raw)
    retain_progress(checkpoint_root, genesis, audit)
    # The callback can release only the exact pending local scheduling slot.
    # It does not grant a commitment, service credit, verdict or reward.
    callback(audit,anchor)
    if source.read_bytes()!=raw:raise ValueError('Audit changed during retirement')
    source.unlink()
    return destination


def assignment_absence_confirmed(audit, view, anchor, rpc):
    prior = audit.get('assigned_audit_anchor') or audit.get('recovered_assignment_anchor')
    if prior == anchor:
        return False
    if view.get('_report_binding_observed') is True:
        return True
    if view.get('_report_absent') is not True:
        return False
    if prior is None and audit.get('publication_intent'):
        validate_intent(audit['publication_intent'])
        prior = audit['publication_intent']['parent']
    if prior is None:
        # Older installations did not persist an assignment anchor. Require
        # a second exact Core read before releasing that scheduling cursor.
        return (rpc.call('getpgnreports', [0, 1, audit['request_id']]) == []
                and rpc.call('getbestblockhash') == anchor)
    if not isinstance(prior, str) or not re.fullmatch('[0-9a-f]{64}', prior):
        return False
    try:
        header = rpc.call('getblockheader', [prior, True])
    except CoreRpcError as error:
        if error.code == -5:
            return False
        raise
    return header.get('hash') == prior and header.get('confirmations') == -1
