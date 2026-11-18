"""Present one assigned audit, while its disjoint engine jobs stay internal."""
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import time
from sqlite3 import Error as SqliteError
from copy import deepcopy
from decimal import Decimal
from report_commitment import canonical_bytes
from horizon_status_stream import size_label


def partition_identity(audit, part):
    return hashlib.sha256(canonical_bytes(dict(domain='PGN_HORIZON_AUDIT_PARTITION_V1',
        request=audit['request_id'], slot=audit['slot'], attempt=audit['attempt'], index=part['index']))).hexdigest()


def presentation(audit, part, folder, title=None):
    ranges = audit['ranges']
    index = ranges.index(part)
    lengths = [int(r['end']) - int(r['start']) for r in ranges]
    if not lengths or min(lengths) <= 0:
        raise ValueError('Invalid assigned audit ranges')
    identity = hashlib.sha256(canonical_bytes(dict(domain='PGN_HORIZON_AUDIT_VIEW_V1',
        request=audit['request_id'], slot=audit['slot'], attempt=audit['attempt']))).hexdigest()
    # Both execution paths visit this assignment in its confirmed range order:
    # NetworkAudits sequentially, or the resident FIFO after prefetched jobs.
    # Offset counts only ranges preceding the active job, never queued work.
    return dict(request_id=audit['request_id'], title=title or 'Audit ' + str(audit['slot'] + 1),
        folder=str(folder), audit_id=identity, audit_slot=audit['slot'], audit_attempt=audit['attempt'],
        offset=str(sum(lengths[:index])), total=str(sum(lengths)),
        part_start=part['start'], part_end=part['end'], part_index=index,
        part_count=len(ranges), final_part=index == len(ranges) - 1,
        part_ticket_id=partition_identity(audit, part),
        prior_partitions=[dict(index=r['index'], start=r['start'], end=r['end']) for r in ranges[:index]])


def retained_scope(ticket_root, part, *, cpu_only=False):
    identity = part['ticket_id']
    if not isinstance(identity, str) or not re.fullmatch('[0-9a-f]{64}', identity):
        raise ValueError('Invalid retained audit partition identity')
    folder = ticket_root / identity
    start, end = part['start'], part['end']
    if cpu_only:
        from horizon_preparation_process import regular
        from pgn_gap_projection import presentation as project_pairs
        state = json.loads(regular(folder / 'cpu-source.json', 1048576))
        configuration = state['configuration']
        if (state.get('schema') != 'PGN_CPU_PROVISIONAL_SOURCE_V1' or state.get('cursor') != end
                or state.get('active') is not None
                or (configuration['ticket_id'], configuration['start'], configuration['end']) != (identity, start, end)):
            raise ValueError('CPU audit partition has no matching complete checkpoint')
        return project_pairs([(int(lo), int(hi)) for lo, hi in state['pairs']], configuration)
    from horizon_status_stream import ticket_frame
    frame = ticket_frame(folder)
    if ((frame['ticket_id'], frame['start'], frame['end']) != (identity, start, end)
            or frame['phase'] != 'Report ready' or int(frame['positions']) != int(end) - int(start)):
        raise ValueError('GPU audit partition has no matching complete checkpoint')
    return frame['scopes']['ticket']


class WholeAuditPresentation:
    """One audit's bounded result views; replace samples instead of counting twice.

    Only source statistics from the disjoint assigned ranges are added. Missing
    boundary samples carry their last result view, never an invented rate.
    This cache is presentation-only and does not change reports or audit counts.
    """
    def __init__(self, ticket_root=None, *, cpu_only=False):
        self.ticket_root = Path(ticket_root) if ticket_root is not None else None
        self.cpu_only = cpu_only
        self.identity = None
        self.parts = {}
        self.metadata = {}
        self.last_progress = None
        self.prior_retry_at = 0

    def restore_prior(self, item):
        if self.ticket_root is None:
            return
        prior = item.get('prior_partitions', [])
        if not isinstance(prior, list) or not 0 < len(prior) == item['part_index'] <= 100:
            return
        pending = [(i, part) for i, part in enumerate(prior) if i not in self.parts]
        now = time.monotonic()
        if not pending or now < self.prior_retry_at:
            return
        self.prior_retry_at = now + 2
        try:
            ordinals = [part['index'] for part in prior]
            bounds = [(int(part['start']), int(part['end'])) for part in prior]
            ranges = sorted(bounds + [(int(item['part_start']), int(item['part_end']))])
            if (any(type(ordinal) is not int or not 0 <= ordinal < 100 for ordinal in ordinals)
                    or len(set(ordinals)) != len(ordinals)
                    or any(not 0 < start < end for start, end in ranges)
                    or any(left[1] > right[0] for left, right in zip(ranges, ranges[1:]))
                    or sum(end - start for start, end in bounds) != int(item['offset'])):
                return
        except (ValueError, TypeError, KeyError):
            return
        audit = dict(request_id=item['request_id'], slot=item['audit_slot'], attempt=item['audit_attempt'])
        for index, part in pending:
            try:
                snapshot = dict(part, ticket_id=partition_identity(audit, part))
                self.parts[index] = retained_scope(self.ticket_root, snapshot, cpu_only=self.cpu_only)
            except (OSError, ValueError, KeyError, TypeError, BufferError, SqliteError):
                # Missing files and archive transitions affect display only.
                # Keep the verified positional prefix and retry bounded reads.
                continue

    def frame(self, item, source=None, *, paused=False):
        if self.identity != item['audit_id']:
            self.identity = item['audit_id']
            self.parts.clear()
            self.metadata.clear()
            self.last_progress = None
            self.prior_retry_at = 0
        value = whole_audit_frame(item, source, paused=paused)
        self.restore_prior(item)
        part = tuple(item[key] for key in ('part_index', 'part_start', 'part_end', 'offset', 'total'))
        if source:
            self.last_progress = part, value['positions']
            for key in ('bits', 'minimum_gap', 'target_merit', 'source_device'):
                if key in source:self.metadata[key] = source[key]
            scope = source.get('scopes', {}).get('ticket')
            if scope:
                self.parts[item['part_index']] = deepcopy(scope)
        elif self.last_progress is not None and self.last_progress[0] == part:
            # A missing read is not a new position. A fresh validated rewind
            # still replaces the retained position when it arrives.
            value['positions'] = self.last_progress[1]
        for key, field in self.metadata.items():value.setdefault(key, field)
        if self.parts:
            scopes = dict(value.get('scopes', {}))
            value['scopes'] = scopes
            scopes['ticket'] = self.aggregate()
        return value

    def aggregate(self):
        scopes = list(self.parts.values())
        statistics = [s['statistics'] for s in scopes]
        stats = {}
        for key in ('count_total', 'at_or_above_target'):
            stats[key] = (sum(s[key] for s in statistics)
                          if all(s.get(key) is not None for s in statistics) else None)
        for key in ('threshold_counts', 'merit_band_counts'):
            if any(s.get(key) is None for s in statistics):stats[key] = None;continue
            counts = {}
            for s in statistics:
                for name, count in s[key].items():counts[name] = counts.get(name, 0) + count
            stats[key] = counts
        targets = {s.get('statistics_target') for s in statistics}
        stats['statistics_target'] = next(iter(targets)) if len(targets) == 1 else None
        stats['mixed_targets'] = len(targets) > 1
        def rows(key):
            unique = {}
            for scope in scopes:
                target = scope['statistics'].get('statistics_target')
                for row in scope.get(key, [])[:100]:
                    value = dict(row)
                    if target is not None:value['target_merit'] = target
                    unique[(value['lower_prime'], value['gap'])] = value
            order = ((lambda r:(Decimal(r['merit']), int(r['gap']), -int(r['lower_prime'])))
                     if key == 'best' else (lambda r:int(r['lower_prime'])))
            return sorted(unique.values(), key=order, reverse=True)[:100]
        return dict(statistics=stats, best=rows('best'), recent=rows('recent'))


def whole_audit_frame(item, source=None, *, paused=False):
    """Offset a validated active-job sample; no fabricated contiguous interval.

    Missing first checkpoints retain the exact completed prefix, with no rates.
    A completed internal job is not a completed audit or a network verdict.
    """
    frame = dict(source or {})
    length = int(item['part_end']) - int(item['part_start'])
    partial = int(frame.get('positions', '0'))
    if not 0 <= partial <= length:
        raise ValueError('Audit progress exceeds its active assigned range')
    expected_ticket = item.get('part_ticket_id')
    if source and (str(frame.get('start')) != item['part_start'] or str(frame.get('end')) != item['part_end']
                   or expected_ticket is not None and frame.get('ticket_id') != expected_ticket):
        raise ValueError('Audit progress belongs to another assigned range')
    done, total = int(item['offset']) + partial, int(item['total'])
    if not 0 <= done <= total:
        raise ValueError('Audit progress exceeds its complete assignment')
    frame.update(schema='PGN_HORIZON_MINING_VIEW_V1', ticket_id=item['audit_id'],
        request_id=item['request_id'], title=item['title'], work_role='audit',
        positions=str(done), positions_total=str(total), positions_label=size_label(total),
        progress_scope='audit', record_pairs=[],
        validation_state='Audit computation; network acceptance remains separate')
    # These coordinates describe the active engine input, not one continuous
    # audit interval. Keep them explicitly scoped, not as whole-audit endpoints.
    frame['audit_active_range'] = dict(start=item['part_start'], end=item['part_end'])
    for key in ('start', 'end', 'current'):
        frame.pop(key, None)
    if not source:
        frame.update(observed_utc=dt.datetime.now(dt.timezone.utc).isoformat(), source_stale=True,
                     source_age_ns=None, speed_rate=None, average_rate=None,
                     compute_tests_rate=None, eta_rate=None)
    complete = done == total and frame.get('phase') == 'Report ready'
    frame['phase'] = 'Paused' if paused else ('Report ready' if complete else 'Searching')
    if paused:
        for key in ('speed_rate', 'average_rate', 'compute_tests_rate', 'eta_rate'):
            frame[key] = None
    return frame
