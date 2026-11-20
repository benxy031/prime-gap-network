from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import json
import subprocess
import sys
import tempfile
from test_audit_progress import AuditProgressTests, NetworkWorker
from test_audit_partition_archive import AuditPartitionArchiveTests
from horizon_audit_progress import presentation

def build_frames():
    fixture=AuditProgressTests()
    fixture.setUp()
    worker=NetworkWorker.__new__(NetworkWorker)
    worker.sequence=42
    output=[]
    wire=[]
    worker.emit=wire.append

    def append(name, sample, positions, rows, count=None, boundary=False):
        data=dict(step=name,frame=sample,positions=str(positions) if positions is not None else '',rows=rows)
        if rows:
            data.update(count=count,top=sample['scopes']['ticket']['best'][0]['lower_prime'])
        if boundary:
            data['boundary']=True
        output.append(data)

    def sample(item, count, finished=False):
        result=fixture.result_sample(item,count)
        if finished:
            result['positions']=str(int(item['part_end'])-int(item['part_start']))
            result['phase']='Report ready'
        return result

    def advance(item, value, phase):
        worker.audits=SimpleNamespace(presentation=item)
        if value is None:
            with patch('horizon_network_worker.ticket_frame',side_effect=FileNotFoundError('pending current partition index')):
                worker._present_progress(dict(phase=phase))
        else:
            with patch('horizon_network_worker.ticket_frame',return_value=value):
                worker._present_progress(dict(phase=phase))
        cycle=dict(schema='PGN_MINING_CYCLE_VIEW_V1',request_id='e'*64,
                   active_audit_id=item['audit_id'],active_index=0,ticket_number='42',search_positions='24000',
                   audits=[dict(state='assigned',positions='1500'),dict(state='assigned',positions='1500')])
        return dict(wire[-1],audit_service=dict(active_role='audit',cycle=cycle))

    first=fixture.item(0)
    second=fixture.item(1)
    append('first partition complete',advance(first,sample(first,2,True),'COMPUTED'),100,2,2)
    frame=advance(second,None,'STARTING')
    assert frame['source_stale'] and frame['scopes']['ticket']['statistics']['count_total']==2
    append('second partition missing its first sample',frame,100,2,2,True)
    append('paused at partition boundary',advance(second,None,'PAUSED_ACK'),100,2,2,True)
    partial=sample(second,1)
    append('second partition receives a sample',advance(second,partial,'SEARCHING'),110,3,3)
    append('duplicate sample',advance(second,partial,'SEARCHING'),110,3,3)
    next_audit=fixture.item(0,dict(fixture.audit,attempt=1))
    append('new audit starts empty',advance(next_audit,None,'STARTING'),0,0,boundary=True)
    advance(next_audit,sample(next_audit,1),'SEARCHING')
    disconnected=dict(advance(next_audit,None,'STARTING'),phase='Disconnected')
    append('actual disconnect strips previous audit',disconnected,None,0)
    search=dict(schema='PGN_HORIZON_MINING_VIEW_V1',ticket_id='c'*64,request_id='d'*64,work_role='search',
                phase='Searching',positions='5',positions_total='10',source_stale=False)
    append('new search without audit rows',search,5,0)
    append('stale search strips expired ticket',dict(search,source_stale=True),None,0)
    bad=dict(frame,positions='999999')
    append('invalid audit progress is hidden',bad,None,0)
    append('stale sample outside the boundary is hidden',dict(frame,source_age_ns='99'),None,0)
    return output + archived_handoff_frames()


def archived_handoff_frames():
    fixture = AuditPartitionArchiveTests()
    fixture.setUp()
    try:
        assert fixture.audit.process_one() == 'paused'
        assert fixture.compact()['compacted']
        assert not (fixture.folder / 'session-index.sqlite3').exists()
        end = int(fixture.part['end'])
        next_part = dict(index=1, start=str(end + 10), end=str(end + 20))
        audit = dict(fixture.input, ranges=[fixture.part, next_part])
        first = presentation(audit, fixture.part, fixture.folder)
        second = presentation(audit, next_part, fixture.root / 'pending-partition')
        owner = NetworkWorker.__new__(NetworkWorker)
        owner.root, owner.sequence = fixture.root, 51
        wire = []
        owner.emit = wire.append
        owner.audits = SimpleNamespace(presentation=first)
        with patch('horizon_network_worker.ticket_frame',
                   return_value=dict(fixture.frame, phase='Searching', positions='0')):
            owner._present_progress(dict(phase='SEARCHING'))
        owner.audits.presentation = second
        owner._present_progress(dict(phase='STARTING'))
        owner._present_progress(dict(phase='PAUSED_ACK'))
        steps = []
        for title, frame, rows in zip(
                ('partial view before archive', 'archive supersedes partial view', 'pause keeps final rows'),
                wire, (0, 1, 1), strict=True):
            total, positions = int(frame['positions_total']), int(frame['positions'])
            cycle = dict(schema='PGN_MINING_CYCLE_VIEW_V1', request_id='f'*64,
                         active_audit_id=first['audit_id'], active_index=0, ticket_number='51',
                         search_positions='24000', audits=[dict(state='assigned',positions=str(total))]*2)
            frame['audit_service'] = dict(active_role='audit', cycle=cycle)
            entry = dict(step=title, frame=frame, positions=str(positions), rows=rows,
                         cycle_fraction=0.05 * positions / total, boundary=bool(rows))
            if rows:
                entry.update(count=1, top=frame['scopes']['ticket']['best'][0]['lower_prime'])
            steps.append(entry)
        return steps
    finally:
        fixture.doCleanups()


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Pass the built audit partition widget test executable')
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='pgn-audit-widget-') as work:
        frames = Path(work) / 'frames.json'
        frames.write_text(json.dumps(build_frames(), indent=2), encoding='utf-8')
        subprocess.run([str(executable), str(frames)], check=True, timeout=30)
