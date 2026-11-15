"""Recover chain-assigned work after loss of its local scheduling journal.

This reader never selects a new assignment, invents an answer or waives duty.
One bounded coherent page is scanned per call; the cursor survives restarts.
"""
from pathlib import Path
import hashlib
import json
import sqlite3
from pgn_session import atomic_bytes
from report_commitment import canonical_bytes, audit_commitment


def digest(value):
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


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
                raise ValueError('Retained inactive audit differs from the assigned scientific work')
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

    def reconcile_duplicates(self):
        """Retire a duplicate scheduling copy only after exact Core validation.

        Original bytes remain content-addressed in its own retained folder.
        Conflicting answers/progress/cycle bindings require attention, never a
        new nonce, guessed credit or arbitrary choice of a different result.
        """
        worker=self.worker
        with worker.network_lock:
            items=[]
            for i in range(2):
                path=worker.root/self.active_root/str(i)/'audits/current.json'
                if not path.is_file():continue
                if path.stat().st_size>262144:raise ValueError('Duplicate audit journal exceeds budget')
                raw=path.read_bytes();value=json.loads(raw);items.append((path,raw,value))
            if len(items)!=2:return False
            left,right=(x[2]for x in items)
            keys=('request_id','slot','attempt')
            if any(left.get(k)!=right.get(k)for k in keys):return False
            if (not digest(left.get('request_id'))or type(left.get('slot'))is not int or left['slot']not in(0,1)
                    or type(left.get('attempt'))is not int or not 0<=left['attempt']<2**32):
                raise ValueError('Invalid duplicate audit identity; originals retained')
            for key in ('ticket','report_root','ranges','work_cycle','physical_cycle'):
                if left.get(key)!=right.get(key):raise ValueError('Conflicting duplicate audit binding; originals retained')
            # Two genuine cycle reservations cannot be erased as scheduling
            # noise. The demonstrated pre-ticket duplicate has no such credit.
            if left.get('work_cycle')or left.get('physical_cycle'):
                raise ValueError('Duplicate audit has a cycle reservation; originals retained')
            for value in (left,right):
                counts=value.get('counts')
                if (not isinstance(counts,list)or len(counts)>5
                        or any(type(n)is not int or not 0<=n<2**64 for n in counts)):
                    raise ValueError('Invalid duplicate audit counts; originals retained')
                answer=value.get('answer')
                if answer is not None:
                    from horizon_audit_answers import AuditAnswers
                    AuditAnswers.check_binding(value,value)
                if value.get('publication_intent'):
                    from horizon_coordination import validate_intent
                    validate_intent(value['publication_intent'])
            size=min(len(left['counts']),len(right['counts']))
            if left['counts'][:size]!=right['counts'][:size]:raise ValueError('Conflicting duplicate audit counts; originals retained')
            if left.get('answer')is not None and right.get('answer')is not None and left['answer']!=right['answer']:
                raise ValueError('Conflicting duplicate audit seals; originals retained')
            view=worker.rpc.call('getpgnworkview',['audits',0,1,left['request_id']]);state=view.get('state',{})
            reports=view.get('reports');rows=view.get('audits')
            if (view.get('schema')!='PGN_WORK_VIEW_V1'or view.get('kind')!='audits'
                    or state.get('genesis')!=worker.args.genesis or not digest(state.get('bestblockhash'))
                    or not isinstance(reports,list)or len(reports)!=1 or not isinstance(rows,list)or len(rows)!=1
                    or reports[0].get('request_id')!=left['request_id']or rows[0].get('request_id')!=left['request_id']
                    or any(reports[0].get(k)!=left[k]for k in ('ticket','report_root'))):
                raise ValueError('Duplicate audit report differs from Core; originals retained')
            jobs=[j for j in rows[0].get('jobs',[])if j.get('slot')==left['slot']and j.get('attempt')==left['attempt']]
            if len(jobs)!=1:raise ValueError('Duplicate audit attempt differs from Core; originals retained')
            job=jobs[0]
            if job.get('state')!='assigned':return False # Existing confirmation/reorg handling owns this case.
            owned=worker.rpc.call('getaddressinfo',[worker.address()],wallet=True)
            if (owned.get('ismine')is not True or owned.get('iswatchonly')is True
                    or owned.get('scriptPubKey')!=job.get('actor') or job.get('expired')is not False
                    or job.get('actor_policy_accepted')is not True or job.get('ranges')!=left.get('ranges')):
                raise ValueError('Duplicate audit is not the owned active Core assignment; originals retained')
            if worker.rpc.call('getbestblockhash')!=state['bestblockhash']:
                raise ValueError('Duplicate audit anchor changed; originals retained')
            winner=max(range(2),key=lambda i:(len(items[i][2]['counts']),items[i][2].get('answer')is not None,
                                             items[i][2].get('publication_intent')is not None,-i))
            selected=items[winner][2]
            answer=selected.get('answer')
            selected_digest=hashlib.sha256(canonical_bytes(answer)).hexdigest()if answer is not None else None
            name=left['request_id']+'-'+str(left['slot'])+'-'+str(left['attempt'])
            # A catalogue/immutable copy may outlive mutable current.json.
            # Never move scheduling away from an unknown original seal and
            # thereby permit the other lane to generate a replacement nonce.
            from horizon_audit_answers import AuditAnswers
            for path,_,value in items:
                database=path.parent/'answers.sqlite3'
                if database.is_file():
                    db=sqlite3.connect(database.as_uri()+'?mode=ro',uri=True)
                    try:row=db.execute('SELECT digest FROM answers WHERE name=?',(name,)).fetchone()
                    finally:db.close()
                    if row and row[0]!=selected_digest:
                        raise ValueError('Registered duplicate audit seal needs recovery; originals retained')
                for seal in (path.parent/(name+'.json'),path.parent/'sealed-answers'/(name+'.json')):
                    if not seal.is_file():continue
                    if seal.stat().st_size>262144:raise ValueError('Duplicate audit seal exceeds budget; originals retained')
                    saved=json.loads(seal.read_bytes());AuditAnswers.check_binding(saved,selected)
                    if hashlib.sha256(canonical_bytes(saved['answer'])).hexdigest()!=selected_digest:
                        raise ValueError('Retained duplicate audit seal differs; originals retained')
            path,raw,_=items[1-winner]
            archive=path.parent/'retained-assignments';archive.mkdir(exist_ok=True)
            target=archive/(hashlib.sha256(raw).hexdigest()+'.json')
            if target.exists():
                if target.stat().st_size!=len(raw)or target.read_bytes()!=raw:raise ValueError('Retained duplicate audit differs')
            else:atomic_bytes(target,raw)
            if path.read_bytes()!=raw:raise ValueError('Duplicate audit changed during recovery; originals retained')
            path.unlink()
            return True

    def claim(self, folder):
        worker = self.worker
        with worker.network_lock:
            identity = worker.rpc.call('validateaddress', [worker.address()])
            actor = identity.get('scriptPubKey')
            if (identity.get('isvalid') is not True or not isinstance(actor, str)
                    or not actor or len(actor) > 20000 or len(actor) % 2
                    or any(c not in '0123456789abcdef' for c in actor)):
                raise ValueError('Authenticated miner signing script required for audit recovery')
            offset = 0
            if self.cursor.is_file():
                if self.cursor.stat().st_size > 1024:
                    raise ValueError('Assigned audit recovery cursor exceeds budget')
                saved = json.loads(self.cursor.read_bytes())
                if saved.get('genesis') != worker.args.genesis or saved.get('actor') != actor:
                    raise ValueError('Assigned audit recovery cursor identity changed')
                offset = saved.get('offset')
                if type(offset) is not int or not 0 <= offset < 2**63 - 100:
                    raise ValueError('Invalid assigned audit recovery cursor')
            view = worker.rpc.call('getpgnworkview', ['audits', offset, 100])
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
                    candidate = dict(schema='PGN_HORIZON_ASSIGNED_AUDIT_V1', request_id=request,
                                     ticket=report['ticket'], report_root=report['report_root'],
                                     slot=slot, attempt=attempt, ranges=ranges, counts=[],
                                     recovered_assignment_anchor=state['bestblockhash'])
                    break
                if candidate is not None:
                    break
            if candidate is not None:
                candidate = self.retain_legacy_answer(candidate)
            if worker.rpc.call('getbestblockhash') != state['bestblockhash']:
                return None, False  # No cursor/journal mutation from a stale page.
            complete = len(reports) < 100
            # Stay on a page with a candidate so the second lane can recover
            # another job there. Each successful claim is durable under the
            # shared publication lock before another lane can inspect it.
            next_offset = offset if candidate else (0 if complete else offset + 100)
            if candidate is not None:
                journal = Path(folder) / 'current.json'
                if journal.exists():
                    raise RuntimeError('Audit recovery cannot overwrite an active journal')
                atomic_bytes(journal, canonical_bytes(candidate) + b'\n')
            atomic_bytes(self.cursor, canonical_bytes(dict(genesis=worker.args.genesis,
                         actor=actor, offset=next_offset)) + b'\n')
            return candidate, complete
