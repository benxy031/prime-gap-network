"""Execute authority-assigned partitions without access to expected counts."""
from pathlib import Path
import hashlib
import json
import math
import secrets
import time
from pgn_native_contract import fields, encode_target_q16
from pgn_session import atomic_bytes
from report_commitment import audit_commitment, canonical_bytes
from horizon_coordination import confirm_candidate, validate_intent, CoordinationParentChanged, PublicationPending
from horizon_audit_answers import AuditAnswers, AuditSealBindingMismatch
from horizon_owner_reports import OwnerReports
from horizon_audit_progress import partition_identity, presentation as audit_presentation
from horizon_audit_recovery import (RetiredAuditSealConflict, archive_assignment_cursor, assignment_absence_confirmed,
                                    recover_retired_progress, retain_changed_assignment)
from horizon_audit_snapshot import AnchoredAuditStates, identity

def save(file, value):
    atomic_bytes(Path(file), canonical_bytes(value) + b"\n")


class RetainedAuditPublicationError(RuntimeError):
    """One immutable receipt needs recovery; it cannot block other receipts."""


def partition_ticket(audit, part):
    start, end = int(part['start']), int(part['end'])
    identity = partition_identity(audit, part)
    return dict(schema='PGN_HORIZON_ENGINE_TICKET_V1', ticket_id=identity, start=str(start), end=str(end),
                threshold_policy=fields(start, end)['threshold_policy'], target_merit_q16=audit['target_merit_q16'])

class NetworkAudits:
    def __init__(self, worker, *, on_assigned=None, on_committed=None, before_commit=None, on_reassigned=None, reassignment_needed=None, on_retired=None):
        self.worker = worker
        self.folder = worker.root / "audits"
        self.folder.mkdir(exist_ok=True)
        self.answers = AuditAnswers(self.folder)
        self.owner_reports = OwnerReports(worker.root)
        # A process may have stopped after scheduling a page but before RPC.
        # Reopen its sweep on restart; retain the durable fair cursor.
        if self.owner_reports.file.exists():
            self.owner_reports.invalidate_sweep()
            self.owner_reports.reopen_for_gold_publications()
        self.owner_poll_more = False
        self.presentation = None
        self.current_audit = None
        self.last_unavailable = None
        self.on_assigned = on_assigned
        self.on_committed = on_committed
        self.before_commit = before_commit
        self.on_reassigned = on_reassigned
        self.reassignment_needed = reassignment_needed
        self.on_retired = on_retired

    def retained_signer(self, script, owner):
        if (type(script) is not str or not script or len(script) > 8192
                or len(script) % 2 or any(c not in '0123456789abcdef' for c in script)):
            raise RetainedAuditPublicationError('Invalid canonical audit signer; private answer retained')
        address = self.worker.rpc.call('decodescript', [script]).get('address')
        if type(address) is not str or not address:
            raise RetainedAuditPublicationError('Canonical audit signer has no supported address; private answer retained')
        if address != owner:
            info = self.worker.rpc.call('getaddressinfo', [address], wallet=True)
            if info.get('ismine') is not True or info.get('scriptPubKey') != script:
                raise RetainedAuditPublicationError('Retained audit signer key is unavailable in this wallet; private answer retained')
        return address

    def event(self, action, request=None, slot=None, attempt=None, body=None, *, preceding_events=None):
        owner = self.worker.address()
        if action == 'reveal' or (action == 'commit' and preceding_events is not None):
            # The miner identity can change after an immutable answer was
            # committed. Recover its canonical signer, never rebind the result
            # or sign on behalf of an address whose key is not in this wallet.
            state = self.state(request)
            job = next((j for j in state.get('jobs', [])
                        if j.get('slot') == slot and j.get('attempt') == attempt), None)
            if not job or (action == 'reveal' and job.get('commitment') != hashlib.sha256(bytes.fromhex(body)).hexdigest()):
                raise RetainedAuditPublicationError('Retained audit no longer matches its canonical commitment; private answer retained')
            script = job.get('actor')
            if script is not None:
                owner = self.retained_signer(script, owner)
        elif action == 'reveal_report':
            # Reports can outlive the selected mining address too. Their
            # original commitment and owning script are chain facts; a current
            # address is never authority to reveal somebody else's report.
            reports = self.worker.rpc.call('getpgnreports', [0, 1, request])
            if (not isinstance(reports, list) or len(reports) != 1
                    or reports[0].get('request_id') != request
                    or reports[0].get('report_root') != hashlib.sha256(bytes.fromhex(body)).hexdigest()):
                raise RetainedAuditPublicationError('Retained owner report no longer matches its canonical commitment; private report retained')
            owner = self.retained_signer(reports[0].get('miner_script'), owner)
        args = [action, owner, request, slot, attempt, body]
        proposal = self.worker.rpc.call("getpgnauditevent", args if preceding_events is None else args + [None, preceding_events])
        if not proposal.get("available"):
            return proposal
        signature = self.worker.rpc.call("signmessage", [owner, proposal["challenge"]], wallet=True)
        return self.worker.rpc.call("getpgnauditevent", args + [signature] + ([] if preceding_events is None else [preceding_events]))

    def apply(self, action, request=None, slot=None, attempt=None, body=None, *, on_intent=None):
        # Identity/body is immutable; physical candidates are parent-bound.
        # Release the lane between attempts so another miner's new block is
        # normal concurrency, not a reason to strand a completed GPU result.
        for attempt_index in range(2):
            try:
                with self.worker.network_lock:
                    proposal = self.event(action, request, slot, attempt, body)
                    if proposal.get("available"):
                        envelopes = [proposal["envelope"]]
                        receipt = (self.worker.ledger(envelopes, on_intent=on_intent)
                                   if on_intent is not None else self.worker.ledger(envelopes))
                        if receipt is not None:
                            proposal = dict(proposal, confirmed_block=receipt["block_hash"])
                    return proposal
            except CoordinationParentChanged:
                if attempt_index:
                    raise  # Durable intent/answer remains available next turn.
                stopping = getattr(self.worker, 'quitting', None)
                running = getattr(self.worker, 'running', None)
                if stopping is not None and stopping.is_set() or running is not None and not running.is_set():
                    raise

    def apply_reveals(self, ready):
        """Publish independent ready answers under one exact parent.

        Every answer already has a durable seal/catalogue entry. The existing
        ledger journal retains the exact candidate across ambiguous sends.
        Nothing is acknowledged here; canonical terminal observation does that.
        """
        if len(ready) > 64 or len({name for name, _ in ready}) != len(ready):
            raise ValueError('Reveal batch exceeds the durable selected page')
        for retry in range(2):
            results = {}
            envelopes = []
            byte_count = 0
            try:
                with self.worker.network_lock:
                    for name, audit in ready:
                        stopping = getattr(self.worker, 'quitting', None)
                        if stopping is not None and stopping.is_set():
                            return {}
                        try:
                            proposal = self.event('reveal', audit['request_id'], audit['slot'],
                                                  audit['attempt'], canonical_bytes(audit['answer']['body']).hex())
                        except RetainedAuditPublicationError as error:
                            results[name] = error
                            continue
                        if not proposal.get('available'):
                            results[name] = proposal
                            continue
                        envelope = proposal['envelope']
                        if not isinstance(envelope, str) or len(envelope) % 2:
                            raise ValueError('Core returned an invalid signed reveal')
                        size = len(envelope) // 2
                        # Leave larger payloads on the ordinary exact single
                        # publication path rather than creating oversized blocks.
                        if byte_count + size > 262144:
                            continue
                        bytes.fromhex(envelope)
                        byte_count += size
                        envelopes.append(envelope)
                        results[name] = proposal
                    if envelopes:
                        receipt = self.worker.ledger(envelopes)
                        block = receipt.get('block_hash') if isinstance(receipt, dict) else None
                        if not isinstance(block, str) or len(block) != 64 or block == '0' * 64 or any(c not in '0123456789abcdef' for c in block):
                            raise RuntimeError('Reveal batch has no canonical block receipt; sealed answers retained')
                        for name, proposal in list(results.items()):
                            if isinstance(proposal, dict) and proposal.get('available'):
                                results[name] = dict(proposal, confirmed_block=receipt['block_hash'])
                    return results
            except CoordinationParentChanged:
                if retry:
                    raise
        raise RuntimeError('Unreachable reveal batch retry state')

    def apply_reveal_group(self, queued):
        """Coalesce ready seals from the two independent physical audit lanes.

        Selected identities stay pending on disk until the next canonical ACK
        sweep. Publication grouping changes neither actor nor audit ownership.
        """
        if len(queued) > 256:
            raise ValueError('Combined reveal selection exceeds four bounded pages')
        for publisher, name, audit in queued:
            if (publisher.worker.rpc is not self.worker.rpc or
                    publisher.worker.network_lock is not self.worker.network_lock):
                raise ValueError('Reveal group crosses authority or publication lane')
        disclosure_errors = []
        if any(audit.get('_ready_owner_disclosure') for _, _, audit in queued):
            from horizon_audit_batch import disclose_local_ready
            queued, disclosure_errors = disclose_local_ready(self, queued)
        for retry in range(2):
            errors = list(disclosure_errors)
            envelopes = []
            seen = set()
            byte_count = 0
            try:
                with self.worker.network_lock:
                    for publisher, name, audit in queued:
                        if self.worker.quitting.is_set():
                            return errors
                        key = (audit['request_id'], audit['slot'], audit['attempt'])
                        # Duplicate retained catalogues refer to one canonical
                        # job. Their own next ACK sweep will retire each copy.
                        if key in seen:
                            continue
                        try:
                            proposal = publisher.event('reveal', *key,
                                canonical_bytes(audit['answer']['body']).hex())
                        except RetainedAuditPublicationError as error:
                            errors.append((publisher, name, error))
                            continue
                        if not proposal.get('available'):
                            continue
                        envelope = proposal['envelope']
                        if not isinstance(envelope, str) or len(envelope) % 2:
                            raise ValueError('Invalid signed shared reveal envelope')
                        size = len(envelope) // 2
                        if len(envelopes) >= 64 or byte_count + size > 262144:
                            continue
                        bytes.fromhex(envelope)
                        byte_count += size
                        seen.add(key)
                        envelopes.append(envelope)
                    if envelopes:
                        receipt = self.worker.ledger(envelopes)
                        block = receipt.get('block_hash') if isinstance(receipt, dict) else None
                        if (not isinstance(block, str) or len(block) != 64 or block == '0' * 64
                                or any(c not in '0123456789abcdef' for c in block)):
                            raise RuntimeError('Shared reveal submission lacks canonical receipt; all seals retained')
                return errors
            except CoordinationParentChanged:
                if retry:
                    raise

    def state(self, request):
        values = self.worker.rpc.call("getpgnauditstate", [0, 1, request])
        return values[0] if values else {}

    def retained_progress_location(self):
        return (
            getattr(self.worker, 'retained_audit_progress', self.folder / 'retained-progress'),
            getattr(getattr(self.worker, 'args', None), 'genesis', None),
        )

    def restore_retired_progress(self, audit):
        root, genesis = self.retained_progress_location()
        return recover_retired_progress(root, genesis, audit)

    def retire_assignment(self, audit, state, anchor, *, archive_conflict=False):
        root, genesis = self.retained_progress_location()
        catalogue_conflict = False
        with self.worker.network_lock:
            if self.worker.rpc.call('getbestblockhash') != anchor:
                raise CoordinationParentChanged('Chain changed during audit retirement; current work retained')
            if audit.get('answer'):
                name = f"{audit['request_id']}-{audit['slot']}-{audit['attempt']}"
                self.answers.check_binding(audit, audit)
                try:
                    retained = self.answers.recover_seal(name, audit)
                except AuditSealBindingMismatch:
                    if not archive_conflict:
                        raise
                    # The caller proved that this cursor is no longer
                    # authorized. Archive it without replacing the other seal.
                else:
                    if retained is None:
                        save(self.folder / (name + '.json'), audit)
                        self.answers.register([name])
                    elif retained['answer'] != audit['answer']:
                        if not archive_conflict:
                            raise ValueError('Current cursor differs from the retained audit seal')
                        catalogue_conflict = True
            return retain_changed_assignment(
                self.folder, audit, state, anchor, self.worker.rpc,
                self.on_retired or (lambda value, block: None),
                checkpoint_root=root, genesis=genesis, archive_conflict=archive_conflict,
                catalogue_conflict=catalogue_conflict,
            )

    def restore_reassigned_answer(self, audit):
        """Recover one exact sealed answer; never scan or recompute a range."""
        name = audit['request_id'] + '-' + str(audit['slot']) + '-' + str(audit['attempt'])
        try:
            audit = self.restore_retired_progress(audit)
        except RetiredAuditSealConflict:
            # The shared checkpoint is ambiguous. An existing, independently
            # recorded seal for this lane still binds the exact assignment.
            cached = self.answers.recover_seal(name, audit, require_durable=True)
            if cached is None:
                raise
        else:
            cached = self.answers.recover_seal(name, audit)
        if cached is None:
            return audit
        if 'target_merit_q16' in audit and audit['target_merit_q16'] != cached.get('target_merit_q16'):
            raise ValueError('Retained audit seal target differs from the active target')
        if audit.get('answer') and audit['answer'] != cached['answer']:
            if audit.get('publication_intent'):
                raise PublicationPending('Audit publication intent and recovered seal disagree; both retained')
            # The recovered answer has the existing catalogue digest or an
            # independent immutable copy. Preserve the mutable copy in case
            # it is later needed to reconcile a network publication.
            with self.worker.network_lock:
                journal = self.folder / 'current.json'
                with journal.open('rb') as stream:
                    raw = stream.read(262145)
                if len(raw) > 262144 or json.loads(raw) != audit:
                    raise PublicationPending('Audit cursor changed during seal reconciliation')
                archive_assignment_cursor(self.folder, raw)
        # A scheduling cursor can lose its answer without a publication intent
        # or cycle callback. The seal is independent of both mutable contexts.
        restored = dict(cached)
        # Only the live journal may supply the intent for this publication.
        restored.pop('publication_intent', None)
        for key, value in audit.items():
            if key not in ('answer', 'counts', 'publication_intent', 'target_merit_q16'):
                restored[key] = value
        if audit.get('answer') == cached['answer'] and audit.get('publication_intent'):
            restored['publication_intent'] = audit['publication_intent']
        save(self.folder / (name + '.json'), restored)
        self.answers.register([name])
        return restored

    def reconcile_reassigned_answer(self, audit):
        if (self.on_reassigned is None or self.reassignment_needed is None
                or not audit.get('publication_intent') or not self.reassignment_needed(audit)):
            return
        intent = audit['publication_intent']
        validate_intent(intent)
        rpc = self.worker.rpc
        before = rpc.call('getbestblockhash')
        state = self.state(audit['request_id'])
        job = next((j for j in state.get('jobs', []) if j.get('slot') == audit['slot'] and j.get('attempt') == audit['attempt']), None)
        if (not job or job.get('state') != 'assigned' or job.get('expired') is True
                or ('actor' in audit and job.get('actor') != audit['actor'])):
            raise RuntimeError('Audit replay requires the exact unexpired reassigned attempt for this actor')
        old = rpc.call('getblockheader', [intent['hash'], True])
        tip = rpc.call('getblockheader', [before, True])
        if (not isinstance(old, dict) or old.get('hash') != intent['hash']
                or type(old.get('height')) is not int or old['height'] < 0
                or type(old.get('confirmations')) is not int
                or not isinstance(tip, dict) or tip.get('hash') != before
                or type(tip.get('height')) is not int or tip['height'] < 0):
            raise ValueError('Audit replay requires verified chain header identities')
        if old['confirmations'] > 0 or (old['height'] <= tip['height'] and rpc.call('getblockhash', [old['height']]) == intent['hash']):
            raise RuntimeError('Canonical audit confirmation cannot be replaced')
        if rpc.call('getbestblockhash') != before:
            raise RuntimeError('Chain changed during audit replay verification')
        self.on_reassigned(audit, intent['hash'])

    def states(self, requests):
        """One authenticated reconstruction per bounded group, pinned to one tip."""
        requests = list(dict.fromkeys(requests))
        if len(requests) > 4096 or any(len(r) != 64 or any(c not in "0123456789abcdef" for c in r) for r in requests):
            raise ValueError("Bounded canonical audit lookups required")
        if not requests:
            return {}
        atomic = getattr(self.worker.rpc, 'audit_snapshot', None)
        if callable(atomic) and len(requests) <= 100:
            # The real Core adapter supplies one authenticated atomic snapshot.
            # Invalid responses propagate; never reinterpret them as legacy data.
            return atomic(requests)
        before = self.worker.rpc.call("getbestblockhash")
        result = {}
        for offset in range(0, len(requests), 100):
            group = requests[offset:offset + 100]
            rows = self.worker.rpc.call("getpgnauditstate", [0, 100, None, group])
            if not isinstance(rows, list) or len(rows) > len(group):
                raise ValueError("Audit lookup returned an oversized page")
            for row in rows:
                request = row.get("request_id")
                if request not in group or request in result:
                    raise ValueError("Audit lookup returned a different or duplicate request")
                result[request] = row
        # A changed branch can be retried later. Never disclose from a mixed view.
        if self.worker.rpc.call("getbestblockhash") != before:
            return None
        return result

    def terminal_on_anchor(self, audit, anchor, states=None):
        commitment = audit.get("answer", {}).get("commitment")
        if not identity(commitment):
            return False
        request = audit["request_id"]
        if isinstance(states, AnchoredAuditStates):
            if states.anchor != anchor:
                return False
            report = states.get(request, {})
        else:
            # The final ACK needs a single native projection. Tip equality
            # around separate reads cannot exclude an intervening reorg.
            view = self.worker.rpc.call("getpgnworkview", ["audits", 0, 1, request])
            state = view.get("state", {})
            rows = view.get("audits", [])
            if (view.get("schema") != "PGN_WORK_VIEW_V1" or view.get("kind") != "audits"
                    or view.get("request_id") != request
                    or state.get("genesis") != anchor["genesis"]
                    or state.get("bestblockhash") != anchor["hash"]
                    or state.get("height") != anchor["height"]
                    or not isinstance(rows, list) or len(rows) != 1
                    or rows[0].get("request_id") != request):
                return False
            report = rows[0]
        verdict = next((job for job in report.get("jobs", [])
                        if job.get("slot") == audit["slot"] and job.get("attempt") == audit["attempt"]), None)
        actor = audit.get("actor")
        return bool(verdict and verdict.get("state") in ("passed", "failed")
                    and verdict.get("commitment") == commitment
                    and (actor is None or verdict.get("actor") == actor)
                    and self.worker.rpc.call("getbestblockhash") == anchor["hash"])

    def reveal_committed(self, *, publication_batch=None):
        # Existing sealed payloads plus a bounded identity-only recovery index.
        index = self.folder / "pending.json"
        if index.is_file() and index.stat().st_size > 1024 * 1024:
            raise ValueError("Pending audit index exceeds budget")
        pending = json.loads(index.read_bytes()) if index.is_file() else []
        if not isinstance(pending, list) or len(pending) > 4096:
            raise ValueError("Pending audit index exceeds its item budget")
        self.answers.restore_pending(pending)
        self.answers.migrate_step()
        if not self.answers.has_answers():
            return
        anchor = self.answers.observe(self.worker.rpc)
        if anchor is None:
            return
        # The legacy file is now a bounded active window, not the backlog.
        # All its identities were durably registered above before replacement.
        # Rotate through the full catalogue even when every older owner waits.
        saved = self.answers.next_page(isolate_failures=True)
        pending = [name for name, _ in saved]
        # Publish recovery before any send: a crash cannot leave its only queue
        # representation in RAM. Catalogue waiting rows independently survive.
        if not index.is_file() or json.loads(index.read_bytes()) != pending:
            save(index, pending)
        states = self.states(audit["request_id"] for _, audit in saved)
        if states is None:
            return
        if isinstance(states, AnchoredAuditStates):
            anchor = states.anchor
        batched = {}
        if publication_batch is None and getattr(self.worker, 'batch_audit_reveals', False):
            ready = []
            for name, audit in saved:
                state = states.get(audit['request_id'], {})
                job = next((j for j in state.get('jobs', [])
                            if j['slot'] == audit['slot'] and j['attempt'] == audit['attempt']), None)
                if job and job['state'] == 'committed' and state.get('details_root'):
                    ready.append((name, audit))
            if len(ready) > 1:
                batched = self.apply_reveals(ready)
        keep = []
        for name, audit in saved:
            stopping = getattr(self.worker, 'quitting', None)
            if stopping is not None and stopping.is_set():
                return  # The full selected window and catalogue stay durable.
            state = states.get(audit["request_id"], {})
            jobs = state.get("jobs", [])
            job = next((j for j in jobs if j["slot"] == audit["slot"] and j["attempt"] == audit["attempt"]), None)
            if not job:
                # A reorganization can temporarily hide this assignment.
                keep.append(name)
                continue
            if job["state"] in ("passed", "failed"):
                if not self.terminal_on_anchor(audit, anchor, states):
                    keep.append(name)
                    continue
                self.answers.acknowledge(name)
                continue
            if job["state"] != "committed" or not state.get("details_root"):
                if (publication_batch is not None and job["state"] == "committed" and len(jobs) == 2
                        and all(j["state"] == "committed" for j in jobs)):
                    publication_batch.append((self, name, dict(audit, _ready_owner_disclosure=True)))
                keep.append(name)
                continue
            if publication_batch is not None:
                publication_batch.append((self, name, audit))
                keep.append(name)
                continue
            try:
                proposal = batched.get(name)
                if isinstance(proposal, RetainedAuditPublicationError):
                    raise proposal
                if proposal is None:
                    proposal = self.apply("reveal", audit["request_id"], audit["slot"], audit["attempt"], canonical_bytes(audit["answer"]["body"]).hex())
            except RetainedAuditPublicationError as error:
                # Preserve this exact commitment and its waiting row. An
                # unavailable historic key must not starve unrelated results
                # in the same bounded publication page. Transport failures
                # still propagate; no successful publication is fabricated.
                self.answers.publication_issues += (dict(name=name, error_type=type(error).__name__, reason=str(error)),)
                keep.append(name)
                continue
            if not proposal.get("available"):
                # The assignment can change between the initial read and send.
                # An unavailable proposal is not a publication acknowledgement.
                keep.append(name)
                continue
            confirmed = self.state(audit["request_id"])
            verdict = next((j for j in confirmed.get("jobs", [])
                            if j["slot"] == audit["slot"] and j["attempt"] == audit["attempt"]), None)
            if not verdict or verdict["state"] not in ("passed", "failed"):
                # Retain the same durable answer across delayed confirmation,
                # reorganization and another attempt's unrelated verdict.
                keep.append(name)
            elif not self.terminal_on_anchor(audit, anchor):
                keep.append(name)
            else:
                self.answers.acknowledge(name)
        if keep != pending:
            save(index, keep)

    def track_owner_report(self, work, sealed):
        self.owner_reports.import_legacy()
        self.owner_reports.register(work["request_id"], dict(ticket=work["ticket"], report_root=sealed["report_root"]))

    def owner_terminal_on_anchor(self, request, anchor, states=None):
        if isinstance(states, AnchoredAuditStates):
            return bool(states.anchor == anchor and states.get(request, {}).get('audited') is True
                        and states.current(self.worker.rpc))
        view = self.worker.rpc.call("getpgnworkview", ["audits", 0, 1, request])
        state, rows = view.get("state", {}), view.get("audits", [])
        return bool(view.get("schema") == "PGN_WORK_VIEW_V1" and view.get("kind") == "audits"
                    and view.get("request_id") == request
                    and state.get("genesis") == anchor["genesis"]
                    and state.get("bestblockhash") == anchor["hash"] and state.get("height") == anchor["height"]
                    and isinstance(rows, list) and len(rows) == 1 and rows[0].get("request_id") == request
                    and rows[0].get("audited") is True
                    and self.worker.rpc.call("getbestblockhash") == anchor["hash"])

    def reveal_owner_reports(self):
        try:
            return self._reveal_owner_reports()
        except Exception:
            self.owner_reports.invalidate_sweep()
            self.owner_poll_more = True
            raise

    def _reveal_owner_reports(self):
        self.owner_poll_more = False
        self.owner_reports.import_legacy()
        if not self.owner_reports.has_reports():
            return
        anchor = self.owner_reports.observe(self.worker.rpc)
        if anchor is None:
            self.owner_poll_more = True
            return
        deferred = dict(self.owner_reports.deferred_publication_turn())
        if deferred:
            self.owner_poll_more = True
            pending = dict(deferred)
            remaining = 64 - len(deferred)
            if remaining:
                fresh, _ = self.owner_reports.next_on_anchor(anchor, limit=min(16, remaining))
                pending.update(fresh)
        else:
            self.owner_reports.retain_publication_turn([])
            page, self.owner_poll_more = self.owner_reports.next_on_anchor(anchor)
            pending = dict(page)
        if not pending:
            return
        states = self.states(pending)
        if states is None:
            self.owner_reports.invalidate_sweep()
            self.owner_poll_more = True
            return
        if isinstance(states, AnchoredAuditStates):
            anchor = states.anchor
        first_error = None
        ready = []
        for request, item in list(pending.items()):
            stopping = getattr(self.worker, 'quitting', None)
            if stopping is not None and stopping.is_set():
                self.owner_reports.invalidate_sweep()
                self.owner_poll_more = True
                return
            try:
                state = states.get(request, {})
                jobs = state.get("jobs", [])
                if not state.get("audited") and (state.get("details_root") or len(jobs) != 2 or any(j["state"] == "assigned" for j in jobs)):
                    continue
                source = self.worker.root / "tickets" / item["ticket"] / "network-report.json"
                if source.stat().st_size > 262144:
                    raise ValueError("Owner report exceeds budget")
                sealed = json.loads(source.read_bytes())
                if (sealed["report_root"] != item["report_root"] or not isinstance(sealed.get("body"), dict)
                        or hashlib.sha256(canonical_bytes(sealed["body"])).hexdigest() != item["report_root"]):
                    raise ValueError("Owner report differs from committed index")
                # This score changes scheduling only. Current Core policies and
                # the exact proof still decide whether a Gold can be accepted.
                merit = max((int(record['gap']) / math.log(int(record['lower_prime']))
                             for record in sealed['body']['computed']['best']), default=0.0)
                ready.append((merit, request, item, sealed, state))
            except Exception as error:
                if first_error is None:
                    first_error = error
        # Open reports release auditors for their next job. Gold publication
        # from already-audited reports only uses the remaining turn budget.
        # Within each category preserve deferred work and use merit as before.
        began, attempts = time.monotonic(), 0
        ready.sort(key=lambda row: (row[4].get("audited") is True, row[1] not in deferred, -row[0]))
        for position, (_, request, item, sealed, state) in enumerate(ready):
            if attempts >= 4 or attempts and time.monotonic()-began >= 5.0:
                # Scheduling never acknowledges a report. Unvisited rows stay
                # waiting in the durable fair catalogue, including on restart.
                self.owner_reports.invalidate_sweep()
                self.owner_reports.retain_publication_turn([(r, i) for _, r, i, _, _ in ready[position:]])
                self.owner_poll_more = True
                break
            attempts += 1
            stopping = getattr(self.worker, 'quitting', None)
            if stopping is not None and stopping.is_set():
                self.owner_reports.invalidate_sweep()
                self.owner_poll_more = True
                return
            try:
                jobs = state.get('jobs', [])
                if not state.get("details_root") and len(jobs) == 2 and all(j["state"] != "assigned" for j in jobs):
                    proposal = self.apply("reveal_report", request, 0, 0, canonical_bytes(sealed["body"]).hex())
                    if not proposal.get("available"):
                        self.owner_reports.invalidate_sweep()
                        self.owner_poll_more = True
                        continue
                    state = self.state(request)
                if state.get("audited"):
                    with self.worker.network_lock:
                        outcome = self.worker.submit_gold(request, sealed, state["details_root"])
                    # A successful Gold publication advances the active chain.
                    # Confirm the terminal report on that new anchor, not the
                    # necessarily obsolete pre-publication tip.
                    terminal_anchor = self.owner_reports.observe(self.worker.rpc) if outcome == "accepted" else anchor
                    if (outcome in ("accepted", "no_gold") and terminal_anchor is not None
                            and self.owner_terminal_on_anchor(request, terminal_anchor,
                                  None if outcome == 'accepted' else states)):
                        self.owner_reports.acknowledge(request, item)
                    else:
                        self.owner_reports.invalidate_sweep()
                        self.owner_poll_more = True
            except Exception as error:
                if first_error is None:
                    first_error = error
        else:
            self.owner_reports.retain_publication_turn([])
        if first_error is not None:
            raise first_error

    def process_one(self):
        self.last_unavailable = None
        self.current_audit = None
        journal = self.folder / "current.json"
        if journal.is_file():
            if journal.stat().st_size > 262144:
                raise ValueError("Current audit exceeds budget")
            audit = json.loads(journal.read_bytes())
            if audit.get("commit_ready") is True:
                return self.resume_grouped_commit(journal, audit)
            # A historic seal belongs only to its exact scientific work.
            # Check the authoritative assignment before touching recovery,
            # so an obsolete cursor cannot block a different live cycle.
            from horizon_audit_batch import ready_audit_view
            native_lane = (getattr(self.worker, 'audit_group_journal', None) is not None
                           and getattr(self.worker, 'audit_prefix_supported', None) is not False)
            if native_lane:
                anchor, current, job = ready_audit_view(self, audit)
            else:
                anchor = self.worker.rpc.call('getbestblockhash') if self.on_retired is not None else None
                current = self.state(audit["request_id"])
                job = next((j for j in current.get("jobs", []) if j["slot"] == audit["slot"] and j["attempt"] == audit["attempt"]), None)
                if job is None:
                    anchor, current, job = ready_audit_view(self, audit, anchor)
            replaced = job is None and assignment_absence_confirmed(audit, current, anchor, self.worker.rpc)
            if job is None and not replaced:
                raise PublicationPending('Audit assignment view incomplete; local work retained')
            expired_or_foreign = job and job['state'] == 'assigned' and (
                job.get('expired') is True
                or (job.get('actor') and audit.get('actor') and job['actor'] != audit['actor'])
                or (job.get('ranges') and job['ranges'] != audit['ranges']))
            if expired_or_foreign or replaced:
                if anchor is None:
                    anchor = self.worker.rpc.call('getbestblockhash')
                self.retire_assignment(audit, current, anchor, archive_conflict=True)
                return 'retired'
            try:
                restored = self.restore_reassigned_answer(audit)
            except (AuditSealBindingMismatch, RetiredAuditSealConflict):
                if self.on_retired is None or not job or job["state"] not in ("passed", "failed"):
                    raise
                # Core finalized this slot. Its verdict cannot authenticate a
                # differently bound local seal or stop the next cycle.
                self.retire_assignment(audit, current, anchor, archive_conflict=True)
                return 'retired'
            if restored != audit:
                save(journal, restored)
            audit = restored
            if not job or job["state"] != "assigned":
                if self.on_committed is not None:
                    if (job and job["state"] in ("committed", "passed", "failed")
                            and "commitment" not in job):
                        raise RuntimeError("PGN Core audit RPC does not expose the committed result digest; update the Core component before resuming; private computation retained")
                    if (not job or job["state"] not in ("committed", "passed", "failed")
                            or job.get("commitment") != audit.get("answer", {}).get("commitment")
                            or not audit.get("publication_intent")):
                        if self.on_retired is not None:
                            self.retire_assignment(audit, current, anchor, archive_conflict=True)
                            return 'retired'
                        raise RuntimeError("Audit assignment changed; private computation retained for reconciliation")
                    # The per-audit intent survives another lane overwriting the
                    # global publication journal before a crash or GUI restart.
                    confirm_candidate(self.worker.rpc, audit["publication_intent"], seconds=0, rebase=True)
                    self.on_committed(audit, audit["publication_intent"]["hash"])
                journal.unlink()
                audit = None
        else:
            audit = None
        if audit is None:
            with self.worker.network_lock:
                if getattr(self.worker, 'audit_group_journal', None) is not None:
                    from horizon_audit_batch import reserve_assignments
                    audit = reserve_assignments(self)
                    if audit is NotImplemented:
                        audit = None
                        proposal = self.apply('assign')
                    else:
                        proposal = dict(available=True) if audit is not None else (self.last_unavailable or dict(available=False))
                else:
                    proposal = self.apply("assign")
                if not proposal.get("available"):
                    self.last_unavailable = proposal
                    recover = getattr(self.worker, 'recover_assigned_audit', None)
                    if proposal.get('unavailable_reason') == 'no_eligible_independent_work' and callable(recover):
                        if getattr(self.worker, 'audit_prefix_supported', False):
                            from horizon_audit_batch import signable_actor
                            if not signable_actor(self.worker, proposal['actor']):
                                self.last_unavailable = dict(proposal, unavailable_reason='signing_key_unavailable',
                                    reason='Wallet signing key unavailable; retained audit work preserved')
                                return 'none'
                        audit, complete = recover(self.folder)
                        if audit is None and not complete:
                            self.last_unavailable = dict(proposal, unavailable_reason='assignment_recovery_pending',
                                reason='Recovering existing assigned audits from the verified chain')
                    if audit is None:
                        return "none"
                elif audit is None:
                    reports = self.worker.rpc.call("getpgnreports", [0, 1, proposal["request_id"]])
                    if len(reports) != 1:
                        raise ValueError("Assigned audit has no immutable report")
                    report = reports[0]
                    audit = dict(schema="PGN_HORIZON_ASSIGNED_AUDIT_V1", request_id=proposal["request_id"], ticket=report["ticket"], report_root=report["report_root"], slot=proposal["slot"], attempt=proposal["attempt"], ranges=proposal["ranges"], counts=[])
                # The Core attribution is already confirmed. Retain its identity
                # before the catalogue can return BUSY/ENOSPC: a retry must resume
                # this assignment, not ask for another and waive its service slot.
                # Seal recovery still precedes any computation or nonce creation.
                audit = self.restore_retired_progress(audit)
                save(journal, audit)
            audit = self.restore_reassigned_answer(audit)
            save(journal, audit)
        elif audit.get('publication_intent') and self.reassignment_needed is not None and self.reassignment_needed(audit):
            audit = self.restore_reassigned_answer(audit)
        self.current_audit = audit
        self.reconcile_reassigned_answer(audit)
        if self.on_assigned is not None:
            self.on_assigned(audit)
            save(journal, audit)
        if "target_merit_q16" not in audit:
            work = self.worker.rpc.call("getpgnscientificwork", [audit["request_id"], audit["report_root"]])
            audit["target_merit_q16"] = encode_target_q16(work)
            save(journal, audit)
        if type(audit["target_merit_q16"]) is not int or not 0 < audit["target_merit_q16"] < 1 << 31:
            raise ValueError("Invalid retained audit target")
        register = getattr(self.worker.source, 'register_preparation', None)
        acknowledge = getattr(self.worker.source, 'acknowledge_preparation', None)
        retain = getattr(self.worker.source, 'retain_audit_partition', None)
        if callable(acknowledge):
            for part in audit['ranges'][:len(audit['counts'])]:
                if callable(retain):retain(audit,part)
                acknowledge(partition_ticket(audit, part)['ticket_id'])
        remaining = audit['ranges'][len(audit['counts']):]
        if remaining and callable(register):
            register([partition_ticket(audit, part) for part in remaining])
        for i in range(len(audit["counts"]), len(audit["ranges"])):
            if not self.worker.running.is_set():
                return "paused"
            part = audit["ranges"][i]
            start, end = int(part["start"]), int(part["end"])
            job = partition_ticket(audit, part)
            identity = job['ticket_id']
            self.presentation = audit_presentation(audit, part, self.worker.root / "tickets" / identity)
            result = self.worker.source.run_ticket(job, self.worker.args.piece_limit)
            if result.get('state') != 'PAUSED':
                finalizer = getattr(self.worker.source, 'finalize_report', None)
                if callable(finalizer):
                    from report_commitment import validate_computed_report
                    work = dict(ticket=job['ticket_id'], start=job['start'],
                                length=str(int(job['end'])-int(job['start'])))
                    result = finalizer(work, result)
                    validate_computed_report(result, work)
            if result.get("state") == "PAUSED":
                return "paused"
            audit["counts"].append(sum(int(n) for n in result["body"]["counts"]))
            save(journal, audit)
            if callable(retain):retain(audit,part,result)
            if callable(acknowledge):
                acknowledge(identity)
        if self.before_commit is not None and not self.before_commit(audit):
            return "paused"
        answer = audit.get("answer")
        if answer is None:
            answer = audit_commitment(audit["ticket"], audit["attempt"], [r["index"] for r in audit["ranges"]], [str(n) for n in audit["counts"]], secrets.token_hex(32))
            audit["answer"] = answer
            save(journal, audit)
        name = audit["request_id"] + "-" + str(audit["slot"]) + "-" + str(audit["attempt"])
        save(self.folder / (name + ".json"), audit)
        # Persist independent recovery before publication, even if the
        # active window is full of older withheld-owner responses.
        self.answers.register([name])
        index = self.folder / "pending.json"
        if index.is_file()and index.stat().st_size>1024*1024:
            raise ValueError("Pending audit index exceeds budget")
        pending = json.loads(index.read_bytes()) if index.is_file() else []
        if not isinstance(pending,list)or len(pending)>4096:
            raise ValueError("Pending audit index exceeds its item budget")
        if name not in pending and len(pending)<64:
            pending.append(name)
        save(index, pending)
        def retain_publication(intent):
            validate_intent(intent)
            audit["publication_intent"] = dict(intent)
            if len(canonical_bytes(audit)) > 262144:
                raise BufferError("Audit publication journal exceeds its budget")
            save(journal, audit)
            save(self.folder / (name + ".json"), audit)
        if getattr(self.worker, 'audit_prefix_supported', False):
            from horizon_audit_batch import AuditReadyStateChanged, commit_ready
            # The calculation barrier is above this block. Once ready, only
            # publish or reconcile the journal under the shared lane lock.
            with self.worker.network_lock:
                audit['commit_ready'] = True
                save(journal, audit)
                try:
                    receipt = commit_ready(self, audit, retain_publication)
                except AuditReadyStateChanged:
                    # Another seal may already be confirmed for this exact
                    # assignment. Resolve it now without recomputing or
                    # waiting for the error loop to revisit the cursor.
                    return self.resume_grouped_commit(journal, audit)
                return self.finish_commit(journal, audit, receipt)
        else:
            receipt = self.apply("commit", audit["request_id"], audit["slot"], audit["attempt"], answer["commitment"],
                                 on_intent=retain_publication if self.on_committed is not None else None)
        return self.finish_commit(journal, audit, receipt)

    def resume_grouped_commit(self, journal, observed):
        from horizon_audit_batch import (commit_ready, identity as audit_identity, prefix_capability,
                                         ready_audit_view, verify_committed_event)
        with self.worker.network_lock:
            audit = json.loads(journal.read_bytes())
            binding = ("ticket", "report_root", "actor", "ranges", "answer")
            if (audit_identity(audit) != audit_identity(observed)
                    or any(audit.get(key) != observed.get(key) for key in binding)
                    or not audit.get("commit_ready") or not audit.get("answer")):
                raise PublicationPending("Ready audit changed during reconciliation; seal retained")
            anchor, state, job = ready_audit_view(self, audit)
            if job is None and not assignment_absence_confirmed(audit, state, anchor, self.worker.rpc):
                raise PublicationPending('Ready audit assignment view incomplete; seal retained')
            bound = (job and audit.get("actor") and job.get("actor") == audit["actor"]
                     and job.get("ranges") == audit["ranges"] and job.get("expired") is False)
            assigned = bound and job["state"] == "assigned" and job.get("actor_policy_accepted") is True
            committed = (bound and job["state"] in ("committed", "passed", "failed")
                         and job.get("commitment") == audit["answer"]["commitment"])
            intent = audit.get("publication_intent") if committed else None
            if committed and not intent and self.on_retired is None:
                raise RetainedAuditPublicationError("Canonical audit has no local block receipt; seal retained")
            if not assigned and not intent:
                if self.on_retired is None:
                    raise PublicationPending("Ready audit is no longer assigned; seal retained")
                # A canonical commitment already satisfied this assignment.
                # Without its exact local receipt, release only the scheduling
                # cursor. Never acknowledge the same answer a second time.
                obsolete = (job is None or
                            job.get("expired") is True or
                            bool(job.get("actor") and job["actor"] != audit.get("actor")) or
                            bool(job.get("ranges") and job["ranges"] != audit["ranges"]) or
                            job["state"] in ("committed", "passed", "failed"))
                self.retire_assignment(audit, state, anchor, archive_conflict=obsolete)
                return "retired"
            if committed:
                verify_committed_event(self.worker.rpc, audit, intent, anchor)
                return self.finish_commit(journal, audit, dict(available=True, confirmed_block=intent["hash"]))
            if assigned:
                if getattr(self.worker, "audit_prefix_supported", None) is None:
                    self.worker.audit_prefix_supported = prefix_capability(self)
                if not self.worker.audit_prefix_supported:
                    raise PublicationPending("Core cannot resume the grouped audit; seal retained")
                self.reconcile_reassigned_answer(audit)
                save(journal, audit)
            return self.finish_commit(journal, audit, commit_ready(self, audit, lambda intent: None))

    def finish_commit(self, journal, audit, receipt):
        if not receipt.get("available"):
            raise RuntimeError("Audit commitment unavailable; private answer retained")
        if self.on_committed is not None:
            if not receipt.get("confirmed_block"):
                raise RuntimeError("Audit confirmation receipt unavailable; private answer retained")
            self.on_committed(audit, receipt["confirmed_block"])
        journal.unlink()
        self.presentation = None
        self.current_audit = None
        return "committed"
